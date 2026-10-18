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

#ifndef KEEPASSXC_SERVICEACCOUNTSDIALOG_H
#define KEEPASSXC_SERVICEACCOUNTSDIALOG_H

#include "account/ServiceAccountAdmin.h"

#include <QDialog>
#include <QPointer>
#include <QScopedPointer>

class AccountManager;
class QLineEdit;

namespace Ui
{
    class ServiceAccountsDialog;
}

/**
 * Org administration for owners and admins: service accounts (create, disable, expire, delete), their
 * secrets (issue, revoke) and vault grants (read or write), and the org's audit log.
 *
 * A new service account's keys and every secret are made on this device. The token `kpx_sa_…` is shown
 * once, in a window of its own, and isn't kept anywhere.
 */
class ServiceAccountsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ServiceAccountsDialog(AccountManager* account, QWidget* parent = nullptr);
    ~ServiceAccountsDialog() override;

    QString currentOrg() const;
    QString currentServiceAccount() const;
    bool isBusy() const;
    // The window that shows the last token, while it is open
    QDialog* tokenDialog() const;

public slots:
    void reload();
    void createServiceAccount(const QString& name, const QDateTime& expiresAt = {});
    void issueSecret(const QString& name, const QDateTime& expiresAt = {});
    void revokeSelectedSecret();
    void grantSelectedVault();
    void applyExpiry();
    void toggleDisabled();
    void deleteSelected();
    void loadAudit(bool more = false);
    void loadMembers();
    // Confirm an accepted member; `vaults` (vault id -> role) only matters for regular members
    void confirmMember(const QString& userId, const QHash<QString, QString>& vaults = {});

private slots:
    void askForNewServiceAccount();
    void askForNewSecret();
    void askToConfirmMember();
    void showDetails();

private:
    void setBusy(const QString& message);
    void showError(const QString& error);
    void showToken(const QString& title, const QString& token);
    void loadServiceAccounts(const QString& select = {});
    ServiceAccountAdmin::ServiceAccount selected() const;

    const QScopedPointer<Ui::ServiceAccountsDialog> m_ui;
    QPointer<AccountManager> m_account;
    ServiceAccountAdmin* m_admin;
    QList<ServiceAccountAdmin::ServiceAccount> m_accounts;
    QList<ServiceAccountAdmin::Secret> m_secrets;
    QList<ServiceAccountAdmin::Member> m_members;
    QString m_auditCursor;
    QPointer<QDialog> m_tokenDialog;
    bool m_busy = false;
};

#endif // KEEPASSXC_SERVICEACCOUNTSDIALOG_H
