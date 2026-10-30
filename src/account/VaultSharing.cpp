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

#include "VaultSharing.h"

#include "AccountManager.h"
#include "VaultKeyring.h"

#include "core/Database.h"
#include "core/Group.h"
#include "crypto/kdf/Argon2Kdf.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncServerClient.h"

#include <QJsonArray>
#include <QTimer>
#include <QUuid>

using namespace AccountCrypto;

// A rotation in progress (SPEC §7)
struct VaultSharing::Rotation
{
    QString vaultId;
    QStringList remove;
    int version = 0;
    SecureBytes newKey;
    // The keys the blob may still be under, newest first
    QList<SecureBytes> oldKeys;
    // Finishing a rotation left pending; `remove` is rotated out afterwards
    bool resumed = false;
    Done done;
};

namespace
{
    // How long to wait for the access token that carries a new vault
    constexpr int TokenWaitMsec = 30 * 1000;

    QList<SecureBytes> unsealKeys(const VaultKeyring::Vault& vault, const SecureBytes& encSk, int onlyVersion = -1)
    {
        QList<SecureBytes> keys;
        for (const auto& sealed : vault.keys) {
            if (onlyVersion >= 0 && sealed.version != onlyVersion) {
                continue;
            }
            SecureBytes key;
            if (unseal(encSk, sealed.sealedKey, vaultKeyAad(vault.id), &key) && key.size() == KeySize) {
                keys << key;
            }
        }
        return keys;
    }

    // The account vault remote, in place of the database's sync server remotes
    void setAccountRemote(const QSharedPointer<Database>& db, const RemoteParams& params)
    {
        RemoteSettings settings(db);
        settings.loadSettings();
        for (auto* existing : settings.getAllRemoteParams()) {
            if (existing->isServer()) {
                settings.removeRemoteParams(existing->name);
            }
        }
        settings.addRemoteParams(new RemoteParams(params));
        settings.saveSettings();
    }
} // namespace

QString VaultSharing::Recipient::fingerprint() const
{
    return publicKeys.isNull() ? QString() : AccountCrypto::fingerprint(publicKeys);
}

QString VaultSharing::Recipient::displayName() const
{
    if (!email.isEmpty()) {
        return email;
    }
    return name.isEmpty() ? principal : name;
}

VaultSharing::Recipient VaultSharing::Recipient::fromJson(const QJsonObject& json)
{
    Recipient recipient;
    recipient.principal = json.value("principal").toString();
    recipient.email = json.value("email").toString();
    recipient.name = json.value("name").toString();
    bool ok = false;
    recipient.publicKeys = PublicKeys::fromJson(json, &ok);
    recipient.verified = ok && verifyPublicKeys(recipient.publicKeys);
    return recipient;
}

QStringList VaultSharing::roles()
{
    return {"read", "write", "manage"};
}

VaultSharing::VaultSharing(AccountManager* account, QObject* parent)
    : QObject(parent)
    , m_account(account)
{
}

AccountManager* VaultSharing::account() const
{
    return m_account;
}

void VaultSharing::lookupRecipient(const QString& email, RecipientCallback done)
{
    if (!m_account) {
        done({}, tr("Not signed in."));
        return;
    }
    m_account->client()->lookupUser(email, [done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({},
                 response.status == 404 ? tr("There is no OpenVireo account with this email address.")
                                        : AuthClient::errorMessage(response));
            return;
        }
        done(Recipient::fromJson(response.json()), {});
    });
}

void VaultSharing::recipientFor(const QString& principal, RecipientCallback done)
{
    if (!m_account) {
        done({}, tr("Not signed in."));
        return;
    }
    if (principal == m_account->principal()) {
        Recipient self;
        self.principal = principal;
        self.email = m_account->email();
        self.publicKeys = m_account->keyset().publicKeys;
        self.verified = verifyPublicKeys(self.publicKeys);
        done(self, {});
        return;
    }
    m_account->client()->publicKey(principal, [principal, done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        auto recipient = Recipient::fromJson(response.json());
        // Only trust the keys for the principal they were asked for
        if (recipient.principal.isEmpty()) {
            recipient.principal = principal;
        } else if (recipient.principal != principal) {
            recipient.verified = false;
        }
        done(recipient, {});
    });
}

void VaultSharing::collectRecipients(const QStringList& principals,
                                     std::function<void(const QList<Recipient>&, const QString&)> done)
{
    if (principals.isEmpty()) {
        done({}, {});
        return;
    }
    struct Gather
    {
        QHash<QString, Recipient> found;
        QString error;
        int left = 0;
    };
    auto gather = QSharedPointer<Gather>::create();
    gather->left = principals.size();
    for (const auto& principal : principals) {
        recipientFor(principal, [gather, principals, done](const Recipient& recipient, const QString& error) {
            if (!error.isEmpty() && gather->error.isEmpty()) {
                gather->error = error;
            }
            if (error.isEmpty()) {
                gather->found.insert(recipient.principal, recipient);
            }
            if (--gather->left > 0) {
                return;
            }
            QList<Recipient> ordered;
            for (const auto& name : principals) {
                if (gather->found.contains(name)) {
                    ordered << gather->found.value(name);
                }
            }
            done(ordered, gather->error);
        });
    }
}

void VaultSharing::loadMembers(const QString& vaultId, MembersCallback done)
{
    if (!m_account) {
        done({}, tr("Not signed in."));
        return;
    }
    QPointer<VaultSharing> guard(this);
    m_account->client()->vaultAccess(vaultId, [guard, done](const AuthClient::Response& response) {
        if (!guard) {
            return;
        }
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        // During a rotation a member can have a row per key version: list them once, with the newest
        QList<Member> members;
        QHash<QString, int> index;
        for (const auto& value : response.json().value("access").toArray()) {
            const auto row = value.toObject();
            Member member;
            member.principal = row.value("principal").toString();
            member.role = row.value("role").toString();
            member.keyVersion = row.value("key_version").toInt();
            member.display = row.value("display").toString();
            member.grantedBy = row.value("granted_by").toString();
            member.grantedAt = QDateTime::fromString(row.value("granted_at").toString(), Qt::ISODate);
            if (member.principal.isEmpty()) {
                continue;
            }
            if (index.contains(member.principal)) {
                auto& existing = members[index.value(member.principal)];
                if (member.keyVersion > existing.keyVersion) {
                    existing = member;
                }
                continue;
            }
            index.insert(member.principal, members.size());
            members << member;
        }
        QStringList principals;
        for (const auto& member : members) {
            principals << member.principal;
        }
        guard->collectRecipients(principals, [members, done](const QList<Recipient>& recipients, const QString&) {
            // Members whose keys can't be fetched are still listed, without a fingerprint
            auto result = members;
            for (auto& member : result) {
                for (const auto& recipient : recipients) {
                    if (recipient.principal == member.principal) {
                        member.keys = recipient;
                    }
                }
            }
            done(result, {});
        });
    });
}

void VaultSharing::grant(const QString& vaultId, const Recipient& recipient, const QString& role, Done done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done(false, tr("Unlock your account first."));
        return;
    }
    if (!roles().contains(role)) {
        done(false, tr("Unknown role: %1").arg(role));
        return;
    }
    if (!recipient.verified) {
        done(false,
             tr("The public keys of %1 don't carry a valid signature. Nothing was shared.")
                 .arg(recipient.displayName()));
        return;
    }
    const auto& keyring = m_account->keyring();
    const auto vault = keyring.vault(vaultId);
    const auto key = keyring.vaultKey(vaultId, vault.currentKeyVersion);
    if (key.empty()) {
        done(false, tr("This device doesn't hold the vault's current key. Reload the vaults and try again."));
        return;
    }
    const auto sealed = seal(recipient.publicKeys.encPk, key, vaultKeyAad(vaultId));
    const QJsonArray grants{AuthClient::grant(recipient.principal, role, vault.currentKeyVersion, sealed)};
    m_account->client()->grantAccess(vaultId, grants, [done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done(false,
                 response.status == 409 ? tr("The vault's key changed in the meantime. Reload and try again.")
                                        : AuthClient::errorMessage(response));
            return;
        }
        done(true, {});
    });
}

void VaultSharing::changeRole(const QString& vaultId, const QString& principal, const QString& role, Done done)
{
    QPointer<VaultSharing> guard(this);
    recipientFor(principal, [guard, vaultId, role, done](const Recipient& recipient, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            done(false, error);
            return;
        }
        guard->grant(vaultId, recipient, role, done);
    });
}

void VaultSharing::revoke(const QString& vaultId, const QStringList& principals, Done done)
{
    if (principals.isEmpty()) {
        done(true, {});
        return;
    }
    if (m_account && principals.contains(m_account->principal())) {
        done(false, tr("You can't remove yourself from a vault you manage."));
        return;
    }
    rotate(vaultId, principals, done);
}

void VaultSharing::rotate(const QString& vaultId, const QStringList& remove, Done done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done(false, tr("Unlock your account first."));
        return;
    }
    emit progress(tr("Checking the vault…"));
    QPointer<VaultSharing> guard(this);
    m_account->client()->getVault(vaultId, [guard, vaultId, remove, done](const AuthClient::Response& response) {
        if (!guard || !guard->m_account) {
            return;
        }
        if (!response.isSuccess()) {
            done(false, AuthClient::errorMessage(response));
            return;
        }
        const auto vault = VaultKeyring::Vault::fromJson(response.json());
        if (vault.role != "manage") {
            done(false, tr("Only members who manage a vault can change who has access."));
            return;
        }
        const auto& encSk = guard->m_account->keyset().encSk;
        auto rotation = QSharedPointer<Rotation>::create();
        rotation->vaultId = vaultId;
        rotation->remove = remove;
        rotation->done = done;

        if (vault.pendingKeyVersion > vault.currentKeyVersion) {
            // A rotation that stopped after its grants were stored: finish it before starting another
            const auto pending = unsealKeys(vault, encSk, vault.pendingKeyVersion);
            if (pending.isEmpty()) {
                done(false,
                     tr("Another member is rotating this vault's key. Try again once they are done, or after "
                        "24 hours."));
                return;
            }
            rotation->resumed = true;
            rotation->version = vault.pendingKeyVersion;
            rotation->newKey = pending.first();
            rotation->oldKeys = unsealKeys(vault, encSk, vault.currentKeyVersion);
            guard->rekey(rotation, 1);
            return;
        }

        rotation->version = vault.currentKeyVersion + 1;
        rotation->newKey = generateVaultKey();
        rotation->oldKeys = unsealKeys(vault, encSk);
        if (rotation->oldKeys.isEmpty()) {
            done(false, tr("This device doesn't hold the vault's key."));
            return;
        }
        guard->startRotation(rotation);
    });
}

void VaultSharing::startRotation(const QSharedPointer<Rotation>& rotation)
{
    emit progress(tr("Sealing a new key for the remaining members…"));
    QPointer<VaultSharing> guard(this);
    m_account->client()->vaultAccess(rotation->vaultId, [guard, rotation](const AuthClient::Response& response) {
        if (!guard || !guard->m_account) {
            return;
        }
        if (!response.isSuccess()) {
            rotation->done(false, AuthClient::errorMessage(response));
            return;
        }
        // Every remaining member keeps their role under the new key; the caller is always one of them
        const auto self = guard->m_account->principal();
        QStringList principals{self};
        QHash<QString, QString> roles{{self, "manage"}};
        for (const auto& value : response.json().value("access").toArray()) {
            const auto row = value.toObject();
            const auto principal = row.value("principal").toString();
            if (principal.isEmpty() || rotation->remove.contains(principal)) {
                continue;
            }
            if (!principals.contains(principal)) {
                principals << principal;
            }
            if (principal != self) {
                roles.insert(principal, row.value("role").toString());
            }
        }
        guard->collectRecipients(
            principals, [guard, rotation, roles](const QList<Recipient>& recipients, const QString& error) {
                if (!guard || !guard->m_account) {
                    return;
                }
                if (!error.isEmpty()) {
                    rotation->done(false, error);
                    return;
                }
                QJsonArray grants;
                for (const auto& recipient : recipients) {
                    if (!recipient.verified) {
                        rotation->done(false,
                                       tr("The public keys of %1 don't carry a valid signature, so no new key was "
                                          "sealed for them. Nothing was changed.")
                                           .arg(recipient.displayName()));
                        return;
                    }
                    const auto sealed =
                        seal(recipient.publicKeys.encPk, rotation->newKey, vaultKeyAad(rotation->vaultId));
                    grants << AuthClient::grant(
                        recipient.principal, roles.value(recipient.principal), rotation->version, sealed);
                }
                guard->m_account->client()->startRotation(
                    rotation->vaultId,
                    rotation->version,
                    grants,
                    rotation->remove,
                    [guard, rotation](const AuthClient::Response& response) {
                        if (!guard) {
                            return;
                        }
                        if (!response.isSuccess()) {
                            rotation->done(false,
                                           response.status == 409
                                               ? tr("The vault's members or key changed in the meantime. Try "
                                                    "again.")
                                               : AuthClient::errorMessage(response));
                            return;
                        }
                        guard->rekey(rotation, 1);
                    });
            });
    });
}

void VaultSharing::rekey(const QSharedPointer<Rotation>& rotation, int attempt)
{
    emit progress(tr("Re-encrypting the vault with the new key…"));
    auto* client = SyncServerClient::create(m_account->vaultRemoteParams(rotation->vaultId), this);
    QPointer<VaultSharing> guard(this);
    client->download([guard, client, rotation, attempt](const SyncServerClient::Response& response) {
        client->deleteLater();
        if (!guard) {
            return;
        }
        if (response.status == 404) {
            // No blob was ever uploaded, so there is nothing to re-key
            guard->activate(rotation);
            return;
        }
        if (response.status != 200) {
            rotation->done(false,
                           tr("The new key is stored but the vault wasn't re-encrypted: %1. Rotate it again to "
                              "finish.")
                               .arg(SyncServerClient::errorMessage(response)));
            return;
        }
        const auto newKey = VaultKeyring::compositeKeyFor(rotation->newKey);
        if (VaultKeyring::readVault(response.body, newKey)) {
            // An earlier attempt already uploaded it
            guard->activate(rotation);
            return;
        }
        QSharedPointer<Database> db;
        QString error;
        for (const auto& key : rotation->oldKeys) {
            db = VaultKeyring::readVault(response.body, VaultKeyring::compositeKeyFor(key), &error);
            if (db) {
                break;
            }
        }
        if (!db) {
            rotation->done(false, tr("The vault on the server could not be opened: %1").arg(error));
            return;
        }
        db->setKey(newKey);
        QByteArray data;
        if (!VaultKeyring::writeVault(db.data(), &data, &error)) {
            rotation->done(false, error);
            return;
        }
        auto* upload = SyncServerClient::create(guard->m_account->vaultRemoteParams(rotation->vaultId), guard);
        upload->upload(data, response.etag, [guard, upload, rotation, attempt](const SyncServerClient::Response& put) {
            upload->deleteLater();
            if (!guard) {
                return;
            }
            if (put.status == 412 && attempt < MaxUploadAttempts) {
                // Another device saved in between: re-key what it uploaded
                guard->rekey(rotation, attempt + 1);
                return;
            }
            if (put.status != 200 && put.status != 201) {
                rotation->done(false,
                               tr("The new key is stored but the vault wasn't re-encrypted: %1. Rotate it again "
                                  "to finish.")
                                   .arg(SyncServerClient::errorMessage(put)));
                return;
            }
            guard->activate(rotation);
        });
    });
}

void VaultSharing::activate(const QSharedPointer<Rotation>& rotation)
{
    emit progress(tr("Activating the new key…"));
    QPointer<VaultSharing> guard(this);
    m_account->client()->activateKey(
        rotation->vaultId, rotation->version, [guard, rotation](const AuthClient::Response& response) {
            if (!guard || !guard->m_account) {
                return;
            }
            if (!response.isSuccess()) {
                rotation->done(false, AuthClient::errorMessage(response));
                return;
            }
            guard->m_account->reloadVaults();
            if (rotation->resumed && !rotation->remove.isEmpty()) {
                guard->rotate(rotation->vaultId, rotation->remove, rotation->done);
                return;
            }
            rotation->done(true, {});
        });
}

void VaultSharing::moveToAccount(const QSharedPointer<Database>& source, const QString& name, VaultCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked || m_account->isServiceAccount()) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    if (!source || !source->key()) {
        done({}, {}, tr("Unlock the database first."));
        return;
    }
    emit progress(tr("Copying the database…"));
    // Work on a copy, so the database stays as it is if anything fails; it includes unsaved changes
    QByteArray data;
    QString error;
    if (!VaultKeyring::writeVault(source.data(), &data, &error)) {
        done({}, {}, error);
        return;
    }
    auto copy = VaultKeyring::readVault(data, source->key(), &error);
    if (!copy) {
        done({}, {}, error);
        return;
    }
    createFromDatabase(copy, name, {}, done);
}

void VaultSharing::importAsVault(const QSharedPointer<Database>& db,
                                 const QString& name,
                                 const QString& orgId,
                                 VaultCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked || m_account->isServiceAccount()) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    if (!db) {
        done({}, {}, tr("Nothing to import."));
        return;
    }
    createFromDatabase(db, name, orgId, done);
}

void VaultSharing::createVault(const QString& name, VaultCallback done)
{
    createVault(name, {}, std::move(done));
}

void VaultSharing::createVault(const QString& name, const QString& orgId, VaultCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked || m_account->isServiceAccount()) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    createFromDatabase(QSharedPointer<Database>::create(), name, orgId, done);
}

void VaultSharing::createFromDatabase(const QSharedPointer<Database>& db,
                                      const QString& name,
                                      const QString& orgId,
                                      VaultCallback done)
{
    const auto vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto vaultKey = generateVaultKey();
    const auto vaultName = name.trimmed().isEmpty() ? db->rootGroup()->name() : name.trimmed();

    // The vault key as a key file, a cheap KDF (the key has full entropy) and the vault name as the root group
    auto kdf = QSharedPointer<Argon2Kdf>::create(Argon2Kdf::Type::Argon2id);
    kdf->setMemory(VaultKeyring::VaultKdfMemoryKiB);
    kdf->setRounds(VaultKeyring::VaultKdfIterations);
    kdf->setParallelism(VaultKeyring::VaultKdfParallelism);
    db->setKdf(kdf);
    db->setKey(VaultKeyring::compositeKeyFor(vaultKey), true, true);
    db->rootGroup()->setName(vaultName);
    auto params = m_account->vaultRemoteParams(vaultId);
    params.name = tr("OpenVireo account");
    setAccountRemote(db, params);

    emit progress(tr("Creating the vault…"));
    // Key version 1, sealed to this account, which is among the grantees (SPEC §7)
    const auto self = m_account->principal();
    const auto sealed = seal(m_account->keyset().publicKeys.encPk, vaultKey, vaultKeyAad(vaultId));
    QJsonArray grants{AuthClient::grant(self, "manage", 1, sealed)};
    if (orgId.isEmpty()) {
        createWithGrants(db, vaultId, {}, vaultName, vaultKey, grants, done);
        return;
    }

    // An org vault is sealed to every confirmed owner and admin as well, with manage; the server refuses
    // one that leaves any of them out (SPEC §7)
    QPointer<VaultSharing> guard(this);
    m_account->client()->orgMembers(
        orgId,
        [guard, db, vaultId, orgId, vaultName, vaultKey, grants, self, done](const AuthClient::Response& response) mutable {
            if (!guard || !guard->m_account) {
                return;
            }
            if (!response.isSuccess()) {
                done({}, {}, AuthClient::errorMessage(response));
                return;
            }
            for (const auto& value : response.json().value("members").toArray()) {
                const auto member = value.toObject();
                const auto role = member.value("role").toString();
                const auto principal = "user:" + member.value("user_id").toString();
                if (principal == self || (role != "owner" && role != "admin")
                    || member.value("status").toString() != "confirmed" || !member.value("public").isObject()) {
                    continue;
                }
                bool ok = false;
                const auto keys = PublicKeys::fromJson(member.value("public").toObject(), &ok);
                if (!ok || !verifyPublicKeys(keys)) {
                    // Never seal to keys that aren't self-signed
                    done({},
                         {},
                         tr("The public keys of %1 don't carry a valid signature, so the vault wasn't created.")
                             .arg(member.value("email").toString()));
                    return;
                }
                grants << AuthClient::grant(principal, "manage", 1, seal(keys.encPk, vaultKey, vaultKeyAad(vaultId)));
            }
            guard->createWithGrants(db, vaultId, orgId, vaultName, vaultKey, grants, done);
        });
}

void VaultSharing::createWithGrants(const QSharedPointer<Database>& db,
                                    const QString& vaultId,
                                    const QString& orgId,
                                    const QString& vaultName,
                                    const SecureBytes& vaultKey,
                                    const QJsonArray& grants,
                                    VaultCallback done)
{
    QPointer<VaultSharing> guard(this);
    m_account->client()->createVault(vaultId,
                                     orgId,
                                     encryptVaultName(vaultKey, vaultId, vaultName),
                                     grants,
                                     [guard, vaultId, db, done](const AuthClient::Response& response) {
                                         if (!guard || !guard->m_account) {
                                             return;
                                         }
                                         if (!response.isSuccess()) {
                                             done({}, {}, AuthClient::errorMessage(response));
                                             return;
                                         }
                                         // The sync server accepts the upload once the access token lists the new vault
                                         auto* account = guard->m_account.data();
                                         auto* waiter = new QObject(guard);
                                         auto proceed = [guard, waiter, vaultId, db, done] {
                                             if (!waiter->property("done").toBool()) {
                                                 waiter->setProperty("done", true);
                                                 waiter->deleteLater();
                                                 if (guard) {
                                                     guard->uploadNewVault(vaultId, db, done);
                                                 }
                                             }
                                         };
                                         connect(account, &AccountManager::accessTokenChanged, waiter, proceed);
                                         QTimer::singleShot(TokenWaitMsec, waiter, proceed);
                                         account->refreshAccessToken();
                                     });
}

void VaultSharing::uploadNewVault(const QString& vaultId, const QSharedPointer<Database>& db, VaultCallback done)
{
    emit progress(tr("Uploading the vault…"));
    QByteArray data;
    QString error;
    if (!VaultKeyring::writeVault(db.data(), &data, &error)) {
        done({}, {}, error);
        return;
    }
    auto* client = SyncServerClient::create(m_account->vaultRemoteParams(vaultId), this);
    QPointer<VaultSharing> guard(this);
    // If-None-Match: *, creating the blob
    client->upload(data, {}, [guard, client, vaultId, db, done](const SyncServerClient::Response& response) {
        client->deleteLater();
        if (!guard || !guard->m_account) {
            return;
        }
        if (response.status != 200 && response.status != 201) {
            done({},
                 {},
                 tr("The vault was created, but its first upload failed: %1")
                     .arg(SyncServerClient::errorMessage(response)));
            return;
        }
        guard->m_account->reloadVaults();
        done(vaultId, db, {});
    });
}
