/*
 *  Copyright (C) 2024 KeePassXC Team <team@keepassxc.org>
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

#include "BrowserPasskeysConfirmationDialog.h"
#include "ui_BrowserPasskeysConfirmationDialog.h"

#include "core/Entry.h"
#include "gui/GuiTools.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultSettingsPage.h"
#include "gui/vault/VaultWidgets.h"

#include <components/dialogs_flyouts/ContentDialog.h>
#include <components/status_info/ProgressBar.h>
#include <components/textfields/Label.h>

#include <QButtonGroup>
#include <QCloseEvent>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#define STEP 1000

namespace
{
    // Width of the content dialog's card, a little wider than an alert's for the passkey details
    constexpr int VaultDialogWidth = 480;

    fluent::textfields::Label*
    vaultLabel(QWidget* parent, Typography::FontRole font, fluent::textfields::Label::TextColorRole color)
    {
        auto label = new fluent::textfields::Label(parent);
        label->setFluentTypography(font);
        label->setTextColorRole(color);
        label->setWordWrap(true);
        return label;
    }
} // namespace

BrowserPasskeysConfirmationDialog::BrowserPasskeysConfirmationDialog(QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::BrowserPasskeysConfirmationDialog())
    , m_passkeyUpdated(false)
{
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);

    m_ui->setupUi(this);
    m_ui->verticalLayout->setAlignment(Qt::AlignTop);

    connect(m_ui->credentialsTable, SIGNAL(cellDoubleClicked(int, int)), this, SLOT(accept()));
    connect(m_ui->confirmButton, SIGNAL(clicked()), SLOT(accept()));
    connect(m_ui->cancelButton, SIGNAL(clicked()), SLOT(reject()));
    connect(m_ui->updateButton, SIGNAL(clicked()), SLOT(updatePasskey()));

    connect(&m_timer, SIGNAL(timeout()), this, SLOT(updateProgressBar()));
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(updateSeconds()));

    // The browser raises the window before asking, so the dialog can show over it
    if (FluentTheme::vaultLayoutEnabled() && parent && parent->window()->isVisible()) {
        setupVaultLayout();
    }
}

BrowserPasskeysConfirmationDialog::~BrowserPasskeysConfirmationDialog()
{
}

// In the Vault Index layout, the dialog shows as a Fluent content dialog over the window, which provides the title
// and the commands. The site and the user read as Fluent settings cards, the passkeys to choose from as radio buttons
// on a card of their own, and the time left as a Fluent progress bar.
void BrowserPasskeysConfirmationDialog::setupVaultLayout()
{
    using fluent::textfields::Label;
    auto ui = m_ui.data();
    VaultSettingsPage page(this, {});

    m_vaultMessage = vaultLabel(this, Typography::FontRole::Body, Label::TextColorRole::Primary);
    m_vaultMessage->setObjectName("passkeyMessage");
    page.addWidget(m_vaultMessage);

    auto details = page.addSection();
    m_vaultRelyingParty = vaultLabel(this, Typography::FontRole::Body, Label::TextColorRole::Secondary);
    m_vaultRelyingParty->setObjectName("passkeyRelyingParty");
    m_vaultRelyingParty->setTextFormat(Qt::PlainText);
    m_vaultRelyingParty->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addSetting(tr("Relying party"), m_vaultRelyingParty);
    m_vaultUsername = vaultLabel(this, Typography::FontRole::Body, Label::TextColorRole::Secondary);
    m_vaultUsername->setObjectName("passkeyUsername");
    m_vaultUsername->setTextFormat(Qt::PlainText);
    m_vaultUsername->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addSetting(tr("Username"), m_vaultUsername);

    // The passkeys show as choices while there are any to choose from
    auto passkeys = page.addSection(tr("Passkeys"));
    m_vaultHint = vaultLabel(this, Typography::FontRole::Caption, Label::TextColorRole::Secondary);
    m_vaultHint->setObjectName("passkeyHint");
    // Wrapped, the hint gets too little height above the cards; it fits the dialog's width on one line
    m_vaultHint->setWordWrap(false);
    m_vaultHint->hide();
    passkeys->addHeaderWidget(m_vaultHint);
    m_vaultEntryList = new QWidget(this);
    m_vaultEntryList->setObjectName("passkeyEntryList");
    auto entryLayout = new QVBoxLayout(m_vaultEntryList);
    entryLayout->setContentsMargins(0, 0, 0, 0);
    entryLayout->setSpacing(8);
    m_vaultEntryChoices = new QButtonGroup(m_vaultEntryList);
    m_vaultEntryList->hide();
    passkeys->addWidget(m_vaultEntryList);

    auto timeout = new QWidget(this);
    auto timeoutLayout = new QVBoxLayout(timeout);
    timeoutLayout->setContentsMargins(0, 0, 0, 0);
    timeoutLayout->setSpacing(6);
    m_vaultProgress = new fluent::status_info::ProgressBar(timeout);
    m_vaultProgress->setObjectName("passkeyTimeoutProgress");
    // The bar is drawn as wide as it gets, up to its bar width, so it spans the card
    m_vaultProgress->setBarWidth(VaultDialogWidth);
    m_vaultProgress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    timeoutLayout->addWidget(m_vaultProgress);
    m_vaultTimeout = vaultLabel(timeout, Typography::FontRole::Caption, Label::TextColorRole::Secondary);
    m_vaultTimeout->setObjectName("passkeyTimeout");
    m_vaultTimeout->setTextFormat(Qt::RichText);
    timeoutLayout->addWidget(m_vaultTimeout);
    page.addWidget(timeout);

    // The content dialog's commands replace the dialog's buttons and its list
    for (auto widget : QList<QWidget*>{ui->cancelButton, ui->updateButton, ui->confirmButton, ui->credentialsTable}) {
        widget->hide();
    }
    page.finish();

    m_vaultCards = findChild<QWidget*>(objectName() + QStringLiteral("Cards"), Qt::FindDirectChildrenOnly);
    Q_ASSERT(m_vaultCards);
    // The content dialog has margins of its own
    m_vaultCards->layout()->setContentsMargins(0, 0, 0, 0);
}

void BrowserPasskeysConfirmationDialog::setVaultText(const QString& title, const QString& message, const QString& hint)
{
    m_vaultTitle = title;
    m_vaultMessage->setText(message);
    m_vaultHint->setText(hint);
    m_vaultHint->setVisible(!hint.isEmpty());
}

int BrowserPasskeysConfirmationDialog::exec()
{
    return m_vaultCards ? execVaultDialog() : QDialog::exec();
}

int BrowserPasskeysConfirmationDialog::execVaultDialog()
{
    using fluent::dialogs_flyouts::ContentDialog;
    auto window = parentWidget()->window();
    ContentDialog dialog(window);
    dialog.setObjectName("passkeyConfirmationDialog");
    dialog.setThemeSource(window);
    dialog.setTitle(m_vaultTitle);
    dialog.setPrimaryButtonText(m_ui->confirmButton->text());
    // Authenticating offers no update, so the dialog has no secondary command then
    dialog.setSecondaryButtonText(m_ui->updateButton->isHidden() ? QString() : m_ui->updateButton->text());
    dialog.setCloseButtonText(m_ui->cancelButton->text());
    dialog.setDefaultButton(ContentDialog::Primary);
    // The cards' text wraps, so they ask for their height at the width the dialog gives them
    dialog.setContent(new VaultFittedContent(m_vaultCards));
    m_vaultCards->show();
    dialog.resize(VaultDialogWidth + 2 * dialog.shadowSize(), dialog.minimumHeight());
    // Enter presses the primary command; not focused, since the dialog takes focus on opening
    for (auto button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == dialog.primaryButtonText()) {
            button->setDefault(true);
        }
    }

    // Running out of time finishes this dialog, which takes the content dialog with it
    auto finished = connect(this, &QDialog::finished, &dialog, [&dialog](int result) {
        dialog.done(result == QDialog::Accepted ? ContentDialog::ResultPrimary : ContentDialog::ResultNone);
    });
    const int result = dialog.exec();
    disconnect(finished);
    m_timer.stop();

    // Take the cards back, so they outlive the content dialog
    m_vaultCards->setParent(this);
    layout()->addWidget(m_vaultCards);

    if (result == ContentDialog::ResultSecondary) {
        m_passkeyUpdated = true;
    }
    const int answer = result == ContentDialog::ResultNone ? QDialog::Rejected : QDialog::Accepted;
    setResult(answer);
    return answer;
}

void BrowserPasskeysConfirmationDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    GuiTools::centerWidgetOnActiveScreen(this);
}

void BrowserPasskeysConfirmationDialog::registerCredential(const QString& username,
                                                           const QString& relyingParty,
                                                           const QList<Entry*>& existingEntries,
                                                           int timeout)
{
    m_ui->firstLabel->setText(tr("Do you want to register a passkey for:"));
    m_ui->relyingPartyLabel->setText(tr("Relying Party: %1").arg(relyingParty));
    m_ui->usernameLabel->setText(tr("Username: %1").arg(username));
    m_ui->updateButton->setVisible(true);
    m_ui->secondLabel->setText("");

    if (!existingEntries.isEmpty()) {
        m_ui->firstLabel->setText(tr("Existing passkey found.\nDo you want to register a new passkey for:"));
        m_ui->secondLabel->setText(tr("Select the existing passkey and press Update to replace it."));
        m_ui->updateButton->setText(tr("Update"));
        m_ui->confirmButton->setText(tr("Register new"));
        updateEntriesToTable(existingEntries);
        if (m_vaultCards) {
            setVaultText(tr("Existing passkey found"),
                         tr("Do you want to register a new passkey for this site?"),
                         tr("Select the existing passkey and press Update to replace it."));
        }
    } else {
        m_ui->verticalLayout->setSizeConstraint(QLayout::SetFixedSize);
        m_ui->confirmButton->setText(tr("Register"));
        m_ui->updateButton->setText(tr("Add to existing entry"));
        m_ui->credentialsTable->setVisible(false);
        if (m_vaultCards) {
            setVaultText(tr("Register passkey"), tr("Do you want to register a passkey for this site?"));
        }
    }

    if (m_vaultCards) {
        m_vaultRelyingParty->setText(relyingParty);
        m_vaultUsername->setText(username);
    }
    startCounter(timeout);
}

void BrowserPasskeysConfirmationDialog::authenticateCredential(const QList<Entry*>& entries,
                                                               const QString& relyingParty,
                                                               int timeout)
{
    m_ui->firstLabel->setText(tr("Authenticate passkey credentials for:"));
    m_ui->relyingPartyLabel->setText(tr("Relying Party: %1").arg(relyingParty));
    m_ui->usernameLabel->setVisible(false);
    m_ui->updateButton->setVisible(false);
    m_ui->secondLabel->setText("");
    updateEntriesToTable(entries);
    if (m_vaultCards) {
        setVaultText(tr("Authenticate passkey"), tr("Choose the passkey to sign in to this site with."));
        m_vaultRelyingParty->setText(relyingParty);
        m_vaultUsername->hide();
    }
    startCounter(timeout);
}

Entry* BrowserPasskeysConfirmationDialog::getSelectedEntry() const
{
    if (m_vaultEntryChoices) {
        const int checked = m_vaultEntryChoices->checkedId();
        return checked >= 0 ? m_entries.value(checked) : nullptr;
    }
    auto selectedItem = m_ui->credentialsTable->currentItem();
    return selectedItem ? m_entries[selectedItem->row()] : nullptr;
}

bool BrowserPasskeysConfirmationDialog::isPasskeyUpdated() const
{
    return m_passkeyUpdated;
}

void BrowserPasskeysConfirmationDialog::updatePasskey()
{
    m_passkeyUpdated = true;
    emit accept();
}

void BrowserPasskeysConfirmationDialog::updateProgressBar()
{
    if (m_counter < m_ui->progressBar->maximum()) {
        m_ui->progressBar->setValue(m_ui->progressBar->maximum() - m_counter);
        m_ui->progressBar->update();
        if (m_vaultProgress) {
            m_vaultProgress->setRange(0, m_ui->progressBar->maximum());
            m_vaultProgress->setValue(m_ui->progressBar->value());
        }
    } else {
        emit reject();
    }
}

void BrowserPasskeysConfirmationDialog::updateSeconds()
{
    ++m_counter;
    updateTimeoutLabel();
}

void BrowserPasskeysConfirmationDialog::startCounter(int timeout)
{
    m_counter = 0;
    m_ui->progressBar->setMaximum(timeout / STEP);
    updateProgressBar();
    updateTimeoutLabel();
    m_timer.start(STEP);
}

void BrowserPasskeysConfirmationDialog::updateTimeoutLabel()
{
    m_ui->timeoutLabel->setText(tr("Timeout in <b>%n</b> seconds...", "", m_ui->progressBar->maximum() - m_counter));
    if (m_vaultTimeout) {
        m_vaultTimeout->setText(m_ui->timeoutLabel->text());
    }
}

void BrowserPasskeysConfirmationDialog::updateEntriesToTable(const QList<Entry*>& entries)
{
    m_entries = entries;
    m_ui->credentialsTable->setRowCount(entries.count());
    m_ui->credentialsTable->setColumnCount(1);

    int row = 0;
    for (const auto& entry : entries) {
        auto item = new QTableWidgetItem();
        item->setText(entry->title() + " - " + entry->username());
        m_ui->credentialsTable->setItem(row, 0, item);

        if (row == 0) {
            item->setSelected(true);
        }

        ++row;
    }

    m_ui->credentialsTable->resizeColumnsToContents();
    m_ui->credentialsTable->horizontalHeader()->setStretchLastSection(true);

    if (!m_vaultEntryList) {
        return;
    }
    // Each passkey is a choice, the first one picked
    for (auto button : m_vaultEntryChoices->buttons()) {
        m_vaultEntryChoices->removeButton(button);
        delete button;
    }
    for (int i = 0; i < entries.count(); ++i) {
        auto choice = new VaultRadioButton(entries[i]->title() + " - " + entries[i]->username(), m_vaultEntryList);
        choice->setChecked(i == 0);
        m_vaultEntryChoices->addButton(choice, i);
        m_vaultEntryList->layout()->addWidget(choice);
    }
    m_vaultEntryList->setVisible(!entries.isEmpty());
}
