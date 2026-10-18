/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
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

#include "ShareVaultDialog.h"
#include "ui_ShareVaultDialog.h"

#include "account/AccountManager.h"
#include "account/VaultKeyring.h"
#include "gui/MessageBox.h"

#include <QFontDatabase>
#include <QHeaderView>
#include <QPushButton>

namespace
{
    constexpr int PrincipalRole = Qt::UserRole;

    QString roleText(const QString& role)
    {
        if (role == "manage") {
            return ShareVaultDialog::tr("Can manage");
        }
        if (role == "write") {
            return ShareVaultDialog::tr("Can edit");
        }
        return ShareVaultDialog::tr("Read only");
    }

    void fillRoles(QComboBox* combo)
    {
        combo->clear();
        for (const auto& role : VaultSharing::roles()) {
            combo->addItem(roleText(role), role);
        }
    }
} // namespace

ShareVaultDialog::ShareVaultDialog(VaultSharing* sharing, const QString& vaultId, QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::ShareVaultDialog())
    , m_sharing(sharing)
    , m_vaultId(vaultId)
{
    m_ui->setupUi(this);
    setObjectName("shareVaultDialog");
    m_ui->messageWidget->setHidden(true);
    fillRoles(m_ui->addRoleCombo);
    fillRoles(m_ui->selectedRoleCombo);
    m_ui->addRoleCombo->setCurrentIndex(1);
    m_ui->membersTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_ui->membersTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_ui->membersTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    const auto fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_ui->recipientLabel->setFont(fixed);
    m_ui->recipientLabel->setVisible(false);

    auto* account = sharing ? sharing->account() : nullptr;
    const auto vault = account ? account->keyring().vault(vaultId) : VaultKeyring::Vault();
    setWindowTitle(tr("Share \"%1\"").arg(vault.name));
    m_ui->introLabel->setText(
        tr("Members open \"%1\" with their own account; its key is sealed to each of them on this device. Removing "
           "someone gives the vault a new key, so the copy they kept can't open what changes afterwards.")
            .arg(vault.name.toHtmlEscaped()));
    m_ui->rotationLabel->setVisible(vault.rotationRequired);
    m_ui->rotateButton->setVisible(vault.rotationRequired);
    if (account) {
        m_ui->ownFingerprintLabel->setText(
            tr("Your fingerprint: %1").arg(AccountCrypto::fingerprint(account->keyset().publicKeys)));
    }

    connect(m_ui->lookupButton, &QPushButton::clicked, this, &ShareVaultDialog::lookup);
    connect(m_ui->emailEdit, &QLineEdit::returnPressed, this, &ShareVaultDialog::lookup);
    connect(m_ui->emailEdit, &QLineEdit::textChanged, this, [this] {
        m_recipient = {};
        m_ui->recipientLabel->setVisible(false);
        updateButtons();
    });
    connect(m_ui->shareButton, &QPushButton::clicked, this, &ShareVaultDialog::share);
    connect(m_ui->changeRoleButton, &QPushButton::clicked, this, &ShareVaultDialog::changeRole);
    connect(m_ui->removeButton, &QPushButton::clicked, this, &ShareVaultDialog::removeSelected);
    connect(m_ui->rotateButton, &QPushButton::clicked, this, &ShareVaultDialog::rotate);
    connect(m_ui->membersTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        for (const auto& member : m_members) {
            if (member.principal == selectedPrincipal()) {
                m_ui->selectedRoleCombo->setCurrentIndex(VaultSharing::roles().indexOf(member.role));
            }
        }
        updateButtons();
    });
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (m_sharing) {
        connect(m_sharing, &VaultSharing::progress, this, [this](const QString& message) {
            if (m_busy) {
                m_ui->statusLabel->setText(message);
            }
        });
    }
    updateButtons();
    reload();
}

ShareVaultDialog::~ShareVaultDialog() = default;

QList<VaultSharing::Member> ShareVaultDialog::members() const
{
    return m_members;
}

bool ShareVaultDialog::isBusy() const
{
    return m_busy;
}

bool ShareVaultDialog::canManage() const
{
    return m_sharing && m_sharing->account()
           && m_sharing->account()->keyring().vault(m_vaultId).role == QStringLiteral("manage");
}

QString ShareVaultDialog::selectedPrincipal() const
{
    const auto items = m_ui->membersTree->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, PrincipalRole).toString();
}

void ShareVaultDialog::setBusy(const QString& message)
{
    m_busy = !message.isEmpty();
    m_ui->statusLabel->setText(message);
    updateButtons();
}

void ShareVaultDialog::updateButtons()
{
    const bool manage = canManage() && !m_busy;
    const auto selected = selectedPrincipal();
    const bool self = m_sharing && m_sharing->account() && selected == m_sharing->account()->principal();
    m_ui->addGroup->setEnabled(manage);
    m_ui->shareButton->setEnabled(manage && m_recipient.verified);
    m_ui->lookupButton->setEnabled(manage && !m_ui->emailEdit->text().trimmed().isEmpty());
    m_ui->removeButton->setEnabled(manage && !selected.isEmpty() && !self);
    m_ui->changeRoleButton->setEnabled(manage && !selected.isEmpty() && !self);
    m_ui->selectedRoleCombo->setEnabled(manage && !selected.isEmpty() && !self);
    m_ui->rotateButton->setEnabled(manage);
}

void ShareVaultDialog::finish(bool ok, const QString& error, const QString& success)
{
    setBusy({});
    if (ok) {
        if (!success.isEmpty()) {
            m_ui->messageWidget->showMessage(success, MessageWidget::Positive);
        }
    } else {
        m_ui->messageWidget->showMessage(error, MessageWidget::Error, MessageWidget::DisableAutoHide);
    }
    reload();
}

void ShareVaultDialog::reload()
{
    if (!m_sharing) {
        return;
    }
    QPointer<ShareVaultDialog> guard(this);
    m_sharing->loadMembers(m_vaultId, [guard](const QList<VaultSharing::Member>& members, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            guard->m_ui->messageWidget->showMessage(error, MessageWidget::Error);
        }
        guard->m_members = members;
        const auto selected = guard->selectedPrincipal();
        auto* tree = guard->m_ui->membersTree;
        tree->clear();
        for (const auto& member : members) {
            auto* item = new QTreeWidgetItem(tree);
            auto name = member.display.isEmpty() ? member.keys.displayName() : member.display;
            if (member.isServiceAccount()) {
                name = tr("%1 (service account)").arg(name);
            }
            if (guard->m_sharing->account() && member.principal == guard->m_sharing->account()->principal()) {
                name = tr("%1 (you)").arg(name);
            }
            item->setText(0, name);
            item->setData(0, PrincipalRole, member.principal);
            item->setText(1, roleText(member.role));
            if (member.keys.publicKeys.isNull()) {
                item->setText(2, tr("unknown"));
            } else if (!member.keys.verified) {
                item->setText(2, tr("invalid signature"));
            } else {
                item->setText(2, member.keys.fingerprint());
            }
            item->setFont(2, QFontDatabase::systemFont(QFontDatabase::FixedFont));
            item->setToolTip(2, tr("Compare this with the fingerprint the member sees in their own app"));
            if (member.principal == selected) {
                item->setSelected(true);
            }
        }
        guard->updateButtons();
    });
}

void ShareVaultDialog::lookup()
{
    const auto email = m_ui->emailEdit->text().trimmed();
    if (email.isEmpty() || !m_sharing) {
        return;
    }
    setBusy(tr("Looking up %1…").arg(email));
    QPointer<ShareVaultDialog> guard(this);
    m_sharing->lookupRecipient(email, [guard, email](const VaultSharing::Recipient& recipient, const QString& error) {
        if (!guard) {
            return;
        }
        guard->setBusy({});
        if (!error.isEmpty()) {
            guard->m_ui->messageWidget->showMessage(error, MessageWidget::Error);
            return;
        }
        guard->m_recipient = recipient;
        auto* label = guard->m_ui->recipientLabel;
        if (!recipient.verified) {
            label->setText(tr("The public keys of %1 don't carry a valid signature. Don't share with this account.")
                               .arg(recipient.displayName()));
        } else {
            label->setText(tr("%1\nFingerprint: %2\nCheck that %1 sees the same fingerprint before you share.")
                               .arg(recipient.displayName(), recipient.fingerprint()));
        }
        label->setVisible(true);
        guard->updateButtons();
    });
}

void ShareVaultDialog::share()
{
    if (!m_sharing || !m_recipient.verified) {
        return;
    }
    const auto recipient = m_recipient;
    const auto role = m_ui->addRoleCombo->currentData().toString();
    setBusy(tr("Sharing with %1…").arg(recipient.displayName()));
    QPointer<ShareVaultDialog> guard(this);
    m_sharing->grant(m_vaultId, recipient, role, [guard, recipient](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (ok) {
            guard->m_ui->emailEdit->clear();
        }
        guard->finish(ok, error, tr("Shared with %1.").arg(recipient.displayName()));
    });
}

void ShareVaultDialog::changeRole()
{
    const auto principal = selectedPrincipal();
    if (!m_sharing || principal.isEmpty()) {
        return;
    }
    const auto role = m_ui->selectedRoleCombo->currentData().toString();
    setBusy(tr("Changing access…"));
    QPointer<ShareVaultDialog> guard(this);
    m_sharing->changeRole(m_vaultId, principal, role, [guard](bool ok, const QString& error) {
        if (guard) {
            guard->finish(ok, error, tr("Access changed."));
        }
    });
}

void ShareVaultDialog::removeSelected()
{
    const auto principal = selectedPrincipal();
    if (!m_sharing || principal.isEmpty()) {
        return;
    }
    QString name = principal;
    for (const auto& member : m_members) {
        if (member.principal == principal && !member.display.isEmpty()) {
            name = member.display;
        }
    }
    auto answer = MessageBox::question(this,
                                       tr("Remove %1?").arg(name),
                                       tr("%1 loses access to this vault. The vault gets a new key, which is sealed "
                                          "to the remaining members, and is re-encrypted with it.")
                                           .arg(name),
                                       MessageBox::Remove | MessageBox::Cancel,
                                       MessageBox::Cancel);
    if (answer != MessageBox::Remove) {
        return;
    }
    setBusy(tr("Removing %1…").arg(name));
    QPointer<ShareVaultDialog> guard(this);
    m_sharing->revoke(m_vaultId, {principal}, [guard, name](bool ok, const QString& error) {
        if (guard) {
            guard->finish(ok, error, tr("%1 was removed and the vault has a new key.").arg(name));
        }
    });
}

void ShareVaultDialog::rotate()
{
    if (!m_sharing) {
        return;
    }
    setBusy(tr("Rotating the key…"));
    QPointer<ShareVaultDialog> guard(this);
    m_sharing->rotate(m_vaultId, {}, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (ok) {
            guard->m_ui->rotationLabel->setVisible(false);
            guard->m_ui->rotateButton->setVisible(false);
        }
        guard->finish(ok, error, tr("The vault has a new key."));
    });
}
