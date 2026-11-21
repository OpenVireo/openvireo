/*
 *  Copyright (C) 2026 Bongani Mbigi
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
 * Filled from openvireo-core (Account::vaults or ServiceAccount::vaults, and each vault's newest key from
 * vault_kdbx_credentials): the core unseals the keys with the keyset it keeps. A vault has a current key version
 * and, while it is being rotated, a pending one (SPEC §7); older keys stay in the core, which re-keys a local copy
 * under one of them when it syncs it.
 *
 * A vault's composite key is the vault key as a raw 32-byte key file, with no password.
 */
class VaultKeyring
{
public:
    using SecureBytes = AccountCrypto::SecureBytes;

    struct Vault
    {
        QString id;
        QString orgId;
        // read, write or manage
        QString role;
        int currentKeyVersion = 0;
        // -1 when no rotation is pending
        int pendingKeyVersion = -1;
        bool rotationRequired = false;
        // As openvireo-core decrypted it; empty when no key could
        QString name;
    };

    // KDF settings of vault files (SPEC §7): the key has full entropy, so a costly KDF adds nothing
    static constexpr quint64 VaultKdfMemoryKiB = 1024;
    static constexpr quint64 VaultKdfIterations = 2;
    static constexpr quint32 VaultKdfParallelism = 1;

    VaultKeyring() = default;
    ~VaultKeyring();

    // Replaces the vaults with these, whose names and keys openvireo-core decrypted already: vault id -> its newest
    // key version and key
    void setVaults(const QList<Vault>& vaults, const QHash<QString, QPair<int, SecureBytes>>& keys);
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
