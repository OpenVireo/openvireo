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

#include "ServiceAccountAdmin.h"

#include "AccountManager.h"
#include "VaultKeyring.h"
#include "VaultSharing.h"

#include <QJsonArray>
#include <QSharedPointer>
#include <QUuid>

using namespace AccountCrypto;

namespace
{
    QDateTime timestamp(const QJsonValue& value)
    {
        return value.isString() ? QDateTime::fromString(value.toString(), Qt::ISODate) : QDateTime();
    }

    QJsonValue timestampJson(const QDateTime& time)
    {
        return time.isValid() ? QJsonValue(time.toUTC().toString(Qt::ISODate)) : QJsonValue(QJsonValue::Null);
    }
} // namespace

ServiceAccountAdmin::ServiceAccount ServiceAccountAdmin::ServiceAccount::fromJson(const QJsonObject& json)
{
    ServiceAccount account;
    account.id = json.value("id").toString();
    account.orgId = json.value("org_id").toString();
    account.name = json.value("name").toString();
    account.publicKeys = PublicKeys::fromJson(json.value("public").toObject());
    account.createdBy = json.value("created_by").toString();
    account.createdAt = timestamp(json.value("created_at"));
    account.expiresAt = timestamp(json.value("expires_at"));
    account.disabled = json.value("disabled").toBool();
    return account;
}

ServiceAccountAdmin::ServiceAccountAdmin(AccountManager* account, QObject* parent)
    : QObject(parent)
    , m_account(account)
{
}

void ServiceAccountAdmin::loadOrgs(OrgsCallback done)
{
    if (!m_account) {
        done({}, tr("Not signed in."));
        return;
    }
    m_account->client()->me([done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        QList<Org> orgs;
        for (const auto& value : response.json().value("orgs").toArray()) {
            const auto json = value.toObject();
            // Invited or accepted members hold no keys yet
            if (json.contains("status") && json.value("status").toString() != "confirmed") {
                continue;
            }
            orgs << Org{json.value("id").toString(), json.value("name").toString(), json.value("role").toString()};
        }
        done(orgs, {});
    });
}

void ServiceAccountAdmin::listMembers(const QString& orgId, MembersCallback done)
{
    m_account->client()->orgMembers(orgId, [done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        QList<Member> members;
        for (const auto& value : response.json().value("members").toArray()) {
            const auto json = value.toObject();
            Member member;
            member.userId = json.value("user_id").toString();
            member.email = json.value("email").toString();
            member.role = json.value("role").toString();
            member.status = json.value("status").toString();
            bool ok = false;
            member.publicKeys = PublicKeys::fromJson(json.value("public").toObject(), &ok);
            member.verified = ok && verifyPublicKeys(member.publicKeys);
            members << member;
        }
        done(members, {});
    });
}

QList<QPair<QString, QString>> ServiceAccountAdmin::orgVaults(const QString& orgId) const
{
    QList<QPair<QString, QString>> vaults;
    if (!m_account) {
        return vaults;
    }
    for (const auto& vault : m_account->keyring().vaults()) {
        // Only vaults this account manages can be granted (SPEC §12)
        if (vault.orgId == orgId && vault.role == "manage") {
            vaults << qMakePair(vault.id, vault.name.isEmpty() ? vault.id : vault.name);
        }
    }
    return vaults;
}

void ServiceAccountAdmin::confirmMember(const QString& orgId,
                                        const Member& member,
                                        const QHash<QString, QString>& vaults,
                                        Done done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done(false, tr("Unlock your account first."));
        return;
    }
    if (!member.verified) {
        done(false,
             tr("The public keys of %1 don't carry a valid signature. Nothing was sealed to them.").arg(member.email));
        return;
    }
    // Owners and admins get every org vault with manage; a regular member what the admin picked
    const auto& keyring = m_account->keyring();
    QJsonArray grants;
    for (const auto& vault : orgVaults(orgId)) {
        QString role = member.isAdmin() ? QStringLiteral("manage") : vaults.value(vault.first);
        if (role.isEmpty()) {
            continue;
        }
        if (!member.isAdmin() && role != "read" && role != "write" && role != "manage") {
            done(false, tr("Unknown role: %1").arg(role));
            return;
        }
        const auto current = keyring.vault(vault.first).currentKeyVersion;
        const auto key = keyring.vaultKey(vault.first, current);
        if (key.empty()) {
            continue;
        }
        auto grant = AuthClient::grant(
            member.principal(), role, current, seal(member.publicKeys.encPk, key, vaultKeyAad(vault.first)));
        grant["vault_id"] = vault.first;
        grants << grant;
    }

    QPointer<ServiceAccountAdmin> guard(this);
    auto send = [guard, orgId, member, grants, done](const QJsonArray& masterKeys) {
        if (!guard || !guard->m_account) {
            return;
        }
        guard->m_account->client()->confirmMember(
            orgId, member.userId, grants, masterKeys, [guard, done](const AuthClient::Response& response) {
                if (!response.isSuccess()) {
                    done(false, AuthClient::errorMessage(response));
                    return;
                }
                if (guard && guard->m_account) {
                    guard->m_account->reloadVaults();
                }
                done(true, {});
            });
    };
    if (!member.isAdmin()) {
        send({});
        return;
    }
    // An admin can issue secrets for every service account, so they get each master key too
    list(orgId, [guard, member, send, done](const QList<ServiceAccount>& accounts, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            done(false, error);
            return;
        }
        if (accounts.isEmpty()) {
            send({});
            return;
        }
        struct Gather
        {
            QJsonArray masterKeys;
            int left = 0;
        };
        auto gather = QSharedPointer<Gather>::create();
        gather->left = accounts.size();
        for (const auto& account : accounts) {
            guard->m_account->client()->getServiceAccount(
                account.id, [guard, gather, member, send, saId = account.id](const AuthClient::Response& response) {
                    if (!guard || !guard->m_account) {
                        return;
                    }
                    SecureBytes master;
                    if (response.isSuccess()
                        && unseal(guard->m_account->keyset().encSk,
                                  fromBase64Url(response.json().value("master_key").toString()),
                                  saMasterAad(saId),
                                  &master)) {
                        gather->masterKeys << QJsonObject{
                            {"sa_id", saId},
                            {"principal", member.principal()},
                            {"sealed_key", toBase64Url(seal(member.publicKeys.encPk, master, saMasterAad(saId)))}};
                    }
                    if (--gather->left == 0) {
                        send(gather->masterKeys);
                    }
                });
        }
    });
}

void ServiceAccountAdmin::list(const QString& orgId, ListCallback done)
{
    m_account->client()->listServiceAccounts(orgId, [done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        QList<ServiceAccount> accounts;
        for (const auto& value : response.json().value("service_accounts").toArray()) {
            accounts << ServiceAccount::fromJson(value.toObject());
        }
        done(accounts, {});
    });
}

QJsonObject ServiceAccountAdmin::newSecret(const QString& saId,
                                           const SecureBytes& master,
                                           const QString& name,
                                           const QDateTime& expiresAt,
                                           QString* token) const
{
    ServiceAccountToken parts;
    parts.url = m_account->server().issuer;
    parts.saId = saId;
    parts.secret = randomSecure(KeySize);
    parts.kek = randomSecure(KeySize);
    *token = buildServiceAccountToken(parts);
    QJsonObject secret{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                       {"secret_hash", parts.secretHash()},
                       {"prefix", parts.secretPrefix()},
                       {"wrapped_master", toBase64Url(wrap(parts.kek, master, saMasterAad(saId)))},
                       {"expires_at", timestampJson(expiresAt)}};
    if (!name.isEmpty()) {
        secret["name"] = name;
    }
    return secret;
}

void ServiceAccountAdmin::create(const QString& orgId,
                                 const QString& name,
                                 const QDateTime& expiresAt,
                                 TokenCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked || m_account->isServiceAccount()) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    if (name.trimmed().isEmpty()) {
        done({}, {}, tr("Give the service account a name."));
        return;
    }
    QPointer<ServiceAccountAdmin> guard(this);
    // The master key is sealed to every owner and admin, so that any of them can issue secrets later
    m_account->client()->orgMembers(orgId, [guard, orgId, name, expiresAt, done](const AuthClient::Response& response) {
        if (!guard || !guard->m_account) {
            return;
        }
        if (!response.isSuccess()) {
            done({}, {}, AuthClient::errorMessage(response));
            return;
        }
        const auto saId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto master = randomSecure(KeySize);
        auto keyset = generateKeyset();

        QJsonArray masterKeys;
        bool sealedToSelf = false;
        const auto self = guard->m_account->principal();
        for (const auto& value : response.json().value("members").toArray()) {
            const auto member = value.toObject();
            const auto role = member.value("role").toString();
            if ((role != "owner" && role != "admin") || member.value("status").toString("confirmed") != "confirmed") {
                continue;
            }
            const auto principal = "user:" + member.value("user_id").toString();
            bool ok = false;
            const auto keys = principal == self ? guard->m_account->keyset().publicKeys
                                                : PublicKeys::fromJson(member.value("public").toObject(), &ok);
            if (principal != self && (!ok || !verifyPublicKeys(keys))) {
                // Never seal to keys that aren't self-signed; that admin can be given the key later
                continue;
            }
            masterKeys << QJsonObject{{"sa_id", saId},
                                      {"principal", principal},
                                      {"sealed_key", toBase64Url(seal(keys.encPk, master, saMasterAad(saId)))}};
            sealedToSelf |= principal == self;
        }
        if (!sealedToSelf) {
            masterKeys << QJsonObject{
                {"sa_id", saId},
                {"principal", self},
                {"sealed_key",
                 toBase64Url(seal(guard->m_account->keyset().publicKeys.encPk, master, saMasterAad(saId)))}};
        }

        QString token;
        const auto secret = guard->newSecret(saId, master, QObject::tr("First secret"), expiresAt, &token);
        QJsonObject body{{"id", saId},
                         {"name", name.trimmed()},
                         {"expires_at", timestampJson(expiresAt)},
                         {"keyset", wrappedKeysetJson(master, keyset)},
                         {"master_keys", masterKeys},
                         {"secret", secret}};
        auto publicKeys = keyset.publicKeys;
        keyset.clear();
        guard->m_account->client()->createServiceAccount(
            orgId, body, [orgId, saId, name, publicKeys, expiresAt, token, done](const AuthClient::Response& response) {
                if (!response.isSuccess()) {
                    done({}, {}, AuthClient::errorMessage(response));
                    return;
                }
                auto account = ServiceAccount::fromJson(response.json());
                if (account.id.isEmpty()) {
                    account.id = saId;
                    account.orgId = orgId;
                    account.name = name.trimmed();
                    account.expiresAt = expiresAt;
                }
                if (account.publicKeys.isNull()) {
                    account.publicKeys = publicKeys;
                }
                done(account, token, {});
            });
    });
}

void ServiceAccountAdmin::setExpiry(const QString& saId, const QDateTime& expiresAt, Done done)
{
    m_account->client()->updateServiceAccount(
        saId, {{"expires_at", timestampJson(expiresAt)}}, [done](const AuthClient::Response& response) {
            done(response.isSuccess(), response.isSuccess() ? QString() : AuthClient::errorMessage(response));
        });
}

void ServiceAccountAdmin::setDisabled(const QString& saId, bool disabled, Done done)
{
    m_account->client()->updateServiceAccount(
        saId, {{"disabled", disabled}}, [done](const AuthClient::Response& response) {
            done(response.isSuccess(), response.isSuccess() ? QString() : AuthClient::errorMessage(response));
        });
}

void ServiceAccountAdmin::remove(const QString& saId, Done done)
{
    m_account->client()->deleteServiceAccount(saId, [done](const AuthClient::Response& response) {
        done(response.isSuccess(), response.isSuccess() ? QString() : AuthClient::errorMessage(response));
    });
}

void ServiceAccountAdmin::listSecrets(const QString& saId, SecretsCallback done)
{
    m_account->client()->listServiceAccountSecrets(saId, [done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        QList<Secret> secrets;
        for (const auto& value : response.json().value("secrets").toArray()) {
            const auto json = value.toObject();
            Secret secret;
            secret.id = json.value("id").toString();
            secret.name = json.value("name").toString();
            secret.prefix = json.value("prefix").toString();
            secret.createdAt = timestamp(json.value("created_at"));
            secret.lastUsedAt = timestamp(json.value("last_used_at"));
            secret.expiresAt = timestamp(json.value("expires_at"));
            secrets << secret;
        }
        done(secrets, {});
    });
}

void ServiceAccountAdmin::issueSecret(const QString& saId,
                                      const QString& name,
                                      const QDateTime& expiresAt,
                                      TokenCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    QPointer<ServiceAccountAdmin> guard(this);
    m_account->client()->getServiceAccount(
        saId, [guard, saId, name, expiresAt, done](const AuthClient::Response& response) {
            if (!guard || !guard->m_account) {
                return;
            }
            if (!response.isSuccess()) {
                done({}, {}, AuthClient::errorMessage(response));
                return;
            }
            const auto json = response.json();
            SecureBytes master;
            if (!unseal(guard->m_account->keyset().encSk,
                        fromBase64Url(json.value("master_key").toString()),
                        saMasterAad(saId),
                        &master)
                || master.size() != KeySize) {
                done({}, {}, tr("You don't hold this service account's master key. Ask another owner or admin."));
                return;
            }
            QString token;
            const auto secret = guard->newSecret(saId, master, name, expiresAt, &token);
            const auto account = ServiceAccount::fromJson(json);
            guard->m_account->client()->issueServiceAccountSecret(
                saId, secret, [account, token, done](const AuthClient::Response& response) {
                    if (!response.isSuccess()) {
                        done({}, {}, AuthClient::errorMessage(response));
                        return;
                    }
                    done(account, token, {});
                });
        });
}

void ServiceAccountAdmin::revokeSecret(const QString& saId, const QString& secretId, Done done)
{
    m_account->client()->revokeServiceAccountSecret(saId, secretId, [done](const AuthClient::Response& response) {
        done(response.isSuccess(), response.isSuccess() ? QString() : AuthClient::errorMessage(response));
    });
}

void ServiceAccountAdmin::grantVault(const ServiceAccount& account,
                                     const QString& vaultId,
                                     const QString& role,
                                     Done done)
{
    if (role != "read" && role != "write") {
        done(false, tr("Service accounts can read or write a vault, but not manage it."));
        return;
    }
    VaultSharing::Recipient recipient;
    recipient.principal = "sa:" + account.id;
    recipient.name = account.name;
    recipient.publicKeys = account.publicKeys;
    recipient.verified = !account.publicKeys.isNull() && verifyPublicKeys(account.publicKeys);
    auto* sharing = new VaultSharing(m_account, this);
    sharing->grant(vaultId, recipient, role, [sharing, done](bool ok, const QString& error) {
        sharing->deleteLater();
        done(ok, error);
    });
}

void ServiceAccountAdmin::auditLog(const QString& orgId, const QString& before, AuditCallback done)
{
    m_account->client()->auditLog(orgId, before, AuditPageSize, [done](const AuthClient::Response& response) {
        if (!response.isSuccess()) {
            done({}, {}, AuthClient::errorMessage(response));
            return;
        }
        const auto json = response.json();
        QList<AuditEvent> events;
        for (const auto& value : json.value("events").toArray()) {
            const auto row = value.toObject();
            AuditEvent event;
            event.id = row.value("id").toString();
            event.at = timestamp(row.value("at"));
            event.actor = row.value("actor").toString();
            event.action = row.value("action").toString();
            event.target = row.value("target").toString();
            event.ip = row.value("ip").toString();
            event.details = row.value("details").toObject();
            events << event;
        }
        done(events, json.value("next").toString(), {});
    });
}
