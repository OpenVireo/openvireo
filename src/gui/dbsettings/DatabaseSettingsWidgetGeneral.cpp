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

#include "DatabaseSettingsWidgetGeneral.h"
#include "ui_DatabaseSettingsWidgetGeneral.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QListView>

#include "core/Clock.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "gui/DatabaseIcons.h"
#include "gui/IconModels.h"
#include "gui/MessageBox.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultDialog.h"
#include "gui/vault/VaultMetrics.h"
#include "gui/vault/VaultSettingsPage.h"
#include "gui/vault/VaultWidgets.h"

#include <components/status_info/InfoBar.h>

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace
{
    constexpr int FieldWidth = 280;
    constexpr int NumberWidth = 120;
    constexpr QSize PickerSize(40, 32);
    constexpr int SwatchSize = 18;

    // Rounded square of the color, outlined so a light color still shows on the card
    QIcon colorSwatch(const QColor& color, qreal devicePixelRatio)
    {
        QPixmap pixmap(QSize(SwatchSize, SwatchSize) * devicePixelRatio);
        pixmap.setDevicePixelRatio(devicePixelRatio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(FluentTheme::palette().strokeStrong, 1));
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(0.5, 0.5, SwatchSize - 1, SwatchSize - 1), 4, 4);
        return QIcon(pixmap);
    }

    constexpr int IconCell = 52;
    constexpr int IconColumns = 8;

    // An icon centred in a rounded cell, tinted and outlined in the accent while picked, as the mockup's grid
    class IconCellDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
        {
            return {IconCell, IconCell};
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto p = FluentTheme::palette();
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const QRectF cell = QRectF(option.rect).adjusted(3, 3, -3, -3);
            QPainterPath path;
            path.addRoundedRect(cell, VaultMetrics::RowRadius, VaultMetrics::RowRadius);
            if (option.state & QStyle::State_Selected) {
                painter->fillPath(path, p.accentSubtle);
                painter->setPen(QPen(p.accent, 2));
                painter->drawPath(path);
            } else if (option.state & QStyle::State_MouseOver) {
                painter->fillPath(path, p.subtleHover);
            }
            // The default icons come as pixmaps
            const auto data = index.data(Qt::DecorationRole);
            const auto icon = data.typeId() == QMetaType::QPixmap ? QIcon(data.value<QPixmap>()) : data.value<QIcon>();
            const QSize size(24, 24);
            const QRect target(option.rect.center() - QPoint(size.width() / 2 - 1, size.height() / 2 - 1), size);
            icon.paint(painter, target);
            painter->restore();
        }
    };
} // namespace

DatabaseSettingsWidgetGeneral::DatabaseSettingsWidgetGeneral(QWidget* parent)
    : DatabaseSettingsWidget(parent)
    , m_ui(new Ui::DatabaseSettingsWidgetGeneral())
{
    m_ui->setupUi(this);

    connect(m_ui->dbPublicColorButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetGeneral::pickPublicColor);
    connect(m_ui->dbPublicColorClearButton, &QPushButton::clicked, this, [this] { setupPublicColorButton({}); });
    connect(m_ui->dbPublicIconButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetGeneral::pickPublicIcon);
    connect(m_ui->dbPublicIconClearButton, &QPushButton::clicked, this, [this] { setupPublicIconButton(-1); });

    connect(m_ui->historyMaxItemsCheckBox, SIGNAL(toggled(bool)), m_ui->historyMaxItemsSpinBox, SLOT(setEnabled(bool)));
    connect(m_ui->historyMaxSizeCheckBox, SIGNAL(toggled(bool)), m_ui->historyMaxSizeSpinBox, SLOT(setEnabled(bool)));
    connect(m_ui->autosaveDelayCheckBox, SIGNAL(toggled(bool)), m_ui->autosaveDelaySpinBox, SLOT(setEnabled(bool)));

    buildVaultPage();
}

void DatabaseSettingsWidgetGeneral::buildVaultPage()
{
    auto ui = m_ui.data();
    // The database settings list already names the page
    VaultSettingsPage page(this, {});

    auto metadata = page.addSection(ui->groupBox);
    // Text fields share one width, so their boxes line up down the cards
    for (auto field : {ui->dbNameEdit, ui->dbDescriptionEdit, ui->defaultUsernameEdit, ui->dbPublicName}) {
        field->setSizePolicy(QSizePolicy::Fixed, field->sizePolicy().verticalPolicy());
        field->setFixedWidth(FieldWidth);
    }
    for (auto spinBox : {ui->historyMaxItemsSpinBox, ui->historyMaxSizeSpinBox, ui->autosaveDelaySpinBox}) {
        spinBox->setFixedWidth(NumberWidth);
    }

    // The color and icon show in their buttons, with a small button to clear them before
    for (auto picker : {ui->dbPublicColorButton, ui->dbPublicIconButton}) {
        picker->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        picker->setFixedSize(PickerSize);
        picker->setIconSize({SwatchSize + 2, SwatchSize + 2});
    }
    for (auto clear : {ui->dbPublicColorClearButton, ui->dbPublicIconClearButton}) {
        const auto text = VaultButton::withoutMnemonic(clear->text());
        clear->setFluentStyle(fluent::basicinput::Button::Subtle);
        clear->setFluentLayout(fluent::basicinput::Button::IconOnly);
        clear->setIconGlyph(Typography::Icons::Dismiss);
        clear->setFixedSize(PickerSize.height(), PickerSize.height());
        clear->setToolTip(text);
        clear->setAccessibleName(text);
    }
    ui->dbPublicIconButton->setToolTip(tr("Publicly visible icon used on the unlock dialog"));

    metadata->addSetting(ui->dbNameLabel, ui->dbNameEdit);
    metadata->addSetting(ui->dbDescriptionLabel, ui->dbDescriptionEdit);
    metadata->addSetting(ui->defaultUsernameLabel, ui->defaultUsernameEdit);

    // The public metadata is readable without unlocking the database, so warn about it as Fluent does
    auto publicMetadata = page.addSection(ui->groupBox_4);
    auto warning = new fluent::status_info::InfoBar(this);
    warning->setObjectName("publicMetadataWarning");
    warning->setSeverity(fluent::status_info::InfoBar::Warning);
    warning->setMessage(ui->warningLabel->text());
    warning->setIsClosable(false);
    warning->setIsOpen(true);
    warning->setSizePolicy(QSizePolicy::Expanding, warning->sizePolicy().verticalPolicy());
    ui->warningLabel->hide();
    publicMetadata->addHeaderWidget(warning, {});
    publicMetadata->addSetting(ui->dbPublicSummaryLabel, ui->dbPublicName, ui->dbPublicName->toolTip());
    publicMetadata->addSetting(ui->dbPublicColorLabel,
                               {ui->dbPublicColorClearButton, ui->dbPublicColorButton},
                               ui->dbPublicColorButton->toolTip());
    publicMetadata->addSetting(ui->dbPublicIconLabel,
                               {ui->dbPublicIconClearButton, ui->dbPublicIconButton},
                               ui->dbPublicIconButton->toolTip());
    setupPublicColorButton({});
    setupPublicIconButton(-1);

    // A switch reads as the setting itself, so the limits lose the colon leading into their count
    for (auto checkBox : {ui->historyMaxItemsCheckBox, ui->historyMaxSizeCheckBox}) {
        auto text = checkBox->text().trimmed();
        if (text.endsWith(QLatin1Char(':'))) {
            text.chop(1);
        }
        checkBox->setText(text);
    }
    auto history = page.addSection(ui->groupBox_2);
    history->addToggle(ui->historyMaxItemsCheckBox)->addHeaderControl(ui->historyMaxItemsSpinBox);
    history->addToggle(ui->historyMaxSizeCheckBox)->addHeaderControl(ui->historyMaxSizeSpinBox);

    auto additional = page.addSection(ui->groupBox_3);
    additional->addToggle(ui->recycleBinEnabledCheckBox);
    additional->addToggle(ui->compressionCheckbox);
    additional->addToggle(ui->autosaveDelayCheckBox)->addHeaderControl(ui->autosaveDelaySpinBox);

    page.finish();
}

DatabaseSettingsWidgetGeneral::~DatabaseSettingsWidgetGeneral() = default;

void DatabaseSettingsWidgetGeneral::initialize()
{
    Metadata* meta = m_db->metadata();

    m_ui->dbNameEdit->setText(meta->name());
    m_ui->dbDescriptionEdit->setText(meta->description());
    m_ui->recycleBinEnabledCheckBox->setChecked(meta->recycleBinEnabled());
    m_ui->defaultUsernameEdit->setText(meta->defaultUserName());
    m_ui->compressionCheckbox->setChecked(m_db->compressionAlgorithm() != Database::CompressionNone);

    m_ui->dbPublicName->setText(m_db->publicName());
    setupPublicColorButton(m_db->publicColor());
    setupPublicIconButton(m_db->publicIcon());

    if (meta->historyMaxItems() > -1) {
        m_ui->historyMaxItemsSpinBox->setValue(meta->historyMaxItems());
        m_ui->historyMaxItemsCheckBox->setChecked(true);
    } else {
        m_ui->historyMaxItemsSpinBox->setValue(Metadata::DefaultHistoryMaxItems);
        m_ui->historyMaxItemsSpinBox->setEnabled(false);
        m_ui->historyMaxItemsCheckBox->setChecked(false);
    }
    int historyMaxSizeMiB = qRound(meta->historyMaxSize() / qreal(1024 * 1024));
    if (historyMaxSizeMiB > 0) {
        m_ui->historyMaxSizeSpinBox->setValue(historyMaxSizeMiB);
        m_ui->historyMaxSizeCheckBox->setChecked(true);
    } else {
        m_ui->historyMaxSizeSpinBox->setValue(qRound(Metadata::DefaultHistoryMaxSize / qreal(1024 * 1024)));
        m_ui->historyMaxSizeSpinBox->setEnabled(false);
        m_ui->historyMaxSizeCheckBox->setChecked(false);
    }
    if (meta->autosaveDelayMin() > 0) {
        m_ui->autosaveDelaySpinBox->setValue(meta->autosaveDelayMin());
        m_ui->autosaveDelayCheckBox->setChecked(true);
    } else {
        m_ui->autosaveDelayCheckBox->setChecked(false);
        m_ui->autosaveDelaySpinBox->setEnabled(false);
    }
}

void DatabaseSettingsWidgetGeneral::uninitialize()
{
}

void DatabaseSettingsWidgetGeneral::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_ui->dbNameEdit->setFocus();
}

bool DatabaseSettingsWidgetGeneral::saveSettings()
{
    auto* meta = m_db->metadata();

    if (!m_ui->recycleBinEnabledCheckBox->isChecked() && meta->recycleBinEnabled()) {
        auto recycleBin = meta->recycleBin();
        if (recycleBin && !recycleBin->isEmpty()) {
            auto result = MessageBox::question(this,
                                               tr("Delete Recycle Bin"),
                                               tr("Do you want to delete the current recycle bin and all its "
                                                  "contents?\nThis action is not reversible."),
                                               MessageBox::Delete | MessageBox::No,
                                               MessageBox::No);

            if (result == MessageBox::Delete) {
                recycleBin->deleteLater();
            } else {
                recycleBin->setName(recycleBin->name().append(tr(" (old)")));
                recycleBin->setIcon(Group::DefaultIconNumber);
            }
        }

        meta->setRecycleBin(nullptr);
    }

    m_db->setCompressionAlgorithm(m_ui->compressionCheckbox->isChecked() ? Database::CompressionGZip
                                                                         : Database::CompressionNone);

    meta->setName(m_ui->dbNameEdit->text());
    meta->setDescription(m_ui->dbDescriptionEdit->text());
    meta->setDefaultUserName(m_ui->defaultUsernameEdit->text());
    meta->setRecycleBinEnabled(m_ui->recycleBinEnabledCheckBox->isChecked());
    meta->setSettingsChanged(Clock::currentDateTimeUtc());

    m_db->setPublicName(m_ui->dbPublicName->text());
    m_db->setPublicColor(m_ui->dbPublicColorButton->property("color").toString());
    m_db->setPublicIcon(m_ui->dbPublicIconButton->property("iconIndex").toInt());

    bool truncate = false;

    int historyMaxItems;
    if (m_ui->historyMaxItemsCheckBox->isChecked()) {
        historyMaxItems = m_ui->historyMaxItemsSpinBox->value();
    } else {
        historyMaxItems = -1;
    }
    if (historyMaxItems != meta->historyMaxItems()) {
        meta->setHistoryMaxItems(historyMaxItems);
        truncate = true;
    }

    int historyMaxSize;
    if (m_ui->historyMaxSizeCheckBox->isChecked()) {
        historyMaxSize = m_ui->historyMaxSizeSpinBox->value() * 1048576;
    } else {
        historyMaxSize = -1;
    }
    if (historyMaxSize != meta->historyMaxSize()) {
        meta->setHistoryMaxSize(historyMaxSize);
        truncate = true;
    }

    int autosaveDelayMin = 0;
    if (m_ui->autosaveDelayCheckBox->isChecked()) {
        autosaveDelayMin = m_ui->autosaveDelaySpinBox->value();
    }
    meta->setAutosaveDelayMin(autosaveDelayMin);

    if (truncate) {
        const QList<Entry*> allEntries = m_db->rootGroup()->entriesRecursive(false);
        for (Entry* entry : allEntries) {
            entry->truncateHistory();
        }
    }

    return true;
}

void DatabaseSettingsWidgetGeneral::pickPublicColor()
{
    auto oldColor = QColor(m_ui->dbPublicColorButton->property("color").toString());
    auto newColor = QColorDialog::getColor(oldColor);
    if (newColor.isValid()) {
        setupPublicColorButton(newColor);
    }
}

void DatabaseSettingsWidgetGeneral::setupPublicColorButton(const QColor& color)
{
    m_ui->dbPublicColorClearButton->setVisible(color.isValid());
    // A Fluent button draws its own face, so the color shows as a swatch in it, or a palette while unset
    m_ui->dbPublicColorButton->setIconGlyph(color.isValid() ? QString() : Typography::Icons::Color);
    m_ui->dbPublicColorButton->setIcon(color.isValid() ? colorSwatch(color, devicePixelRatioF()) : QIcon());
    m_ui->dbPublicColorButton->setProperty("color", color.isValid() ? color.name() : QVariant());
}

void DatabaseSettingsWidgetGeneral::pickPublicIcon()
{
    // A card over the main window: the default icons in a grid on an inset panel, Use no icon on the left
    auto* dialog = new VaultDialog(this);
    dialog->setObjectName("databaseIconDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Database icon"));
    dialog->setNamePrefix(QStringLiteral("vaultDatabaseIcon"));
    dialog->setMaximumDialogWidth(560);

    auto* intro =
        new QLabel(tr("Shown on the unlock screen and the database tab, even while the database is locked."), dialog);
    intro->setWordWrap(true);
    Vault::followTheme(
        intro, [intro] { Vault::styleLabel(intro, Typography::FontRole::Body, FluentTheme::palette().textSecondary); });

    auto* panel = new VaultPanel(VaultPanel::Inset, dialog);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(8, 8, 8, 8);
    auto* iconList = new QListView(panel);
    iconList->setObjectName("databaseIconList");
    iconList->setAccessibleName(tr("Icons"));
    iconList->setViewMode(QListView::IconMode);
    iconList->setFlow(QListView::LeftToRight);
    iconList->setMovement(QListView::Static);
    iconList->setResizeMode(QListView::Adjust);
    iconList->setWrapping(true);
    iconList->setUniformItemSizes(true);
    iconList->setGridSize({IconCell, IconCell});
    iconList->setFrameShape(QFrame::NoFrame);
    iconList->setMouseTracking(true);
    iconList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    iconList->viewport()->setAutoFillBackground(false);
    iconList->setStyleSheet(QStringLiteral("QListView { background: transparent; }"));
    iconList->setItemDelegate(new IconCellDelegate(iconList));
    auto* iconModel = new DefaultIconModel(iconList);
    iconList->setModel(iconModel);
    const int rows = (iconModel->rowCount() + IconColumns - 1) / IconColumns;
    // Eight columns, and up to six rows before it scrolls
    const int scrollBar = rows > 6 ? iconList->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, iconList) : 0;
    iconList->setFixedSize(IconCell * IconColumns + scrollBar + 4, IconCell * qMin(rows, 6) + 4);
    const int current = m_ui->dbPublicIconButton->property("iconIndex").toInt();
    iconList->setCurrentIndex(iconModel->index(current >= 0 ? current : 0, 0));
    panelLayout->addWidget(iconList, 0, Qt::AlignHCenter);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto* use = buttonBox->addButton(tr("Use icon"), QDialogButtonBox::AcceptRole);
    use->setObjectName("databaseIconUse");
    use->setDefault(true);
    auto* none = buttonBox->addButton(tr("Use no icon"), QDialogButtonBox::ResetRole);
    none->setObjectName("databaseIconNone");

    auto* layout = new QVBoxLayout(dialog);
    layout->setSpacing(14);
    layout->addWidget(intro);
    layout->addWidget(panel);
    layout->addWidget(buttonBox);
    dialog->setInitialFocus(iconList);

    connect(iconList, &QListView::doubleClicked, dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(none, &QPushButton::clicked, dialog, [this, dialog] {
        setupPublicIconButton(-1);
        dialog->reject();
    });
    connect(
        dialog, &QDialog::accepted, this, [this, iconList] { setupPublicIconButton(iconList->currentIndex().row()); });
    dialog->open();
}

void DatabaseSettingsWidgetGeneral::setupPublicIconButton(int iconIndex)
{
    auto valid = iconIndex >= 0 && iconIndex < databaseIcons()->count();
    m_ui->dbPublicIconClearButton->setVisible(valid);
    // While unset, the button shows a plus to pick an icon instead of an empty face
    m_ui->dbPublicIconButton->setIconGlyph(valid ? QString() : Typography::Icons::Add);
    if (valid) {
        m_ui->dbPublicIconButton->setIcon(databaseIcons()->icon(iconIndex));
        m_ui->dbPublicIconButton->setProperty("iconIndex", iconIndex);
        m_ui->dbPublicIconClearButton->setVisible(true);
    } else {
        m_ui->dbPublicIconButton->setIcon(QIcon());
        m_ui->dbPublicIconButton->setProperty("iconIndex", -1);
        m_ui->dbPublicIconClearButton->setVisible(false);
    }
}
