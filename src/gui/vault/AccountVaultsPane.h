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

#include <QPointer>
#include <QWidget>

class AccountController;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/**
 * Sidebar with the signed-in KeePassXC account and its vaults: personal vaults first, then each org's
 * under the org's name. While the account is locked it asks for the master password once ("Unlock
 * account"), which opens every vault. Selecting a vault shows it. It is hidden in the self-hosted and
 * no-account modes.
 */
class AccountVaultsPane : public QWidget
{
    Q_OBJECT

public:
    static constexpr int VaultIdRole = Qt::UserRole;
    // The org of a vault or of a heading, empty for personal vaults
    static constexpr int OrgIdRole = Qt::UserRole + 1;

    explicit AccountVaultsPane(AccountController* controller, QWidget* parent = nullptr);

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
    void openItem(QListWidgetItem* item);
    void updateButtons();

private:
    QPointer<AccountController> m_controller;
    QLabel* m_accountLabel;
    QLabel* m_stateLabel;
    QWidget* m_unlockBox;
    QLineEdit* m_passwordEdit;
    QPushButton* m_unlockButton;
    QLabel* m_errorLabel;
    QListWidget* m_vaultList;
    QPushButton* m_newVaultButton;
    QPushButton* m_shareButton;
    QPushButton* m_serviceAccountsButton;
    QPushButton* m_lockButton;
    QPushButton* m_signOutButton;
};

#endif // KEEPASSXC_ACCOUNTVAULTSPANE_H
