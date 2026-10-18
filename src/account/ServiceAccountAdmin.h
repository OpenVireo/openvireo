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

#ifndef KEEPASSXC_SERVICEACCOUNTADMIN_H
#define KEEPASSXC_SERVICEACCOUNTADMIN_H

#include "AccountCrypto.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <functional>

class AccountManager;

/**
 * Org administration for owners and admins: service accounts, their secrets and vault grants, and the
 * org's audit log (docs/plans/accounts/openapi.yaml, SPEC §9).
 *
 * Everything secret is made here, on the client: the SA keyset, its master key, which is sealed to every
 * owner and admin of the org, and each secret with its `kek`. The token `kpx_sa_…` that carries a secret
 * and its kek exists only in the callback that creates it, to be shown once; the server keeps a hash of
 * the secret and the master key wrapped with the kek.
 */
class ServiceAccountAdmin : public QObject
{
    Q_OBJECT

public:
    struct Org
    {
        QString id;
        QString name;
        QString role;

        bool canAdminister() const
        {
            return role == "owner" || role == "admin";
        }
    };

    struct Member
    {
        QString userId;
        QString email;
        QString role;
        // invited, accepted or confirmed
        QString status;
        AccountCrypto::PublicKeys publicKeys;
        bool verified = false;

        QString principal() const
        {
            return "user:" + userId;
        }
        bool isAdmin() const
        {
            return role == "owner" || role == "admin";
        }
    };

    struct ServiceAccount
    {
        QString id;
        QString orgId;
        QString name;
        AccountCrypto::PublicKeys publicKeys;
        QString createdBy;
        QDateTime createdAt;
        QDateTime expiresAt;
        bool disabled = false;

        static ServiceAccount fromJson(const QJsonObject& json);
    };

    struct Secret
    {
        QString id;
        QString name;
        QString prefix;
        QDateTime createdAt;
        QDateTime lastUsedAt;
        QDateTime expiresAt;
    };

    struct AuditEvent
    {
        QString id;
        QDateTime at;
        QString actor;
        QString action;
        QString target;
        QString ip;
        QJsonObject details;
    };

    using Done = std::function<void(bool ok, const QString& error)>;
    using OrgsCallback = std::function<void(const QList<Org>& orgs, const QString& error)>;
    using MembersCallback = std::function<void(const QList<Member>& members, const QString& error)>;
    using ListCallback = std::function<void(const QList<ServiceAccount>& accounts, const QString& error)>;
    using SecretsCallback = std::function<void(const QList<Secret>& secrets, const QString& error)>;
    // `token` is the kpx_sa_… token, to show once
    using TokenCallback =
        std::function<void(const ServiceAccount& account, const QString& token, const QString& error)>;
    using AuditCallback =
        std::function<void(const QList<AuditEvent>& events, const QString& next, const QString& error)>;

    explicit ServiceAccountAdmin(AccountManager* account, QObject* parent = nullptr);

    void loadOrgs(OrgsCallback done);
    void listMembers(const QString& orgId, MembersCallback done);
    // Confirm an accepted member. Owners and admins get every org vault with manage and the master key of
    // every service account; a regular member gets the vaults in `vaults` (vault id -> read or write),
    // which the admin picks. Keys are sealed only to a member whose public keys are self-signed.
    void confirmMember(const QString& orgId, const Member& member, const QHash<QString, QString>& vaults, Done done);
    // The org's vaults this account holds the keys of
    QList<QPair<QString, QString>> orgVaults(const QString& orgId) const;

    void list(const QString& orgId, ListCallback done);
    // A new SA with its first secret; `expiresAt` invalid means it doesn't expire
    void create(const QString& orgId, const QString& name, const QDateTime& expiresAt, TokenCallback done);
    void setExpiry(const QString& saId, const QDateTime& expiresAt, Done done);
    void setDisabled(const QString& saId, bool disabled, Done done);
    void remove(const QString& saId, Done done);

    void listSecrets(const QString& saId, SecretsCallback done);
    // Another secret for the SA: needs this admin's sealed copy of its master key
    void issueSecret(const QString& saId, const QString& name, const QDateTime& expiresAt, TokenCallback done);
    void revokeSecret(const QString& saId, const QString& secretId, Done done);

    // Seal the vault's current key to the SA and grant it `role` (read or write)
    void grantVault(const ServiceAccount& account, const QString& vaultId, const QString& role, Done done);

    void auditLog(const QString& orgId, const QString& before, AuditCallback done);

    static constexpr int AuditPageSize = 50;

private:
    // {id, name, secret_hash, prefix, wrapped_master, expires_at} and the token for it
    QJsonObject newSecret(const QString& saId,
                          const AccountCrypto::SecureBytes& master,
                          const QString& name,
                          const QDateTime& expiresAt,
                          QString* token) const;

    QPointer<AccountManager> m_account;
};

#endif // KEEPASSXC_SERVICEACCOUNTADMIN_H
