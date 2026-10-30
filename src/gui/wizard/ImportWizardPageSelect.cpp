/*
 *  Copyright (C) 2023 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ImportWizardPageSelect.h"
#include "ui_ImportWizardPageSelect.h"

#include "ImportWizard.h"

#include "account/AccountManager.h"
#include "gui/DatabaseWidget.h"
#include "gui/FileDialog.h"
#include "gui/Icons.h"
#include "gui/MainWindow.h"
#include "gui/MessageBox.h"
#include "gui/remote/RemoteHandler.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/account/AccountController.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultSettingsPage.h"

#include <QDesktopServices>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollArea>
#include <QStyledItemDelegate>

namespace
{
    constexpr int SourceDescriptionRole = Qt::UserRole + 1;
    constexpr int TileHeight = 72;
    constexpr int TileGap = 4;
    constexpr int TileIconSize = 32;
    constexpr int TileMinimumWidth = 240;

    /**
     * Paints a source as a tile with its icon, name and file type. The chosen tile is outlined in the accent
     * color with a filled radio mark, like the steps of the modal.
     */
    class SourceTileDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            Q_UNUSED(index)
            auto view = qobject_cast<const QListView*>(option.widget);
            return view && view->gridSize().isValid() ? view->gridSize() : QSize(TileMinimumWidth, TileHeight);
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto colors = VaultModal::colors();
            const bool selected = option.state & QStyle::State_Selected;
            const bool hovered = option.state & QStyle::State_MouseOver;
            const QRectF tile = QRectF(option.rect).adjusted(TileGap, TileGap, -TileGap, -TileGap);

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            QColor fill = hovered ? colors.hover : Qt::transparent;
            if (selected) {
                fill = colors.selected;
                fill.setAlpha(FluentTheme::isDark() ? 40 : 18);
            }
            painter->setPen(selected ? QPen(colors.selected, 2) : QPen(colors.border, 1));
            painter->setBrush(fill);
            const qreal inset = selected ? 1 : 0.5;
            painter->drawRoundedRect(tile.adjusted(inset, inset, -inset, -inset), 8, 8);

            const QRect icon(
                qRound(tile.left()) + 16, qRound(tile.center().y()) - TileIconSize / 2, TileIconSize, TileIconSize);
            index.data(Qt::DecorationRole).value<QIcon>().paint(painter, icon);

            // Radio mark on the right
            const QRectF mark(tile.right() - 16 - 18, tile.center().y() - 9, 18, 18);
            if (selected) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(colors.selected);
                painter->drawEllipse(mark);
                painter->setBrush(colors.onSelected);
                painter->drawEllipse(mark.center(), 3.5, 3.5);
            } else {
                painter->setPen(QPen(colors.textSecondary, 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawEllipse(mark.adjusted(0.5, 0.5, -0.5, -0.5));
            }

            const int textLeft = icon.right() + 1 + 14;
            const int textWidth = qRound(mark.left()) - 12 - textLeft;
            const auto nameFont = VaultModal::pixelFont(option.font, 14, QFont::DemiBold);
            const auto descriptionFont = VaultModal::pixelFont(option.font, 12);
            const int nameHeight = QFontMetrics(nameFont).height();
            const int descriptionHeight = QFontMetrics(descriptionFont).height();
            const int top = qRound(tile.center().y()) - (nameHeight + 2 + descriptionHeight) / 2;

            painter->setFont(nameFont);
            painter->setPen(colors.text);
            painter->drawText(
                QRect(textLeft, top, textWidth, nameHeight),
                Qt::AlignLeft | Qt::AlignVCenter,
                painter->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textWidth));
            painter->setFont(descriptionFont);
            painter->setPen(colors.textSecondary);
            painter->drawText(QRect(textLeft, top + nameHeight + 2, textWidth, descriptionHeight),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              painter->fontMetrics().elidedText(
                                  index.data(SourceDescriptionRole).toString(), Qt::ElideRight, textWidth));
            painter->restore();
        }
    };

    // Lays the tiles out in as many columns as fit, each filling its share of the width
    class TileGridFitter : public QObject
    {
    public:
        explicit TileGridFitter(QListView* view)
            : QObject(view)
            , m_view(view)
        {
            view->viewport()->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == m_view->viewport() && event->type() == QEvent::Resize) {
                // Size the tiles as if the scroll bar showed, so they fit either way. Sized to the viewport, the
                // scroll bar showing would wrap them onto more rows than fit, and hiding again would unwrap them.
                const int scrollBar = m_view->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_view);
                const int width = m_view->contentsRect().width() - scrollBar;
                const int columns = qMax(1, width / TileMinimumWidth);
                const QSize grid(qMax(1, (width - 1) / columns), TileHeight);
                if (m_view->gridSize() != grid) {
                    m_view->setGridSize(grid);
                }
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        QListView* m_view;
    };

    // Enter on a tile chooses it, as double-clicking does; item views otherwise handle it differently per platform
    class TileActivator : public QObject
    {
    public:
        TileActivator(QListView* view, std::function<void()> activate)
            : QObject(view)
            , m_activate(std::move(activate))
        {
            view->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::KeyPress) {
                const auto key = static_cast<QKeyEvent*>(event)->key();
                if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                    m_activate();
                    return true;
                }
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        std::function<void()> m_activate;
    };
} // namespace

ImportWizardPageSelect::ImportWizardPageSelect(QWidget* parent)
    : QWizardPage(parent)
    , m_ui(new Ui::ImportWizardPageSelect())
{
    m_ui->setupUi(this);

    m_vaultLayout = FluentTheme::vaultLayoutEnabled();

    struct Source
    {
        QString icon;
        QString label;
        // Name and file type on the tiles of the Vault Index layout
        QString name;
        QString description;
        ImportWizard::ImportType type;
    };
    const QList<Source> sources = {
        {"csv",
         tr("Comma Separated Values (.csv)"),
         tr("CSV File"),
         tr("Comma separated values (.csv)"),
         ImportWizard::IMPORT_CSV},
        {"onepassword",
         tr("1Password Export (.1pux)"),
         tr("1Password"),
         tr("Export file (.1pux)"),
         ImportWizard::IMPORT_OPUX},
        {"onepassword",
         tr("1Password Vault (.opvault)"),
         tr("1Password Vault"),
         tr("Vault folder (.opvault)"),
         ImportWizard::IMPORT_OPVAULT},
        {"bitwarden",
         tr("Bitwarden (.json)"),
         tr("Bitwarden"),
         tr("JSON export (.json)"),
         ImportWizard::IMPORT_BITWARDEN},
        {"proton",
         tr("Proton Pass (.json)"),
         tr("Proton Pass"),
         tr("JSON export (.json)"),
         ImportWizard::IMPORT_PROTONPASS},
        {"web",
         tr("Remote Database (.kdbx)"),
         tr("Remote Database"),
         tr("Command download (.kdbx)"),
         ImportWizard::IMPORT_REMOTE},
        {"remote-sync",
         tr("OpenVireo Sync Server (.kdbx)"),
         tr("Sync Server"),
         tr("OpenVireo Sync Server (.kdbx)"),
         ImportWizard::IMPORT_SYNC_SERVER},
        {"database-merge",
         tr("KeePass 2 Database (.kdbx)"),
         tr("KeePass 2"),
         tr("KeePass or OpenVireo database (.kdbx)"),
         ImportWizard::IMPORT_KEEPASS2},
        {"object-locked",
         tr("KeePass 1 Database (.kdb)"),
         tr("KeePass 1"),
         tr("KeePass 1 database (.kdb)"),
         ImportWizard::IMPORT_KEEPASS1},
    };
    for (const auto& source : sources) {
        auto item = new QListWidgetItem(
            icons()->icon(source.icon), m_vaultLayout ? source.name : source.label, m_ui->importTypeList);
        item->setData(Qt::UserRole, source.type);
        item->setData(SourceDescriptionRole, source.description);
        item->setData(Qt::AccessibleTextRole, source.label);
    }

    connect(m_ui->importTypeList, &QListWidget::currentItemChanged, this, &ImportWizardPageSelect::itemSelected);
    m_ui->importTypeList->setCurrentRow(0);

    connect(m_ui->importFileButton, &QAbstractButton::clicked, this, &ImportWizardPageSelect::chooseImportFile);
    connect(m_ui->keyFileButton, &QAbstractButton::clicked, this, &ImportWizardPageSelect::chooseKeyFile);
    connect(m_ui->existingDatabaseRadio, &QRadioButton::toggled, this, [this](bool state) {
        m_ui->existingDatabaseChoice->setEnabled(state);
    });

    updateDatabaseChoices();

    m_ui->downloadCommandHelpButton->setIcon(icons()->icon("system-help"));
    connect(m_ui->downloadCommandHelpButton, &QToolButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl("https://keepassxc.org/docs/KeePassXC_UserGuide#_remote_database_support"));
    });

    connect(m_ui->importFileEdit, &QLineEdit::textChanged, this, &QWizardPage::completeChanged);
    connect(m_ui->downloadCommand, &QLineEdit::textChanged, this, &QWizardPage::completeChanged);
    connect(m_ui->serverUrlEdit, &QLineEdit::textChanged, this, &QWizardPage::completeChanged);
    connect(m_ui->serverTokenEdit, &QLineEdit::textChanged, this, &QWizardPage::completeChanged);
    connect(m_ui->serverDatabaseIdChoice, &QComboBox::currentTextChanged, this, &QWizardPage::completeChanged);
    connect(
        m_ui->serverLoadDatabasesButton, &QPushButton::clicked, this, &ImportWizardPageSelect::loadSyncServerDatabases);

    registerField("ImportType", this);
    registerField("ImportFile", m_ui->importFileEdit);
    registerField("ImportIntoType", m_ui->importIntoGroupBox); // This is intentional
    registerField("ImportInto", m_ui->importIntoLabel); // This is intentional
    registerField("ImportPassword", m_ui->passwordEdit, "text", "textChanged");
    registerField("ImportKeyFile", m_ui->keyFileEdit);
    registerField("DownloadCommand", m_ui->downloadCommand);
    registerField("DownloadInput", m_ui->downloadCommandInput, "plainText", "textChanged");
    registerField("SyncServerUrl", m_ui->serverUrlEdit);
    registerField("SyncServerToken", m_ui->serverTokenEdit);
    registerField("SyncServerDatabaseId", m_ui->serverDatabaseIdChoice, "currentText", "currentTextChanged");

    if (m_vaultLayout) {
        setupVaultLayout();
    }
    setupImportChoices();
    connect(m_ui->existingDatabaseRadio, &QRadioButton::toggled, this, &ImportWizardPageSelect::updateImportChoices);
}

void ImportWizardPageSelect::setupImportChoices()
{
    // Beside the other destinations, so the radio buttons stay exclusive: they share a parent
    auto parent = m_ui->newDatabaseRadio->parentWidget();
    auto layout = qobject_cast<QBoxLayout*>(parent->layout());
    Q_ASSERT(layout);
    auto rowOf = [layout](QWidget* widget) {
        for (int i = 0; i < layout->count(); ++i) {
            auto item = layout->itemAt(i);
            if (item->widget() == widget || (item->layout() && item->layout()->indexOf(widget) >= 0)) {
                return i;
            }
        }
        return layout->count() - 1;
    };

    // Merge or copies, for a .kdbx going into an existing database; a parent of their own keeps them a pair
    m_kdbxModes = new QWidget(parent);
    m_kdbxModes->setObjectName("importKdbxModes");
    auto modes = new QVBoxLayout(m_kdbxModes);
    modes->setContentsMargins(28, 0, 0, 0);
    modes->setSpacing(6);
    m_mergeRadio = new VaultRadioButton(tr("Merge, matching entries by their ID"), m_kdbxModes);
    m_mergeRadio->setObjectName("importMergeRadio");
    m_mergeRadio->setToolTip(tr("A newer version of an entry replaces the older one, which goes into its history. "
                                "Nothing is deleted, so importing the same file again changes nothing."));
    m_mergeRadio->setChecked(true);
    m_copiesRadio = new VaultRadioButton(tr("Add as copies under the selected group"), m_kdbxModes);
    m_copiesRadio->setObjectName("importCopiesRadio");
    m_copiesRadio->setToolTip(tr("The file's groups and entries are added as new copies. Nothing is matched, so "
                                 "importing again adds another copy."));
    modes->addWidget(m_mergeRadio);
    modes->addWidget(m_copiesRadio);
    layout->insertWidget(rowOf(m_ui->existingDatabaseRadio) + 1, m_kdbxModes);

    m_newVaultRadio = new VaultRadioButton(tr("New vault in my account"), parent);
    m_newVaultRadio->setObjectName("newVaultRadio");
    m_vaultOrgChoice = new VaultComboBox(parent);
    m_vaultOrgChoice->setObjectName("newVaultOrgChoice");
    m_vaultOrgChoice->setEnabled(false);
    auto vaultRow = new QHBoxLayout();
    vaultRow->setSpacing(12);
    vaultRow->addWidget(m_newVaultRadio);
    vaultRow->addWidget(m_vaultOrgChoice, 1);
    layout->insertLayout(rowOf(m_ui->newDatabaseRadio) + 1, vaultRow);
    connect(m_newVaultRadio, &QRadioButton::toggled, m_vaultOrgChoice, &QWidget::setEnabled);

    updateImportChoices();
}

void ImportWizardPageSelect::updateImportChoices()
{
    if (!m_newVaultRadio) {
        return;
    }
    auto controller = AccountController::instance();
    auto account = controller ? controller->account() : nullptr;
    const bool signedIn =
        account && account->state() == AccountManager::State::Unlocked && !account->isServiceAccount();

    // Owners and admins can also create a vault in their org
    const auto selected = m_vaultOrgChoice->currentData().toString();
    m_vaultOrgChoice->clear();
    m_vaultOrgChoice->addItem(tr("Personal vault"), QString());
    if (signedIn) {
        for (const auto& org : account->orgs()) {
            if (org.canAdminister()) {
                m_vaultOrgChoice->addItem(org.name, org.id);
            }
        }
    }
    m_vaultOrgChoice->setCurrentIndex(qMax(0, m_vaultOrgChoice->findData(selected)));

    if (!signedIn && m_newVaultRadio->isChecked()) {
        m_ui->newDatabaseRadio->setChecked(true);
    }
    m_newVaultRadio->setVisible(signedIn);
    m_vaultOrgChoice->setVisible(signedIn && m_vaultOrgChoice->count() > 1);
    const bool showModes =
        field("ImportType").toInt() == ImportWizard::IMPORT_KEEPASS2 && m_ui->existingDatabaseRadio->isChecked();
    if (m_kdbxModes->isHidden() == showModes) {
        m_kdbxModes->setVisible(showModes);
        // The settings card around the choices sizes itself from its contents only when asked to
        for (auto widget = m_kdbxModes->parentWidget(); widget && widget != this; widget = widget->parentWidget()) {
            if (widget->layout()) {
                widget->layout()->invalidate();
            }
            widget->updateGeometry();
        }
    }
}

bool ImportWizardPageSelect::importAsCopies() const
{
    return m_copiesRadio && m_copiesRadio->isChecked() && field("ImportType").toInt() == ImportWizard::IMPORT_KEEPASS2;
}

QString ImportWizardPageSelect::vaultOrgId() const
{
    return m_vaultOrgChoice ? m_vaultOrgChoice->currentData().toString() : QString();
}

// In the Vault Index layout, the page is the content of a modal, which names its steps. The sources show as tiles,
// and the file, the credentials and where to import into read as Fluent settings cards, shown as a step of their own.
void ImportWizardPageSelect::setupVaultLayout()
{
    auto ui = m_ui.data();
    setTitle({});

    auto list = ui->importTypeList;
    list->setItemDelegate(new SourceTileDelegate(list));
    list->setViewMode(QListView::IconMode);
    list->setMovement(QListView::Static);
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(true);
    list->setResizeMode(QListView::Adjust);
    list->setSpacing(0);
    list->setFrameShape(QFrame::NoFrame);
    list->setMinimumSize(0, 0);
    // The columns follow the width the list gets, so it mustn't ask for the width of its columns: the form asks
    // for that, and the list growing or shrinking with them would change their number again
    list->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->viewport()->setAttribute(Qt::WA_Hover);
    list->viewport()->setAutoFillBackground(false);
    list->setAccessibleName(tr("Import source"));
    new TileGridFitter(list);
    connect(list, &QListWidget::itemDoubleClicked, this, &ImportWizardPageSelect::sourceActivated);
    new TileActivator(list, [this] { emit sourceActivated(); });
    // The tiles show on their own, outside of the cards
    ui->verticalLayout_2->removeWidget(list);
    list->setParent(this);

    VaultSettingsPage page(this, {});

    auto location = page.addSection(tr("Location"));
    location->addSetting(ui->importFileLabel, {ui->importFileEdit, ui->importFileButton});
    location->addSetting(ui->downloadCommandLabel, {ui->downloadCommand, ui->downloadCommandHelpButton});
    ui->downloadCommandInput->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->downloadCommandInput->setMinimumWidth(320);
    ui->downloadCommandInput->setFixedHeight(120);
    location->addSetting(ui->downloadCommandInputLabel, ui->downloadCommandInput);
    // The sync server's settings show while it is the source
    for (auto card : {location->addSetting(ui->serverUrlLabel, ui->serverUrlEdit),
                      location->addSetting(ui->serverTokenLabel, ui->serverTokenEdit),
                      location->addSetting(ui->serverDatabaseIdLabel,
                                           {ui->serverDatabaseIdChoice, ui->serverLoadDatabasesButton})}) {
        card->follow(ui->syncServerFields);
    }
    ui->serverDatabaseIdChoice->setMinimumWidth(200);

    // Only the sources that are locked ask for them
    auto credentials = page.addSection(tr("Credentials"));
    ui->passwordEdit->setMinimumWidth(240);
    credentials->addSetting(ui->passwordLabel, ui->passwordEdit);
    credentials->addSetting(ui->keyFileLabel, {ui->keyFileEdit, ui->keyFileButton});

    // The choices share a parent, so the radio buttons stay exclusive
    auto destination = page.addSection(tr("Import into"));
    auto choices = new QWidget(this);
    auto choicesLayout = new QVBoxLayout(choices);
    choicesLayout->setContentsMargins(0, 0, 0, 0);
    choicesLayout->setSpacing(8);
    choicesLayout->addWidget(ui->newDatabaseRadio);
    auto existing = new QHBoxLayout();
    existing->setSpacing(12);
    existing->addWidget(ui->existingDatabaseRadio);
    existing->addWidget(ui->existingDatabaseChoice, 1);
    choicesLayout->addLayout(existing);
    choicesLayout->addWidget(ui->temporaryDatabaseRadio);
    ui->existingDatabaseRadio->setText(tr("Existing database"));
    ui->temporaryDatabaseRadio->setText(tr("Temporary database"));
    destination->addWidget(choices);

    page.finish();

    auto cards = findChild<QWidget*>(objectName() + QStringLiteral("Cards"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(cards);
    layout()->removeWidget(cards);
    auto scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(cards);
    cards->setAutoFillBackground(false);
    m_details = scroll;

    layout()->addWidget(list);
    layout()->addWidget(m_details);
    setSourceStep(true);
}

void ImportWizardPageSelect::setSourceStep(bool sourceStep)
{
    if (!m_details) {
        return;
    }
    m_ui->importTypeList->setVisible(sourceStep);
    m_details->setVisible(!sourceStep);
}

QString ImportWizardPageSelect::sourceName() const
{
    auto item = m_ui->importTypeList->currentItem();
    return item ? item->text() : QString();
}

ImportWizardPageSelect::~ImportWizardPageSelect()
{
}

void ImportWizardPageSelect::initializePage()
{
    setField("ImportType", m_ui->importTypeList->currentItem()->data(Qt::UserRole).toInt());
    updateImportChoices();
    if (!m_vaultLayout) {
        adjustSize();
    }
}

bool ImportWizardPageSelect::validatePage()
{
    if (field("ImportType").toInt() == ImportWizard::IMPORT_SYNC_SERVER && !claimPairingCode()) {
        return false;
    }
    if (m_ui->existingDatabaseRadio->isChecked()) {
        if (m_ui->existingDatabaseChoice->currentIndex() == -1) {
            return false;
        }
        setField("ImportIntoType", ImportWizard::EXISTING_DATABASE);
        setField("ImportInto", m_ui->existingDatabaseChoice->currentData());
    } else if (m_newVaultRadio && m_newVaultRadio->isChecked()) {
        setField("ImportIntoType", ImportWizard::NEW_VAULT);
        setField("ImportInto", {});
    } else if (m_ui->temporaryDatabaseRadio->isChecked()) {
        setField("ImportIntoType", ImportWizard::TEMPORARY_DATABASE);
        setField("ImportInto", {});
    } else {
        setField("ImportIntoType", ImportWizard::NEW_DATABASE);
        setField("ImportInto", {});
    }

    return true;
}

bool ImportWizardPageSelect::isComplete() const
{
    if (field("ImportType").toInt() == ImportWizard::IMPORT_REMOTE) {
        return !field("DownloadCommand").toString().isEmpty();
    }
    if (field("ImportType").toInt() == ImportWizard::IMPORT_SYNC_SERVER) {
        return !field("SyncServerUrl").toString().trimmed().isEmpty()
               && !field("SyncServerToken").toString().trimmed().isEmpty()
               && !field("SyncServerDatabaseId").toString().trimmed().isEmpty();
    }
    return !field("ImportFile").toString().isEmpty();
}

void ImportWizardPageSelect::itemSelected(QListWidgetItem* current, QListWidgetItem* previous)
{
    Q_UNUSED(previous)

    if (!current) {
        setCredentialState(false);
        return;
    }

    m_ui->importFileEdit->clear();
    m_ui->passwordEdit->clear();
    m_ui->keyFileEdit->clear();

    auto type = current->data(Qt::UserRole).toInt();
    setField("ImportType", type);
    setSyncServer(false);
    updateImportChoices();
    switch (type) {
    // Unencrypted types
    case ImportWizard::IMPORT_CSV:
    case ImportWizard::IMPORT_OPUX:
    case ImportWizard::IMPORT_PROTONPASS:
        setCredentialState(false);
        setDownloadCommand(false);
        break;
    // Password may be required
    case ImportWizard::IMPORT_BITWARDEN:
    case ImportWizard::IMPORT_OPVAULT:
        setCredentialState(true);
        setDownloadCommand(false);
        break;
    // Password and/or Key File may be required
    case ImportWizard::IMPORT_KEEPASS1:
    case ImportWizard::IMPORT_KEEPASS2:
        setCredentialState(true, true);
        setDownloadCommand(false);
        break;
    case ImportWizard::IMPORT_REMOTE:
        setCredentialState(true, true);
        setDownloadCommand(true);
        break;
    case ImportWizard::IMPORT_SYNC_SERVER:
        setCredentialState(true, true);
        setDownloadCommand(false);
        setSyncServer(true);
        break;
    default:
        Q_ASSERT(false);
    }
}

void ImportWizardPageSelect::updateDatabaseChoices() const
{
    m_ui->existingDatabaseChoice->clear();
    auto mainWindow = getMainWindow();
    if (mainWindow) {
        for (auto dbWidget : mainWindow->getOpenDatabases()) {
            // Remove all connections
            disconnect(dbWidget, nullptr, this, nullptr);

            // Skip over locked databases
            if (dbWidget->isLocked()) {
                continue;
            }

            connect(dbWidget, &DatabaseWidget::databaseLocked, this, &ImportWizardPageSelect::updateDatabaseChoices);
            connect(dbWidget, &DatabaseWidget::databaseModified, this, &ImportWizardPageSelect::updateDatabaseChoices);

            // Enable the selection of an existing database
            m_ui->existingDatabaseRadio->setEnabled(true);
            m_ui->existingDatabaseRadio->setToolTip("");

            // Add a separator between databases
            if (m_ui->existingDatabaseChoice->count() > 0) {
                m_ui->existingDatabaseChoice->insertSeparator(m_ui->existingDatabaseChoice->count());
            }

            // Add the root group as a special line item
            auto db = dbWidget->database();
            m_ui->existingDatabaseChoice->addItem(
                QString("%1 (%2)").arg(dbWidget->displayName(), db->rootGroup()->name()),
                QList<QVariant>() << db->uuid() << db->rootGroup()->uuid());

            if (dbWidget->isVisible()) {
                m_ui->existingDatabaseChoice->setCurrentIndex(m_ui->existingDatabaseChoice->count() - 1);
            }

            // Add remaining groups
            for (const auto& group : db->rootGroup()->groupsRecursive(false)) {
                if (!group->isRecycled()) {
                    auto path = group->hierarchy();
                    path.removeFirst();
                    m_ui->existingDatabaseChoice->addItem(QString("  / %1").arg(path.join(" / ")),
                                                          QList<QVariant>() << db->uuid() << group->uuid());
                }
            }
        }
    }

    if (m_ui->existingDatabaseChoice->count() == 0) {
        m_ui->existingDatabaseRadio->setEnabled(false);
        m_ui->newDatabaseRadio->setChecked(true);
    }
}

void ImportWizardPageSelect::chooseImportFile()
{
    QString file;
#ifndef Q_OS_MACOS
    // OPVault is a folder except on macOS
    if (field("ImportType").toInt() == ImportWizard::IMPORT_OPVAULT) {
        file = fileDialog()->getExistingDirectory(this, tr("Open OPVault"), QDir::homePath());
    } else {
#endif
        file = fileDialog()->getOpenFileName(this, tr("Select import file"), QDir::homePath(), importFileFilter());
#ifndef Q_OS_MACOS
    }
#endif

    if (!file.isEmpty()) {
        m_ui->importFileEdit->setText(file);
    }
}

void ImportWizardPageSelect::chooseKeyFile()
{
    auto filter = QString("%1 (*);;%2 (*.keyx; *.key)").arg(tr("All files"), tr("Key files"));
    auto file = fileDialog()->getOpenFileName(this, tr("Select key file"), QDir::homePath(), filter);
    if (!file.isEmpty()) {
        m_ui->keyFileEdit->setText(file);
    }
}

void ImportWizardPageSelect::setCredentialState(bool passwordEnabled, bool keyFileEnable)
{
    bool passwordStateChanged = m_ui->passwordLabel->isVisible() != passwordEnabled;
    m_ui->passwordLabel->setVisible(passwordEnabled);
    m_ui->passwordEdit->setVisible(passwordEnabled);

    bool keyFileStateChanged = m_ui->keyFileLabel->isVisible() != keyFileEnable;
    m_ui->keyFileLabel->setVisible(keyFileEnable);
    m_ui->keyFileEdit->setVisible(keyFileEnable);
    m_ui->keyFileButton->setVisible(keyFileEnable);

    // Workaround Qt bug where the wizard window is not updated when the internal layout changes. In the Vault
    // Index layout, the window is the main window, which the modal's cards don't size.
    if (window() && !m_vaultLayout) {
        int height = window()->height();
        if (passwordStateChanged) {
            auto diff = m_ui->passwordEdit->height() + m_ui->inputFields->layout()->spacing();
            height += passwordEnabled ? diff : -diff;
        }
        if (keyFileStateChanged) {
            auto diff = m_ui->keyFileEdit->height() + m_ui->inputFields->layout()->spacing();
            height += keyFileEnable ? diff : -diff;
        }
        window()->resize(window()->width(), height);
    }
}

void ImportWizardPageSelect::setDownloadCommand(bool downloadCommandEnabled)
{
    bool downloadCommandStateChanged = m_ui->downloadCommandLabel->isVisible() != downloadCommandEnabled;
    m_ui->downloadCommandLabel->setVisible(downloadCommandEnabled);
    m_ui->downloadCommand->setVisible(downloadCommandEnabled);
    m_ui->downloadCommandInputLabel->setVisible(downloadCommandEnabled);
    m_ui->downloadCommandInput->setVisible(downloadCommandEnabled);
    m_ui->downloadCommandHelpButton->setVisible(downloadCommandEnabled);

    m_ui->temporaryDatabaseRadio->setVisible(downloadCommandEnabled);

    m_ui->importFileLabel->setVisible(!downloadCommandEnabled);
    m_ui->importFileEdit->setVisible(!downloadCommandEnabled);
    m_ui->importFileButton->setVisible(!downloadCommandEnabled);

    // Workaround Qt bug where the wizard window is not updated when the internal layout changes
    if (window() && !m_vaultLayout) {
        int height = window()->height();
        if (downloadCommandStateChanged) {
            auto diff = m_ui->downloadCommand->height() + m_ui->downloadCommandInput->height()
                        + m_ui->temporaryDatabaseRadio->height() + m_ui->inputFields->layout()->spacing();
            height += downloadCommandEnabled ? diff : -diff;
        }
        window()->resize(window()->width(), height);
    }
}

void ImportWizardPageSelect::setSyncServer(bool syncServerEnabled)
{
    bool stateChanged = m_ui->syncServerFields->isVisible() != syncServerEnabled;
    m_ui->syncServerFields->setVisible(syncServerEnabled);
    if (syncServerEnabled) {
        m_ui->importFileLabel->setVisible(false);
        m_ui->importFileEdit->setVisible(false);
        m_ui->importFileButton->setVisible(false);
        m_ui->temporaryDatabaseRadio->setVisible(true);
    }
    // The server copy keeps its own credentials, so a "new" database is really a local copy of it
    if (m_vaultLayout) {
        m_ui->newDatabaseRadio->setText(syncServerEnabled ? tr("Local copy (keeps the server database's credentials)")
                                                          : tr("New database"));
    } else {
        m_ui->newDatabaseRadio->setText(syncServerEnabled ? tr("Local Copy (keeps the server database's credentials)")
                                                          : tr("New Database"));
    }

    // Workaround Qt bug where the wizard window is not updated when the internal layout changes
    if (window() && stateChanged && !m_vaultLayout) {
        auto diff = m_ui->syncServerFields->sizeHint().height() + m_ui->inputFields->layout()->spacing();
        window()->resize(window()->width(), window()->height() + (syncServerEnabled ? diff : -diff));
    }
}

void ImportWizardPageSelect::loadSyncServerDatabases()
{
    RemoteParams params;
    params.type = RemoteParams::Type::SyncServer;
    params.serverUrl = m_ui->serverUrlEdit->text().trimmed();
    params.serverToken = m_ui->serverTokenEdit->text().trimmed();
    if (params.serverUrl.isEmpty() || params.serverToken.isEmpty()) {
        MessageBox::warning(this, tr("Sync Server"), tr("Enter the server URL and access token first."));
        return;
    }

    if (!claimPairingCode()) {
        return;
    }
    params.serverToken = m_ui->serverTokenEdit->text().trimmed();

    RemoteHandler handler;
    auto listing = handler.listSyncServerDatabases(&params);
    if (!listing.success) {
        MessageBox::warning(this, tr("Sync Server"), listing.errorMessage);
        return;
    }
    if (listing.databaseIds.isEmpty()) {
        MessageBox::information(this, tr("Sync Server"), tr("There are no databases on the server yet."));
        return;
    }

    auto current = m_ui->serverDatabaseIdChoice->currentText();
    m_ui->serverDatabaseIdChoice->clear();
    m_ui->serverDatabaseIdChoice->addItems(listing.databaseIds);
    if (!current.isEmpty() && listing.databaseIds.contains(current)) {
        m_ui->serverDatabaseIdChoice->setCurrentText(current);
    }
}

bool ImportWizardPageSelect::claimPairingCode()
{
    const auto code = m_ui->serverTokenEdit->text().trimmed();
    if (!SyncServerClient::isPairingCode(code)) {
        return true;
    }
    RemoteParams params;
    params.type = RemoteParams::Type::SyncServer;
    params.serverUrl = m_ui->serverUrlEdit->text().trimmed();
    RemoteHandler handler;
    auto pairing = handler.claimSyncServerPairingCode(&params, code);
    if (!pairing.success) {
        MessageBox::warning(this, tr("Sync Server"), tr("Could not pair this device: %1").arg(pairing.errorMessage));
        return false;
    }
    // From here on the field holds this device's own token, which the import stores on this device
    m_ui->serverTokenEdit->setText(pairing.token);
    return true;
}

QString ImportWizardPageSelect::importFileFilter()
{
    switch (field("ImportType").toInt()) {
    case ImportWizard::IMPORT_CSV:
        return QString("%1 (*.csv);;%2 (*)").arg(tr("Comma Separated Values"), tr("All files"));
    case ImportWizard::IMPORT_OPUX:
        return QString("%1 (*.1pux)").arg(tr("1Password Export"));
    case ImportWizard::IMPORT_BITWARDEN:
        return QString("%1 (*.json)").arg(tr("Bitwarden JSON Export"));
    case ImportWizard::IMPORT_PROTONPASS:
        return QString("%1 (*.json)").arg(tr("Proton Pass JSON Export"));
    case ImportWizard::IMPORT_OPVAULT:
        return QString("%1 (*.opvault)").arg(tr("1Password Vault"));
    case ImportWizard::IMPORT_KEEPASS1:
        return QString("%1 (*.kdb)").arg(tr("KeePass1 Database"));
    case ImportWizard::IMPORT_KEEPASS2:
        return QString("%1 (*.kdbx);;%2 (*)").arg(tr("KeePass 2 Database"), tr("All files"));
    default:
        return {};
    }
}
