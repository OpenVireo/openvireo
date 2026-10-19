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

#ifndef KEEPASSXC_ACCOUNTVAULTSPANE_H
#define KEEPASSXC_ACCOUNTVAULTSPANE_H

#include "account/VaultKeyring.h"

#include <QPointer>
#include <QWidget>

class AccountController;
class AccountManager;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/**
 * Sidebar with the signed-in KeePassXC account and its vaults: personal vaults first, then each org's
 * under the org's name. While the account is locked it asks for the master password once ("Unlock
 * account"), or offers Quick Unlock when the platform has it and the password unlocked the account
 * before; either opens every vault. Selecting a vault shows it. It is hidden in the self-hosted and
 * no-account modes. The Vault Index layout lists the vaults in its database picker instead.
 */
class AccountVaultsPane : public QWidget
{
    Q_OBJECT

public:
    static constexpr int VaultIdRole = Qt::UserRole;
    // The org of a vault or of a heading, empty for personal vaults
    static constexpr int OrgIdRole = Qt::UserRole + 1;

    // The account's vaults as the sidebar and the database picker list them
    struct Section
    {
        QString orgId;
        // Empty for the personal vaults of an account without orgs, which need no heading
        QString title;
        QString toolTip;
        // Vaults of an org the user isn't confirmed in yet can't be opened, so none are listed
        QList<VaultKeyring::Vault> vaults;
    };

    explicit AccountVaultsPane(AccountController* controller, QWidget* parent = nullptr);

    // Personal vaults first, then one section per org in the order of the org names
    static QList<Section> sections(AccountManager* account);
    static QString vaultName(const VaultKeyring::Vault& vault);
    static QString vaultToolTip(const VaultKeyring::Vault& vault);

    QListWidget* vaultList() const;
    // The vault selected in the list, empty when none
    QString selectedVault() const;

signals:
    void shareRequested(const QString& vaultId);
    void newVaultRequested();
    void serviceAccountsRequested();

private slots:
    void refresh();
    void unlock();
    void quickUnlock();
    void openItem(QListWidgetItem* item);
    void updateButtons();

private:
    QPointer<AccountController> m_controller;
    QLabel* m_accountLabel;
    QLabel* m_stateLabel;
    QWidget* m_unlockBox;
    QLineEdit* m_passwordEdit;
    QPushButton* m_unlockButton;
    QPushButton* m_quickUnlockButton;
    QLabel* m_errorLabel;
    QListWidget* m_vaultList;
    QPushButton* m_newVaultButton;
    QPushButton* m_shareButton;
    QPushButton* m_serviceAccountsButton;
    QPushButton* m_lockButton;
    QPushButton* m_signOutButton;
};

#endif // KEEPASSXC_ACCOUNTVAULTSPANE_H
