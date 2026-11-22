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

#include "VaultSharing.h"

#include "AccountManager.h"
#include "VaultKeyring.h"

#include "core/Database.h"
#include "core/Group.h"
#include "crypto/Random.h"
#include "crypto/kdf/Argon2Kdf.h"
#include "gui/remote/RemoteSettings.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"

#include <botan/mem_ops.h>

#include <QScopeGuard>

using namespace AccountCrypto;

namespace
{
    rust::String rs(const QString& text)
    {
        const auto utf8 = text.toUtf8();
        return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
    }

    rust::Vec<uint8_t> bytesOf(const QByteArray& data)
    {
        rust::Vec<uint8_t> out;
        out.reserve(static_cast<size_t>(data.size()));
        for (auto c : data) {
            out.push_back(static_cast<uint8_t>(c));
        }
        return out;
    }

    QByteArray toBytes(const rust::Vec<uint8_t>& data)
    {
        return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    }

    ov::VaultRole roleOf(const QString& role)
    {
        return role == "manage" ? ov::VaultRole::Manage : role == "write" ? ov::VaultRole::Write : ov::VaultRole::Read;
    }

    QString roleName(ov::VaultRole role)
    {
        switch (role) {
        case ov::VaultRole::Manage:
            return QStringLiteral("manage");
        case ov::VaultRole::Write:
            return QStringLiteral("write");
        default:
            return QStringLiteral("read");
        }
    }

    PublicKeys publicKeysOf(const ov::PublicKeys& keys)
    {
        PublicKeys out;
        out.encPk = toBytes(keys.enc_pk);
        out.signPk = toBytes(keys.sign_pk);
        out.sig = toBytes(keys.sig);
        return out;
    }

    VaultSharing::Recipient recipientOf(const ov::ShareRecipient& found)
    {
        VaultSharing::Recipient recipient;
        recipient.principal = CoreHost::toQString(found.principal);
        recipient.email = CoreHost::toQString(found.email);
        recipient.name = CoreHost::toQString(found.name);
        recipient.publicKeys = publicKeysOf(found.public_keys);
        recipient.verified = verifyPublicKeys(recipient.publicKeys);
        return recipient;
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
    auto found = std::make_shared<Recipient>();
    QPointer<VaultSharing> guard(this);
    m_account->callAccount(
        this,
        [email, found](const ov::Account& account, const ov::CancelToken& cancel) {
            *found = recipientOf(account.look_up(rs(email), cancel));
        },
        [guard, found, done](const AccountManager::CoreError& error) {
            if (!error.ok()) {
                done({},
                     error.code == "not_found" ? tr("There is no OpenVireo account with this email address.")
                                               : guard->messageFor(error));
                return;
            }
            done(*found, {});
        });
}

void VaultSharing::loadMembers(const QString& vaultId, MembersCallback done)
{
    if (!m_account) {
        done({}, tr("Not signed in."));
        return;
    }
    const auto self = m_account->principal();
    const auto selfEmail = m_account->email();
    const auto selfKeys = m_account->keyset().publicKeys;
    auto members = std::make_shared<QList<Member>>();
    QPointer<VaultSharing> guard(this);
    m_account->callAccount(
        this,
        [vaultId, members, self, selfEmail, selfKeys](const ov::Account& account, const ov::CancelToken& cancel) {
            for (const auto& row : account.members(rs(vaultId), cancel)) {
                Member member;
                member.principal = CoreHost::toQString(row.principal);
                member.role = roleName(row.role);
                member.keyVersion = static_cast<int>(row.key_version);
                member.display = CoreHost::toQString(row.display);
                if (member.principal.isEmpty()) {
                    continue;
                }
                if (member.principal == self) {
                    member.keys.principal = self;
                    member.keys.email = selfEmail;
                    member.keys.publicKeys = selfKeys;
                    member.keys.verified = verifyPublicKeys(selfKeys);
                } else {
                    try {
                        // The core checks the self-signature, and refuses keys without one
                        member.keys = recipientOf(account.recipient(row.principal, row.display, cancel));
                    } catch (const std::exception&) {
                        // Members whose keys can't be fetched are still listed, without a fingerprint
                        member.keys = {};
                    }
                }
                *members << member;
            }
        },
        [guard, members, done](const AccountManager::CoreError& error) {
            if (!error.ok()) {
                done({}, guard->messageFor(error));
                return;
            }
            done(*members, {});
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
    // The core fetches the recipient's keys again and seals the vault's current key to them only if they are still
    // the ones the user compared (share_vault_with_keys), then grants `role`
    const auto principal = recipient.principal;
    const auto keys = recipient.publicKeys;
    const auto name = recipient.displayName();
    QPointer<VaultSharing> guard(this);
    m_account->callAccount(
        this,
        [vaultId, principal, role, keys](const ov::Account& account, const ov::CancelToken& cancel) {
            ov::Grantee grantee;
            grantee.kind = ov::GranteeKind::Principal;
            grantee.value = rs(principal);
            ov::PublicKeys confirmed;
            confirmed.enc_pk = bytesOf(keys.encPk);
            confirmed.sign_pk = bytesOf(keys.signPk);
            confirmed.sig = bytesOf(keys.sig);
            confirmed.fingerprint = rs(AccountCrypto::fingerprint(keys));
            account.share_vault_with_keys(rs(vaultId), grantee, roleOf(role), confirmed, cancel);
        },
        [guard, done, name](const AccountManager::CoreError& error) {
            if (!error.ok()) {
                done(false,
                     error.code == "conflict" ? tr("The vault's key changed in the meantime. Reload and try again.")
                     : error.code == "invalid_public_keys"
                         ? tr("The public keys of %1 changed since you checked them. Nothing was shared.").arg(name)
                         : guard->messageFor(error));
                return;
            }
            done(true, {});
        });
}

void VaultSharing::changeRole(const QString& vaultId, const QString& principal, const QString& role, Done done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done(false, tr("Unlock your account first."));
        return;
    }
    if (!roles().contains(role)) {
        done(false, tr("Unknown role: %1").arg(role));
        return;
    }
    QPointer<VaultSharing> guard(this);
    m_account->callAccount(
        this,
        [vaultId, principal, role](const ov::Account& account, const ov::CancelToken& cancel) {
            account.change_role(rs(vaultId), rs(principal), roleOf(role), cancel);
        },
        [guard, done](const AccountManager::CoreError& error) {
            done(error.ok(), error.ok() ? QString() : guard->messageFor(error));
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
    emit progress(tr("Rotating the vault key…"));
    // The core seals a new key to every remaining member (the removed ones lose access at once), re-keys the blob
    // (again on 412) and activates the new version; a rotation this account holds the pending key for (another of
    // its devices, or this one, stopped before activating) is finished first
    QPointer<VaultSharing> guard(this);
    m_account->callAccount(
        this,
        [vaultId, remove](const ov::Account& account, const ov::CancelToken& cancel) {
            rust::Vec<rust::String> principals;
            for (const auto& principal : remove) {
                principals.push_back(rs(principal));
            }
            account.rotate_vault_key(rs(vaultId), std::move(principals), cancel);
        },
        [guard, done](const AccountManager::CoreError& error) {
            if (!guard || !guard->m_account) {
                return;
            }
            guard->m_account->reloadVaults();
            if (!error.ok()) {
                // rotation_pending: someone else's rotation is pending and this account doesn't hold its key. The old
                // key keeps working until it is activated or, after 24 hours, the server lets a new one replace it.
                done(false,
                     error.code == "too_many_conflicts"
                         ? tr("Other devices kept uploading the vault, so its new key isn't active yet. Rotate it "
                              "again.")
                     : error.serverCode == "rotation_pending"
                         ? tr("Another member's key rotation of this vault hasn't finished. Try again after 24 hours, "
                              "when the server drops it.")
                         : guard->messageFor(error));
                return;
            }
            done(true, {});
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
    emit progress(tr("Creating the vault…"));
    QPointer<VaultSharing> guard(this);
    // openvireo-core checks the keys of an org's owners and admins (naming one whose keys don't verify, before
    // anything is created), seals the new key, uploads the empty vault and hands back its file
    m_account->createVault(name.trimmed(),
                           orgId,
                           [guard, done](const QString& vaultId, QSharedPointer<Database> db, const QString& error) {
                               if (!guard || !guard->m_account) {
                                   return;
                               }
                               if (!error.isEmpty()) {
                                   done({}, {}, error);
                                   return;
                               }
                               auto params = guard->m_account->vaultRemoteParams(vaultId);
                               params.name = tr("OpenVireo account");
                               setAccountRemote(db, params);
                               done(vaultId, db, {});
                           });
}

void VaultSharing::createFromDatabase(const QSharedPointer<Database>& db,
                                      const QString& name,
                                      const QString& orgId,
                                      VaultCallback done)
{
    const auto vaultName = name.trimmed().isEmpty() ? db->rootGroup()->name() : name.trimmed();
    // The core reads the database under a throwaway password, re-keys it to a fresh vault key (sealed to this account
    // and in an org to its owners and admins, whose keys it checks first), creates the vault and uploads it. The
    // database's custom data comes along, so its other remotes (scp, ...) stay with it; the core leaves out the sync
    // server and account vault remotes.
    const auto password = QString::fromLatin1(randomGen()->randomArray(32).toHex());
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(password));
    // The password has full entropy: a cheap KDF, as for vault files
    auto kdf = QSharedPointer<Argon2Kdf>::create(Argon2Kdf::Type::Argon2id);
    kdf->setMemory(VaultKeyring::VaultKdfMemoryKiB);
    kdf->setRounds(VaultKeyring::VaultKdfIterations);
    kdf->setParallelism(VaultKeyring::VaultKdfParallelism);
    db->setKdf(kdf);
    db->setKey(key, true, true);
    QByteArray data;
    QString error;
    if (!VaultKeyring::writeVault(db.data(), &data, &error)) {
        done({}, {}, error);
        return;
    }

    emit progress(tr("Creating the vault…"));
    struct Imported
    {
        QString vaultId;
        QByteArray kdbx;
        QByteArray keyFile;
        QString uploadError;
    };
    auto imported = std::make_shared<Imported>();
    auto input = std::make_shared<QByteArray>(data);
    QPointer<VaultSharing> guard(this);
    m_account->callAccount(
        this,
        [input, password, vaultName, orgId, imported](const ov::Account& account, const ov::CancelToken& cancel) {
            ov::KdbxCredentials credentials;
            credentials.has_password = true;
            credentials.password = bytesOf(password.toUtf8());
            ov::ImportMode mode;
            mode.kind = ov::ImportModeKind::NewVault;
            mode.name = rs(vaultName);
            mode.org_id = rs(orgId);
            auto outcome = account.import_kdbx(bytesOf(*input), std::move(credentials), mode, cancel);
            imported->vaultId = CoreHost::toQString(outcome.vault->id());
            if (outcome.has_upload_error || !outcome.uploaded) {
                imported->uploadError = CoreHost::toQString(outcome.upload_error.message);
            }
            auto vaultCredentials = account.vault_kdbx_credentials(outcome.vault->id(), cancel);
            imported->keyFile = toBytes(vaultCredentials.key_file);
            imported->kdbx = AccountManager::vaultFile(*outcome.vault, std::move(vaultCredentials), cancel);
        },
        [guard, imported, done](const AccountManager::CoreError& error) {
            const auto scrub = qScopeGuard([imported] {
                Botan::secure_scrub_memory(imported->keyFile.data(), static_cast<size_t>(imported->keyFile.size()));
            });
            if (!guard || !guard->m_account) {
                return;
            }
            if (!error.ok()) {
                done({}, {}, guard->messageFor(error));
                return;
            }
            guard->m_account->reloadVaults();
            if (!imported->uploadError.isEmpty()) {
                done({}, {}, tr("The vault was created, but its first upload failed: %1").arg(imported->uploadError));
                return;
            }
            QString readError;
            auto vault = VaultKeyring::readVault(
                imported->kdbx, AccountManager::vaultCompositeKey(imported->keyFile), &readError);
            if (!vault) {
                done({}, {}, readError);
                return;
            }
            auto params = guard->m_account->vaultRemoteParams(imported->vaultId);
            params.name = tr("OpenVireo account");
            setAccountRemote(vault, params);
            done(imported->vaultId, vault, {});
        });
}

QString VaultSharing::messageFor(const AccountManager::CoreError& error) const
{
    return m_account ? m_account->errorMessage(error) : error.message;
}
