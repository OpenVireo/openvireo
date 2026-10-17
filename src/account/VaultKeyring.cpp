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

#include "VaultKeyring.h"

#include "core/Database.h"
#include "core/Group.h"
#include "crypto/kdf/Argon2Kdf.h"
#include "format/KeePass2Reader.h"
#include "format/KeePass2Writer.h"
#include "keys/CompositeKey.h"
#include "keys/FileKey.h"

#include <QBuffer>
#include <QJsonObject>
#include <QObject>

#include <algorithm>

VaultKeyring::Vault VaultKeyring::Vault::fromJson(const QJsonObject& json, bool* ok)
{
    Vault vault;
    vault.id = json.value("id").toString();
    vault.orgId = json.value("org_id").toString();
    vault.ownerId = json.value("owner_id").toString();
    bool nameOk = false;
    vault.encryptedName = AccountCrypto::fromBase64Url(json.value("encrypted_name").toString(), &nameOk);
    vault.role = json.value("role").toString();
    vault.currentKeyVersion = json.value("current_key_version").toInt();
    vault.pendingKeyVersion =
        json.value("pending_key_version").isDouble() ? json.value("pending_key_version").toInt() : -1;
    vault.rotationRequired = json.value("rotation_required").toBool();
    for (const auto& value : json.value("keys").toArray()) {
        const auto key = value.toObject();
        bool keyOk = false;
        SealedKey sealed;
        sealed.version = key.value("key_version").toInt();
        sealed.sealedKey = AccountCrypto::fromBase64Url(key.value("sealed_key").toString(), &keyOk);
        if (keyOk && !sealed.sealedKey.isEmpty()) {
            vault.keys << sealed;
        }
    }
    // Newest first, whatever order the server used
    std::sort(vault.keys.begin(), vault.keys.end(), [](const SealedKey& a, const SealedKey& b) {
        return a.version > b.version;
    });
    if (ok) {
        *ok = !vault.id.isEmpty() && nameOk;
    }
    return vault;
}

VaultKeyring::~VaultKeyring()
{
    clear();
}

void VaultKeyring::setVaults(const QList<Vault>& vaults, const SecureBytes& encSk)
{
    clear();
    for (auto vault : vaults) {
        QMap<int, SecureBytes> keys;
        for (const auto& sealed : vault.keys) {
            SecureBytes vaultKey;
            // The AAD binds each sealed key to its vault, so the server can't hand one vault's key out as another's
            if (AccountCrypto::unseal(encSk, sealed.sealedKey, AccountCrypto::vaultKeyAad(vault.id), &vaultKey)
                && vaultKey.size() == AccountCrypto::KeySize) {
                keys.insert(sealed.version, vaultKey);
            }
        }
        // The name was encrypted with one of the keys, normally the newest
        for (auto it = keys.constEnd(); it != keys.constBegin();) {
            --it;
            if (AccountCrypto::decryptVaultName(it.value(), vault.id, vault.encryptedName, &vault.name)) {
                break;
            }
        }
        m_keys.insert(vault.id, keys);
        m_vaults.insert(vault.id, vault);
    }
}

void VaultKeyring::setVaults(const QJsonArray& vaults, const SecureBytes& encSk)
{
    QList<Vault> parsed;
    for (const auto& value : vaults) {
        bool ok = false;
        auto vault = Vault::fromJson(value.toObject(), &ok);
        if (ok) {
            parsed << vault;
        }
    }
    setVaults(parsed, encSk);
}

void VaultKeyring::clear()
{
    // SecureBytes wipe themselves when freed
    m_keys.clear();
    m_vaults.clear();
}

bool VaultKeyring::isEmpty() const
{
    return m_vaults.isEmpty();
}

QList<VaultKeyring::Vault> VaultKeyring::vaults() const
{
    return m_vaults.values();
}

bool VaultKeyring::contains(const QString& vaultId) const
{
    return m_vaults.contains(vaultId);
}

VaultKeyring::Vault VaultKeyring::vault(const QString& vaultId) const
{
    return m_vaults.value(vaultId);
}

QList<int> VaultKeyring::keyVersions(const QString& vaultId) const
{
    auto versions = m_keys.value(vaultId).keys();
    std::reverse(versions.begin(), versions.end());
    return versions;
}

VaultKeyring::SecureBytes VaultKeyring::vaultKey(const QString& vaultId, int version) const
{
    const auto keys = m_keys.value(vaultId);
    if (keys.isEmpty()) {
        return {};
    }
    if (version < 0) {
        return keys.last();
    }
    return keys.value(version);
}

QSharedPointer<CompositeKey> VaultKeyring::compositeKey(const QString& vaultId, int version) const
{
    const auto key = vaultKey(vaultId, version);
    return key.empty() ? QSharedPointer<CompositeKey>() : compositeKeyFor(key);
}

QSharedPointer<CompositeKey> VaultKeyring::compositeKeyFor(const SecureBytes& vaultKey)
{
    if (vaultKey.size() != AccountCrypto::KeySize) {
        return {};
    }
    // The vault key is a raw 32-byte key file (SPEC §7)
    auto data = AccountCrypto::fromSecure(vaultKey);
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    auto fileKey = QSharedPointer<FileKey>::create();
    if (!fileKey->load(&buffer) || fileKey->type() != FileKey::FixedBinary) {
        // 32 bytes that happen to look like an XML key file: the raw bytes are the key all the same
        fileKey = QSharedPointer<FileKey>::create();
        fileKey->setRawKey(data);
    }
    buffer.close();
    Botan::secure_scrub_memory(data.data(), static_cast<size_t>(data.size()));

    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(fileKey);
    return key;
}

QSharedPointer<Database>
VaultKeyring::readVault(const QByteArray& data, const QSharedPointer<const CompositeKey>& key, QString* error)
{
    QByteArray copy(data);
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    auto db = QSharedPointer<Database>::create();
    KeePass2Reader reader;
    if (!reader.readDatabase(&buffer, key, db.data())) {
        if (error) {
            *error = reader.errorString();
        }
        return {};
    }
    return db;
}

bool VaultKeyring::writeVault(Database* db, QByteArray* data, QString* error)
{
    data->clear();
    QBuffer buffer(data);
    buffer.open(QIODevice::WriteOnly);
    KeePass2Writer writer;
    if (!writer.writeDatabase(&buffer, db)) {
        if (error) {
            *error = writer.errorString();
        }
        return false;
    }
    return true;
}

QSharedPointer<Database>
VaultKeyring::openVault(const QString& vaultId, const QByteArray& data, int* version, QString* error) const
{
    const auto versions = keyVersions(vaultId);
    if (versions.isEmpty()) {
        if (error) {
            *error = QObject::tr("This account holds no key for the vault.");
        }
        return {};
    }
    QString lastError;
    for (const auto keyVersion : versions) {
        auto db = readVault(data, compositeKey(vaultId, keyVersion), &lastError);
        if (db) {
            if (version) {
                *version = keyVersion;
            }
            return db;
        }
    }
    if (error) {
        *error = lastError;
    }
    return {};
}

QSharedPointer<Database> VaultKeyring::createVaultDatabase(const QString& name, const SecureBytes& vaultKey)
{
    auto db = QSharedPointer<Database>::create();
    auto kdf = QSharedPointer<Argon2Kdf>::create(Argon2Kdf::Type::Argon2id);
    kdf->setMemory(VaultKdfMemoryKiB);
    kdf->setRounds(VaultKdfIterations);
    kdf->setParallelism(VaultKdfParallelism);
    db->setKdf(kdf);
    db->setKey(compositeKeyFor(vaultKey));
    db->rootGroup()->setName(name);
    return db;
}
