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

    const auto selected = selectedVault();
    m_vaultList->clear();
    if (unlocked) {
        auto vaults = account->keyring().vaults();
        std::sort(vaults.begin(), vaults.end(), [](const VaultKeyring::Vault& a, const VaultKeyring::Vault& b) {
            return a.name.localeAwareCompare(b.name) < 0;
        });
        for (const auto& vault : vaults) {
            auto* item = new QListWidgetItem(icons()->icon("database"), vault.name.isEmpty() ? vault.id : vault.name);
            item->setData(VaultIdRole, vault.id);
            auto tip = roleText(vault.role);
            if (vault.rotationRequired) {
                tip += "\n" + tr("A member was removed: rotate the key from the Share dialog.");
            }
            item->setToolTip(tip);
            m_vaultList->addItem(item);
            if (vault.id == selected) {
                m_vaultList->setCurrentItem(item);
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

void AccountVaultsPane::openItem(QListWidgetItem* item)
{
    if (m_controller && item) {
        m_controller->openVault(item->data(VaultIdRole).toString());
    }
}
