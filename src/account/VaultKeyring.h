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

#ifndef KEEPASSXC_VAULTKEYRING_H
#define KEEPASSXC_VAULTKEYRING_H

#include "AccountCrypto.h"

#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QSharedPointer>

class CompositeKey;
class Database;

/**
 * The vault keys an unlocked account holds: vault id -> the composite key of each key version.
 *
 * Built from GET /api/v1/vaults, whose sealed keys are unsealed with the account's keyset. A vault has
 * a current key version and, while it is being rotated, a pending one (SPEC §7). The blob is re-keyed
 * between the two, so a vault is opened with the newest key first and then the older one, which
 * keeps it readable whichever step a rotation stopped at.
 *
 * A vault's composite key is the vault key as a raw 32-byte key file, with no password.
 */
class VaultKeyring
{
public:
    using SecureBytes = AccountCrypto::SecureBytes;

    struct SealedKey
    {
        int version = 0;
        QByteArray sealedKey;
    };

    struct Vault
    {
        QString id;
        QString orgId;
        QString ownerId;
        QByteArray encryptedName;
        // read, write or manage
        QString role;
        int currentKeyVersion = 0;
        // -1 when no rotation is pending
        int pendingKeyVersion = -1;
        bool rotationRequired = false;
        QList<SealedKey> keys;
        // Decrypted once the keys are unsealed, empty when no key could decrypt it
        QString name;

        static Vault fromJson(const QJsonObject& json, bool* ok = nullptr);
    };

    // KDF settings of vault files (SPEC §7): the key has full entropy, so a costly KDF adds nothing
    static constexpr quint64 VaultKdfMemoryKiB = 1024;
    static constexpr quint64 VaultKdfIterations = 2;
    static constexpr quint32 VaultKdfParallelism = 1;

    VaultKeyring() = default;
    ~VaultKeyring();

    // Replaces the vaults with these, unsealing their keys with `encSk`. Keys that don't unseal
    // (e.g. sealed for another key version of the account) are left out.
    void setVaults(const QList<Vault>& vaults, const SecureBytes& encSk);
    void setVaults(const QJsonArray& vaults, const SecureBytes& encSk);
    // Forgets every vault key, e.g. when the account locks
    void clear();

    bool isEmpty() const;
    QList<Vault> vaults() const;
    bool contains(const QString& vaultId) const;
    Vault vault(const QString& vaultId) const;
    // The key versions this account holds for a vault, newest first
    QList<int> keyVersions(const QString& vaultId) const;
    SecureBytes vaultKey(const QString& vaultId, int version = -1) const;
    // The composite key of a vault's newest key version, or of the given one
    QSharedPointer<CompositeKey> compositeKey(const QString& vaultId, int version = -1) const;

    // Read a downloaded vault, trying the newest key version first and then the older ones.
    // `version` tells which one opened it.
    QSharedPointer<Database>
    openVault(const QString& vaultId, const QByteArray& data, int* version = nullptr, QString* error = nullptr) const;

    static QSharedPointer<CompositeKey> compositeKeyFor(const SecureBytes& vaultKey);
    // An empty vault database for a new vault key: the root group carries the vault name
    static QSharedPointer<Database> createVaultDatabase(const QString& name, const SecureBytes& vaultKey);
    static QSharedPointer<Database>
    readVault(const QByteArray& data, const QSharedPointer<const CompositeKey>& key, QString* error = nullptr);
    // The database as a KDBX file under its current key, e.g. for an upload
    static bool writeVault(Database* db, QByteArray* data, QString* error = nullptr);

private:
    QMap<QString, Vault> m_vaults;
    // vault id -> key version -> vault key
    QHash<QString, QMap<int, SecureBytes>> m_keys;
};

#endif // KEEPASSXC_VAULTKEYRING_H
