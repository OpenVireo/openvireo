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

#include "AccountVaultsPane.h"

#include "VaultControls.h"

#include "account/AccountManager.h"
#include "account/VaultKeyring.h"
#include "gui/Icons.h"
#include "gui/account/AccountController.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QVBoxLayout>

namespace
{
    QString roleText(const QString& role)
    {
        if (role == "manage") {
            return QObject::tr("Can manage");
        }
        if (role == "write") {
            return QObject::tr("Can edit");
        }
        return QObject::tr("Read only");
    }
} // namespace

AccountVaultsPane::AccountVaultsPane(AccountController* controller, QWidget* parent)
    : QWidget(parent)
    , m_controller(controller)
    , m_accountLabel(new QLabel(this))
    , m_stateLabel(new QLabel(this))
    , m_unlockBox(new QWidget(this))
    , m_passwordEdit(new VaultLineEdit(m_unlockBox))
    , m_unlockButton(new VaultButton(tr("Unlock account"), m_unlockBox))
    , m_quickUnlockButton(new VaultButton(tr("Quick Unlock"), m_unlockBox))
    , m_errorLabel(new QLabel(this))
    , m_vaultList(new QListWidget(this))
    , m_newVaultButton(new VaultButton(tr("New vault…"), this))
    , m_shareButton(new VaultButton(tr("Share…"), this))
    , m_serviceAccountsButton(new VaultButton(tr("Service accounts…"), this))
    , m_lockButton(new VaultButton(tr("Lock"), this))
    , m_signOutButton(new VaultButton(tr("Sign out"), this))
{
    setObjectName("accountVaultsPane");
    setMinimumWidth(200);
    setMaximumWidth(280);

    auto font = m_accountLabel->font();
    font.setBold(true);
    m_accountLabel->setFont(font);
    m_accountLabel->setObjectName("accountVaultsAccount");
    m_accountLabel->setWordWrap(true);
    m_stateLabel->setObjectName("accountVaultsState");
    m_errorLabel->setObjectName("accountVaultsError");
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);

    m_passwordEdit->setObjectName("accountVaultsPassword");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("Master password"));
    m_passwordEdit->setAccessibleName(tr("Account master password"));
    m_unlockButton->setObjectName("accountVaultsUnlock");
    m_unlockButton->setDefault(true);
    auto* unlockLayout = new QVBoxLayout(m_unlockBox);
    unlockLayout->setContentsMargins(0, 0, 0, 0);
    unlockLayout->addWidget(m_passwordEdit);
    unlockLayout->addWidget(m_unlockButton);
    m_quickUnlockButton->setObjectName("accountVaultsQuickUnlock");
    m_quickUnlockButton->setToolTip(tr("Unlock with Touch ID, Windows Hello or your system password"));
    m_quickUnlockButton->setVisible(false);
    unlockLayout->addWidget(m_quickUnlockButton);

    m_vaultList->setObjectName("accountVaultsList");
    m_vaultList->setAccessibleName(tr("Account vaults"));
    m_vaultList->setFrameShape(QFrame::NoFrame);
    m_newVaultButton->setObjectName("accountVaultsNew");
    m_shareButton->setObjectName("accountVaultsShare");
    m_serviceAccountsButton->setObjectName("accountVaultsServiceAccounts");
    m_lockButton->setObjectName("accountVaultsLock");
    m_signOutButton->setObjectName("accountVaultsSignOut");

    auto* vaultButtons = new QHBoxLayout();
    vaultButtons->addWidget(m_newVaultButton);
    vaultButtons->addWidget(m_shareButton);
    auto* accountButtons = new QHBoxLayout();
    accountButtons->addWidget(m_lockButton);
    accountButtons->addWidget(m_signOutButton);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_accountLabel);
    layout->addWidget(m_stateLabel);
    layout->addWidget(m_unlockBox);
    layout->addWidget(m_errorLabel);
    layout->addWidget(m_vaultList, 1);
    layout->addLayout(vaultButtons);
    layout->addWidget(m_serviceAccountsButton);
    layout->addLayout(accountButtons);

    connect(m_unlockButton, &QPushButton::clicked, this, &AccountVaultsPane::unlock);
    connect(m_quickUnlockButton, &QPushButton::clicked, this, &AccountVaultsPane::quickUnlock);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &AccountVaultsPane::unlock);
    connect(m_vaultList, &QListWidget::itemActivated, this, &AccountVaultsPane::openItem);
    connect(m_vaultList, &QListWidget::itemClicked, this, &AccountVaultsPane::openItem);
    connect(m_vaultList, &QListWidget::currentItemChanged, this, &AccountVaultsPane::updateButtons);
    connect(m_newVaultButton, &QPushButton::clicked, this, &AccountVaultsPane::newVaultRequested);
    connect(m_shareButton, &QPushButton::clicked, this, [this] {
        if (!selectedVault().isEmpty()) {
            emit shareRequested(selectedVault());
        }
    });
    connect(m_serviceAccountsButton, &QPushButton::clicked, this, &AccountVaultsPane::serviceAccountsRequested);
    connect(m_lockButton, &QPushButton::clicked, this, [this] {
        if (m_controller) {
            m_controller->lockAccount();
        }
    });
    connect(m_signOutButton, &QPushButton::clicked, this, [this] {
        if (m_controller) {
            m_controller->signOut();
        }
    });

    if (m_controller) {
        auto* account = m_controller->account();
        connect(account, &AccountManager::stateChanged, this, &AccountVaultsPane::refresh);
        connect(account, &AccountManager::vaultsChanged, this, &AccountVaultsPane::refresh);
        connect(account, &AccountManager::orgsChanged, this, &AccountVaultsPane::refresh);
        connect(account, &AccountManager::emailChanged, this, &AccountVaultsPane::refresh);
        connect(account, &AccountManager::error, this, [this](const QString& message) {
            if (m_controller && m_controller->account()->state() == AccountManager::State::Locked) {
                m_errorLabel->setText(message);
                m_errorLabel->setVisible(true);
            }
        });
        connect(m_controller, &AccountController::modeChanged, this, &AccountVaultsPane::refresh);
    }
    refresh();
}

QListWidget* AccountVaultsPane::vaultList() const
{
    return m_vaultList;
}

QString AccountVaultsPane::selectedVault() const
{
    auto* item = m_vaultList->currentItem();
    return item ? item->data(VaultIdRole).toString() : QString();
}

void AccountVaultsPane::refresh()
{
    if (!m_controller) {
        setVisible(false);
        return;
    }
    auto* account = m_controller->account();
    const auto state = account->state();
    setVisible(m_controller->mode() == AccountController::Mode::Account && state != AccountManager::State::SignedOut);
    m_accountLabel->setText(account->email());

    const bool unlocked = state == AccountManager::State::Unlocked;
    const bool busy = state == AccountManager::State::SigningIn;
    m_stateLabel->setText(unlocked ? tr("Unlocked") : busy ? tr("Unlocking…") : tr("Locked"));
    m_unlockBox->setVisible(!unlocked);
    m_unlockBox->setEnabled(!busy);
    if (unlocked) {
        m_passwordEdit->clear();
        m_errorLabel->setVisible(false);
    }

    // Quick Unlock only once the password unlocked the account in this session, as for databases
    m_quickUnlockButton->setVisible(!unlocked && account->canQuickUnlock());

    const auto selected = selectedVault();
    m_vaultList->clear();
    if (unlocked) {
        auto vaults = account->keyring().vaults();
        std::sort(vaults.begin(), vaults.end(), [](const VaultKeyring::Vault& a, const VaultKeyring::Vault& b) {
            return a.name.localeAwareCompare(b.name) < 0;
        });
        // Personal vaults first, then one group per org in the order of the org names
        QMap<QString, QList<VaultKeyring::Vault>> byOrg;
        for (const auto& vault : vaults) {
            byOrg[vault.orgId] << vault;
        }
        QList<AccountManager::Org> orgs;
        for (const auto& org : account->orgs()) {
            if (byOrg.contains(org.id) || !org.isConfirmed()) {
                orgs << org;
            }
        }
        for (const auto& orgId : byOrg.keys()) {
            if (!orgId.isEmpty() && account->org(orgId).id.isEmpty()) {
                // Not in /me (yet): still listed, under a generic heading
                orgs << AccountManager::Org{orgId, {}, {}, {}};
            }
        }
        std::sort(orgs.begin(), orgs.end(), [](const AccountManager::Org& a, const AccountManager::Org& b) {
            return a.name.localeAwareCompare(b.name) < 0;
        });

        const auto addHeading = [this](const QString& text, const QString& orgId, const QString& tip) {
            auto* heading = new QListWidgetItem(text);
            heading->setFlags(Qt::NoItemFlags);
            heading->setData(OrgIdRole, orgId);
            heading->setToolTip(tip);
            auto font = heading->font();
            font.setBold(true);
            heading->setFont(font);
            m_vaultList->addItem(heading);
        };
        const auto addVault = [this, &selected](const VaultKeyring::Vault& vault) {
            auto* item = new QListWidgetItem(icons()->icon("database"), vault.name.isEmpty() ? vault.id : vault.name);
            item->setData(VaultIdRole, vault.id);
            item->setData(OrgIdRole, vault.orgId);
            auto tip = roleText(vault.role);
            if (vault.rotationRequired) {
                tip += "\n" + tr("A member was removed: rotate the key from the Share dialog.");
            }
            item->setToolTip(tip);
            m_vaultList->addItem(item);
            if (vault.id == selected) {
                m_vaultList->setCurrentItem(item);
            }
        };

        // Without any org the list stays as plain as it always was
        if (!orgs.isEmpty() && byOrg.contains({})) {
            addHeading(tr("Personal"), {}, {});
        }
        for (const auto& vault : byOrg.value({})) {
            addVault(vault);
        }
        for (const auto& org : orgs) {
            const auto name = org.name.isEmpty() ? tr("Organization") : org.name;
            if (!org.isConfirmed()) {
                addHeading(tr("%1 (waiting for confirmation)").arg(name),
                           org.id,
                           tr("An owner or admin of %1 has to confirm you before you can open its vaults.").arg(name));
                continue;
            }
            addHeading(name, org.id, {});
            for (const auto& vault : byOrg.value(org.id)) {
                addVault(vault);
            }
        }
    }
    m_vaultList->setEnabled(unlocked);
    updateButtons();
}

void AccountVaultsPane::updateButtons()
{
    const bool unlocked = m_controller && m_controller->account()->state() == AccountManager::State::Unlocked;
    const bool serviceAccount = m_controller && m_controller->account()->isServiceAccount();
    m_newVaultButton->setEnabled(unlocked && !serviceAccount);
    m_shareButton->setEnabled(unlocked && !selectedVault().isEmpty());
    m_serviceAccountsButton->setEnabled(unlocked && !serviceAccount);
    m_lockButton->setEnabled(unlocked);
}

void AccountVaultsPane::unlock()
{
    if (!m_controller || m_passwordEdit->text().isEmpty()) {
        return;
    }
    m_errorLabel->setVisible(false);
    m_controller->unlockAccount(m_passwordEdit->text());
    m_passwordEdit->clear();
}

void AccountVaultsPane::quickUnlock()
{
    if (!m_controller) {
        return;
    }
    m_errorLabel->setVisible(false);
    m_controller->quickUnlockAccount();
}

void AccountVaultsPane::openItem(QListWidgetItem* item)
{
    // Headings carry no vault
    if (m_controller && item && !item->data(VaultIdRole).toString().isEmpty()) {
        m_controller->openVault(item->data(VaultIdRole).toString());
    }
}
