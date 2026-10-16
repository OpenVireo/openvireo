/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2013 Francois Ferrand
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "BrowserAccessControlDialog.h"
#include "ui_BrowserAccessControlDialog.h"
#include <QUrl>

#include "core/Entry.h"
#include "gui/GuiTools.h"
#include "gui/Icons.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultSettingsPage.h"
#include "gui/vault/VaultWidgets.h"

#include <components/dialogs_flyouts/ContentDialog.h>
#include <components/textfields/Label.h>

#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{
    // Width of the content dialog's card, a little wider than an alert's for the entries
    constexpr int VaultDialogWidth = 480;
} // namespace

BrowserAccessControlDialog::BrowserAccessControlDialog(QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::BrowserAccessControlDialog())
{
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);

    m_ui->setupUi(this);

    connect(m_ui->allowButton, SIGNAL(clicked()), SLOT(accept()));
    connect(m_ui->denyButton, SIGNAL(clicked()), SLOT(reject()));
    connect(m_ui->itemsTable, SIGNAL(cellDoubleClicked(int, int)), this, SLOT(accept()));
    connect(m_ui->itemsTable->selectionModel(),
            SIGNAL(selectionChanged(QItemSelection, QItemSelection)),
            this,
            SLOT(selectionChanged()));
    connect(m_ui->itemsTable, SIGNAL(acceptSelections()), SLOT(accept()));
    connect(m_ui->itemsTable, SIGNAL(focusInWithoutSelections()), this, SLOT(selectionChanged()));

    // The browser brings the window forward before asking, when the dialog can show over it
    if (FluentTheme::vaultLayoutEnabled() && parent && parent->window()->isVisible()
        && !parent->window()->isMinimized()) {
        setupVaultLayout();
    }
}

BrowserAccessControlDialog::~BrowserAccessControlDialog()
{
}

// In the Vault Index layout, the dialog shows as a Fluent content dialog over the window, which provides the title
// and the commands. Each entry is a check box on a card, with a button that disables it for the site, and remembering
// the decision is a toggle card. The table stays out of sight and keeps the selection, so the rest of the dialog's
// code works as it did.
void BrowserAccessControlDialog::setupVaultLayout()
{
    using fluent::textfields::Label;
    auto ui = m_ui.data();
    VaultSettingsPage page(this, {});

    m_vaultMessage = new Label(this);
    m_vaultMessage->setObjectName("accessMessage");
    m_vaultMessage->setFluentTypography(Typography::FontRole::Body);
    m_vaultMessage->setTextColorRole(Label::TextColorRole::Primary);
    m_vaultMessage->setTextFormat(Qt::PlainText);
    m_vaultMessage->setWordWrap(true);
    page.addWidget(m_vaultMessage);

    auto entries = page.addSection(tr("Entries"));
    m_vaultEntryList = new QWidget(this);
    m_vaultEntryList->setObjectName("accessEntryList");
    auto entryLayout = new QVBoxLayout(m_vaultEntryList);
    entryLayout->setContentsMargins(0, 0, 0, 0);
    entryLayout->setSpacing(4);
    entries->addWidget(m_vaultEntryList);

    auto decision = page.addSection();
    decision->addToggle(ui->rememberDecisionCheckBox,
                        tr("Allow the checked entries and deny the others for this site without asking again"));

    // The content dialog's commands replace the dialog's buttons, and the check boxes its table
    for (auto widget : QList<QWidget*>{ui->siteLabel, ui->allowButton, ui->denyButton, ui->itemsTable}) {
        widget->hide();
    }
    page.finish();

    m_vaultCards = findChild<QWidget*>(objectName() + QStringLiteral("Cards"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(m_vaultCards);
    // The content dialog has margins of its own
    m_vaultCards->layout()->setContentsMargins(0, 0, 0, 0);
}

void BrowserAccessControlDialog::addVaultEntryRow(int row)
{
    using fluent::textfields::Label;
    auto rowWidget = new QWidget(m_vaultEntryList);
    auto layout = new QHBoxLayout(rowWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto title = m_ui->itemsTable->item(row, 0);
    auto choice = new VaultCheckBox(title->text(), rowWidget);
    choice->setObjectName(QStringLiteral("accessEntryChoice%1").arg(row));
    choice->setToolTip(title->toolTip());
    layout->addWidget(choice);

    auto username = new Label(rowWidget);
    username->setFluentTypography(Typography::FontRole::Body);
    username->setTextColorRole(Label::TextColorRole::Secondary);
    username->setTextFormat(Qt::PlainText);
    username->setText(m_ui->itemsTable->item(row, 1)->text());
    layout->addWidget(username, 1);

    auto disable = new VaultButton(rowWidget);
    disable->setObjectName(QStringLiteral("accessEntryDisable%1").arg(row));
    disable->setFluentStyle(fluent::basicinput::Button::Subtle);
    layout->addWidget(disable);
    m_vaultEntryList->layout()->addWidget(rowWidget);
    m_vaultEntryRows.append({choice, username, disable});

    // Checking an entry selects its row of the table, which is what allowing goes by
    connect(choice, &QAbstractButton::toggled, this, [this, row](bool checked) {
        for (int column = 0; column < 2; ++column) {
            m_ui->itemsTable->item(row, column)->setSelected(checked);
        }
        emit vaultChoicesChanged();
    });
    // The table's button disables the entry or brings it back
    connect(disable, &QAbstractButton::clicked, this, [this, row] {
        if (auto button = qobject_cast<QAbstractButton*>(m_ui->itemsTable->cellWidget(row, 2))) {
            emit button->pressed();
        }
        syncVaultEntryRow(row);
        emit vaultChoicesChanged();
    });
    syncVaultEntryRow(row);
}

void BrowserAccessControlDialog::syncVaultEntryRow(int row)
{
    const auto& vaultRow = m_vaultEntryRows[row];
    const auto title = m_ui->itemsTable->item(row, 0);
    const bool disabled = title->flags() == Qt::NoItemFlags;

    {
        QSignalBlocker blocker(vaultRow.choice);
        vaultRow.choice->setChecked(!disabled && title->isSelected());
    }
    vaultRow.choice->setEnabled(!disabled);
    for (QWidget* widget : {static_cast<QWidget*>(vaultRow.choice), static_cast<QWidget*>(vaultRow.username)}) {
        auto font = widget->font();
        font.setStrikeOut(disabled);
        widget->setFont(font);
    }

    auto button = qobject_cast<QAbstractButton*>(m_ui->itemsTable->cellWidget(row, 2));
    vaultRow.disable->setIcon(button->icon());
    vaultRow.disable->setToolTip(button->toolTip());
    vaultRow.disable->setAccessibleName(button->toolTip());
}

bool BrowserAccessControlDialog::hasVaultChoice() const
{
    for (const auto& row : m_vaultEntryRows) {
        if (row.choice->isEnabled() && row.choice->isChecked()) {
            return true;
        }
    }
    return false;
}

int BrowserAccessControlDialog::exec()
{
    return m_vaultCards ? execVaultDialog() : QDialog::exec();
}

int BrowserAccessControlDialog::execVaultDialog()
{
    using fluent::dialogs_flyouts::ContentDialog;
    auto window = parentWidget()->window();
    ContentDialog dialog(window);
    dialog.setObjectName("accessControlDialog");
    dialog.setThemeSource(window);
    dialog.setTitle(tr("Allow access?"));
    dialog.setPrimaryButtonText(tr("Allow"));
    dialog.setCloseButtonText(m_ui->denyButton->text());
    dialog.setDefaultButton(ContentDialog::Primary);
    // The cards' text wraps, so they ask for their height at the width the dialog gives them
    dialog.setContent(new VaultFittedContent(m_vaultCards));
    m_vaultCards->show();
    dialog.resize(VaultDialogWidth + 2 * dialog.shadowSize(), dialog.minimumHeight());

    // Allowing needs an entry to allow
    QAbstractButton* allow = nullptr;
    for (auto button : dialog.findChildren<fluent::basicinput::Button*>()) {
        if (button->text() == dialog.primaryButtonText()) {
            allow = button;
        }
    }
    Q_ASSERT(allow);
    // Enter presses it; not focused, since the dialog takes focus on opening
    if (auto button = qobject_cast<QPushButton*>(allow)) {
        button->setDefault(true);
    }
    auto updateAllow = [this, allow] { allow->setEnabled(hasVaultChoice()); };
    auto choicesChanged = connect(this, &BrowserAccessControlDialog::vaultChoicesChanged, &dialog, updateAllow);
    updateAllow();

    // Locking the database finishes this dialog, which takes the content dialog with it
    auto finished = connect(this, &QDialog::finished, &dialog, [&dialog](int result) {
        dialog.done(result == QDialog::Accepted ? ContentDialog::ResultPrimary : ContentDialog::ResultNone);
    });
    const int result = dialog.exec();
    disconnect(finished);
    disconnect(choicesChanged);

    // Take the cards back, so they outlive the content dialog
    m_vaultCards->setParent(this);
    layout()->addWidget(m_vaultCards);

    const int answer = result == ContentDialog::ResultPrimary ? QDialog::Accepted : QDialog::Rejected;
    setResult(answer);
    return answer;
}

void BrowserAccessControlDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    GuiTools::centerWidgetOnActiveScreen(this);
}

void BrowserAccessControlDialog::setEntries(const QList<Entry*>& entriesToConfirm,
                                            const QString& urlString,
                                            bool httpAuth)
{
    QUrl url(urlString);
    m_ui->siteLabel->setText(m_ui->siteLabel->text().arg(
        url.toDisplayString(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment)));
    m_ui->siteLabel->setToolTip(urlString);

    m_ui->rememberDecisionCheckBox->setVisible(!httpAuth);
    m_ui->rememberDecisionCheckBox->setChecked(false);

    m_ui->itemsTable->setRowCount(entriesToConfirm.count());
    m_ui->itemsTable->setColumnCount(3);

    int row = 0;
    for (const auto& entry : entriesToConfirm) {
        addEntryToList(entry, row);
        ++row;
    }

    m_ui->itemsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_ui->itemsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_ui->itemsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_ui->itemsTable->selectAll();
    m_ui->allowButton->setFocus();

    if (m_vaultCards) {
        m_vaultMessage->setText(m_ui->siteLabel->text());
        for (int i = 0; i < entriesToConfirm.count(); ++i) {
            addVaultEntryRow(i);
        }
    }
}

void BrowserAccessControlDialog::addEntryToList(const Entry* entry, int row)
{
    const auto titleItem = new QTableWidgetItem();
    const auto entryTitle = entry->resolveMultiplePlaceholders(entry->title());
    const auto entryUrl = entry->resolveMultiplePlaceholders(entry->url());
    titleItem->setText(entryTitle);
    titleItem->setToolTip(entryUrl);
    titleItem->setData(Qt::UserRole, row);
    titleItem->setFlags(titleItem->flags() | Qt::ItemIsSelectable);
    m_ui->itemsTable->setItem(row, 0, titleItem);

    const auto usernameItem = new QTableWidgetItem();
    const auto entryUsername = entry->resolveMultiplePlaceholders(entry->username());
    usernameItem->setText(entryUsername);
    usernameItem->setData(Qt::UserRole, row);
    m_ui->itemsTable->setItem(row, 1, usernameItem);

    auto disableButton = new QPushButton();
    disableButton->setIcon(icons()->icon("entry-delete"));
    disableButton->setToolTip(tr("Disable for this site"));

    connect(disableButton, &QAbstractButton::pressed, [&, titleItem, usernameItem, disableButton] {
        auto font = titleItem->font();
        if (titleItem->flags() == Qt::NoItemFlags) {
            titleItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            usernameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            titleItem->setSelected(true);
            usernameItem->setSelected(true);

            font.setStrikeOut(false);
            titleItem->setFont(font);
            usernameItem->setFont(font);

            disableButton->setIcon(icons()->icon("entry-delete"));
            disableButton->setToolTip(tr("Disable for this site"));
            m_ui->rememberDecisionCheckBox->setEnabled(true);
        } else {
            titleItem->setFlags(Qt::NoItemFlags);
            usernameItem->setFlags(Qt::NoItemFlags);
            titleItem->setSelected(false);
            usernameItem->setSelected(false);

            font.setStrikeOut(true);
            titleItem->setFont(font);
            usernameItem->setFont(font);

            disableButton->setIcon(icons()->icon("entry-restore"));
            disableButton->setToolTip(tr("Undo"));

            // Disable Remember checkbox if all items are disabled
            auto areAllDisabled = BrowserAccessControlDialog::areAllDisabled();
            m_ui->rememberDecisionCheckBox->setEnabled(!areAllDisabled);
        }
    });

    m_ui->itemsTable->setCellWidget(row, 2, disableButton);
}

bool BrowserAccessControlDialog::remember() const
{
    return m_ui->rememberDecisionCheckBox->isChecked();
}

QList<QTableWidgetItem*> BrowserAccessControlDialog::getEntries(SelectionType selectionType) const
{
    QList<QTableWidgetItem*> selected;
    for (auto& item : getAllItems()) {
        // Add to list depending on selection type and item status
        if ((selectionType == SelectionType::Selected && item->isSelected())
            || (selectionType == SelectionType::NonSelected && !item->isSelected())
            || (selectionType == SelectionType::Disabled && item->flags() == Qt::NoItemFlags)) {
            selected.append(item);
        }
    }
    return selected;
}

void BrowserAccessControlDialog::selectionChanged()
{
    auto selectedRows = m_ui->itemsTable->selectionModel()->selectedRows();

    m_ui->allowButton->setEnabled(!selectedRows.isEmpty());
    m_ui->allowButton->setDefault(!selectedRows.isEmpty());
    m_ui->allowButton->setAutoDefault(!selectedRows.isEmpty());

    if (selectedRows.isEmpty()) {
        m_ui->allowButton->clearFocus();
        m_ui->denyButton->setFocus();
    }
}

bool BrowserAccessControlDialog::areAllDisabled() const
{
    for (const auto& item : getAllItems()) {
        if (item->flags() != Qt::NoItemFlags) {
            return false;
        }
    }

    return true;
}

QList<QTableWidgetItem*> BrowserAccessControlDialog::getAllItems() const
{
    QList<QTableWidgetItem*> items;
    for (int i = 0; i < m_ui->itemsTable->rowCount(); ++i) {
        auto item = m_ui->itemsTable->item(i, 0);
        items.append(item);
    }
    return items;
}
