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

#ifndef KEEPASSXC_VAULTSHARING_H
#define KEEPASSXC_VAULTSHARING_H

#include "AccountCrypto.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>

#include <functional>

class AccountManager;
class Database;

/**
 * Who can open an account's vaults, and the vault operations that need the vault key on this device
 * (docs/plans/accounts/SPEC.md §6 and §7):
 *
 * - share: look the recipient's public keys up, check their self-signature, seal the vault key of the
 *   current key version to them and grant it;
 * - revoke: rotate the vault key in one step. The new key is sealed to every remaining member, the
 *   server drops the removed ones at once and keeps the new grants pending, the blob is re-keyed
 *   (download, re-key, conditional PUT, again on 412), then the new version is activated;
 * - create a vault, empty or from an existing database ("Move to account"), and upload its first blob.
 *
 * Every operation runs asynchronously and ends with its callback; nothing here blocks on the network.
 * The account must be unlocked.
 */
class VaultSharing : public QObject
{
    Q_OBJECT

public:
    using Done = std::function<void(bool ok, const QString& error)>;

    // A principal's public keys, checked before anything is sealed to them
    struct Recipient
    {
        QString principal;
        QString email;
        QString name;
        AccountCrypto::PublicKeys publicKeys;
        // The keys carry a valid self-signature
        bool verified = false;

        QString fingerprint() const;
        QString displayName() const;
        static Recipient fromJson(const QJsonObject& json);
    };

    struct Member
    {
        QString principal;
        QString role;
        int keyVersion = 0;
        // Email or service account name
        QString display;
        QString grantedBy;
        QDateTime grantedAt;
        // Filled in when the public keys could be fetched
        Recipient keys;

        bool isServiceAccount() const
        {
            return principal.startsWith("sa:");
        }
    };

    using MembersCallback = std::function<void(const QList<Member>& members, const QString& error)>;
    using RecipientCallback = std::function<void(const Recipient& recipient, const QString& error)>;
    using VaultCallback =
        std::function<void(const QString& vaultId, const QSharedPointer<Database>& vault, const QString& error)>;

    // Re-keying retries this often when another device uploads in between (412)
    static constexpr int MaxUploadAttempts = 3;
    // Roles, from least to most (SPEC §9)
    static QStringList roles();

    explicit VaultSharing(AccountManager* account, QObject* parent = nullptr);

    AccountManager* account() const;

    void loadMembers(const QString& vaultId, MembersCallback done);
    void lookupRecipient(const QString& email, RecipientCallback done);
    void recipientFor(const QString& principal, RecipientCallback done);

    // Seal the vault's current key to a checked recipient and grant `role`
    void grant(const QString& vaultId, const Recipient& recipient, const QString& role, Done done);
    // A new role for an existing member; their key is sealed again, as the server wants a full grant
    void changeRole(const QString& vaultId, const QString& principal, const QString& role, Done done);
    // Remove members, which rotates the vault key in the same step
    void revoke(const QString& vaultId, const QStringList& principals, Done done);
    // Rotate the vault key, removing `remove`. A rotation this account left pending (e.g. after a crash
    // between the upload and the activation) is finished first.
    void rotate(const QString& vaultId, const QStringList& remove, Done done);

    // Create a vault (key version 1, sealed to this account) from a copy of `source` re-keyed to a fresh
    // vault key, upload it and hand back the vault database. `source` itself isn't changed.
    void moveToAccount(const QSharedPointer<Database>& source, const QString& name, VaultCallback done);
    // Create an empty vault and upload it
    void createVault(const QString& name, VaultCallback done);

signals:
    // A step of a longer operation, for a progress label
    void progress(const QString& message);

private:
    struct Rotation;

    void startRotation(const QSharedPointer<Rotation>& rotation);
    void rekey(const QSharedPointer<Rotation>& rotation, int attempt);
    void activate(const QSharedPointer<Rotation>& rotation);
    void createFromDatabase(const QSharedPointer<Database>& db, const QString& name, VaultCallback done);
    void uploadNewVault(const QString& vaultId, const QSharedPointer<Database>& db, VaultCallback done);
    void collectRecipients(const QStringList& principals,
                           std::function<void(const QList<Recipient>&, const QString& error)> done);

    QPointer<AccountManager> m_account;
};

#endif // KEEPASSXC_VAULTSHARING_H
