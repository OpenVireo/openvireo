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

#include "FakeAccountServer.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QUuid>

using namespace AccountCrypto;

namespace FakeAccount
{
    const QString Issuer = QStringLiteral("https://accounts.keepassxc.example");
    const QString SyncUrl = QStringLiteral("https://sync.keepassxc.example/api/v1");
    const QString SelfHostedUrl = QStringLiteral("https://sync.home.example");
    const QString Totp = QStringLiteral("123456");
    const QString InvitationUrl = QStringLiteral("https://app.keepassxc.example/invitations");

    namespace
    {
        QString uuid()
        {
            return QUuid::createUuid().toString(QUuid::WithoutBraces);
        }

        QString now()
        {
            return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        }

        int roleRank(const QString& role)
        {
            return QStringList{"read", "write", "manage"}.indexOf(role);
        }

        bool isManagerRole(const QString& role)
        {
            return role == "owner" || role == "admin";
        }
    } // namespace

    AccountManager::ServerConfig serverConfig()
    {
        AccountManager::ServerConfig config;
        config.issuer = Issuer;
        config.api = Issuer + "/api/v1";
        config.sync = SyncUrl;
        config.opaqueServerId = QUrl(Issuer).host();
        return config;
    }

    KdfParams testKdf()
    {
        auto kdf = KdfParams::generate();
        kdf.memoryKiB = KdfParams::MinMemoryKiB;
        kdf.iterations = KdfParams::MinIterations;
        return kdf;
    }

    // The record is the password's hash; logging in compares hashes
    bool FakeOpaqueClient::createRegistrationRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* request)
    {
        *sec = password;
        *request = "request";
        return true;
    }

    bool
    FakeOpaqueClient::finalizeRegistration(const SecureBytes& sec, const QByteArray&, const Ids&, QByteArray* record)
    {
        *record = sha256(fromSecure(sec));
        return true;
    }

    bool FakeOpaqueClient::createCredentialRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* ke1)
    {
        *sec = password;
        *ke1 = sha256(fromSecure(password));
        return true;
    }

    bool FakeOpaqueClient::recoverCredentials(const QByteArray& ke2,
                                              const SecureBytes&,
                                              const Ids&,
                                              QByteArray* authU,
                                              SecureBytes*)
    {
        *authU = "auth";
        return ke2 == "ok";
    }

    QString Server::next(const QString& prefix)
    {
        return prefix + QString::number(++counter);
    }

    QString Server::roleOf(const QString& vaultId, const QString& principal) const
    {
        QString role;
        for (const auto& access : vaults.value(vaultId).access) {
            if (access.principal == principal && roleRank(access.role) > roleRank(role)) {
                role = access.role;
            }
        }
        return role;
    }

    QString Server::effectiveRole(const QString& vaultId, const QString& principal) const
    {
        const auto vault = vaults.value(vaultId);
        if (principal.startsWith("user:")
            && ((vault.orgId.isEmpty() && "user:" + vault.ownerId == principal)
                || (!vault.orgId.isEmpty() && isOrgManager(vault.orgId, principal)))) {
            return QStringLiteral("manage");
        }
        return roleOf(vaultId, principal);
    }

    bool Server::isOrgManager(const QString& orgId, const QString& principal) const
    {
        for (const auto& member : orgs.value(orgId).members) {
            if ("user:" + member.userId == principal && member.status == "confirmed" && isManagerRole(member.role)) {
                return true;
            }
        }
        return false;
    }

    Server::OrgMember* Server::orgMember(const QString& orgId, const QString& userId)
    {
        if (!orgs.contains(orgId)) {
            return nullptr;
        }
        for (auto& member : orgs[orgId].members) {
            if (member.userId == userId) {
                return &member;
            }
        }
        return nullptr;
    }

    QString Server::issueAccessToken(const QString& principal)
    {
        Token token;
        token.principal = principal;
        for (const auto& vault : vaults) {
            const auto role = effectiveRole(vault.id, principal);
            if (!role.isEmpty()) {
                token.vaults.insert(vault.id, role);
            }
        }
        const auto value = next("at-");
        accessTokens.insert(value, token);
        return value;
    }

    QJsonObject Server::newSession(const User& user)
    {
        const auto refresh = next("kpx_rt_");
        refreshTokens.insert(refresh, user.id);
        return {{"access_token", issueAccessToken("user:" + user.id)},
                {"refresh_token", refresh},
                {"expires_in", 600},
                {"device_id", "d4e5f6a7-b8c9-4d0e-9f1a-2b3c4d5e6f70"},
                {"user", QJsonObject{{"id", user.id}, {"email", user.email}}},
                {"kdf", user.kdf},
                {"keyset", user.keyset}};
    }

    void Server::log(const QString& actor, const QString& action, const QString& target, const QString& orgId)
    {
        audit.prepend(QJsonObject{{"id", next("ev-")},
                                  {"at", now()},
                                  {"actor", actor},
                                  {"action", action},
                                  {"target", target},
                                  {"ip", "192.0.2.1"},
                                  {"org", orgId},
                                  {"details", QJsonObject()}});
    }

    Keyset Server::addUser(const QString& email, QString* principal)
    {
        auto keyset = generateKeyset();
        User user;
        user.id = uuid();
        user.email = email;
        users.insert(email.toLower(), user);
        auto entry = keyset.publicKeys.toJson();
        entry["principal"] = "user:" + user.id;
        entry["email"] = email;
        directory.insert("user:" + user.id, entry);
        if (principal) {
            *principal = "user:" + user.id;
        }
        return keyset;
    }

    QString Server::userId(const QString& email) const
    {
        return users.value(email.toLower()).id;
    }

    QString Server::addOrg(const QString& name, const QString& ownerEmail)
    {
        Org org;
        org.id = uuid();
        org.name = name;
        org.members << OrgMember{userId(ownerEmail), "owner"};
        orgs.insert(org.id, org);
        return org.id;
    }

    void Server::addOrgMember(const QString& orgId, const QString& email, const QString& role, const QString& status)
    {
        orgs[orgId].members << OrgMember{userId(email), role, status};
    }

    QString Server::addPairingCode()
    {
        const auto code = QString("PAIR%1").arg(++counter, 4, 10, QChar('0'));
        pairingCodes.insert(code, next("device-token-"));
        return code;
    }

    SecureBytes
    Server::vaultKeyFor(const QString& vaultId, const QString& principal, const SecureBytes& encSk, int version) const
    {
        const auto vault = vaults.value(vaultId);
        const auto wanted = version < 0 ? vault.current : version;
        for (const auto& access : vault.access) {
            if (access.principal == principal && access.keyVersion == wanted) {
                SecureBytes key;
                if (unseal(encSk, fromBase64Url(access.sealedKey), vaultKeyAad(vaultId), &key)) {
                    return key;
                }
            }
        }
        return {};
    }

    FakeAuthClient::FakeAuthClient(QSharedPointer<Server> server)
        : AuthClient(Issuer + "/api/v1")
        , m_server(std::move(server))
    {
    }

    void FakeAuthClient::send(const QByteArray& verb,
                              const QUrl& url,
                              const Headers& headers,
                              const QByteArray& body,
                              int,
                              Callback callback)
    {
        auto& server = *m_server;
        Response response;
        const auto reply = [&](int status, const QJsonObject& object = {}) {
            response.status = status;
            response.body = status == 204 ? QByteArray() : QJsonDocument(object).toJson(QJsonDocument::Compact);
        };
        // {"error": message, "code": code}, as the real server answers
        const auto error = [&](int status, const QString& code, const QString& message = {}) {
            reply(status, {{"error", message.isEmpty() ? code : message}, {"code", code}});
        };

        if (url.path() == "/.well-known/kpx-configuration") {
            server.requests << "GET .well-known/kpx-configuration";
            if (url.host() == QUrl(Issuer).host()) {
                reply(200,
                      {{"issuer", Issuer},
                       {"api", Issuer + "/api/v1"},
                       {"sync", SyncUrl},
                       {"jwks_uri", Issuer + "/.well-known/jwks.json"},
                       {"opaque",
                        QJsonObject{{"lib", "libopaque"}, {"context", "kpx-opaque-v1"}, {"ids", QUrl(Issuer).host()}}},
                       {"kdf_min", QJsonObject{{"m", 19456}, {"t", 2}}}});
            } else {
                error(404, "not_found");
            }
            deliverLater(response, std::move(callback));
            return;
        }

        const auto path = url.path().mid(QString("/api/v1/").size());
        const auto parts = path.split('/');
        server.requests << QString::fromLatin1(verb) + " " + path;
        const auto json = QJsonDocument::fromJson(body).object();
        QString bearer;
        for (const auto& header : headers) {
            if (header.first == "Authorization") {
                bearer = QString::fromLatin1(header.second).mid(QString("Bearer ").size());
            }
        }
        const auto b64 = [&json](const QString& name) { return fromBase64Url(json.value(name).toString()); };
        const auto caller = server.accessTokens.value(bearer).principal;
        const auto callerUser = [&]() -> Server::User {
            for (const auto& user : server.users) {
                if ("user:" + user.id == caller) {
                    return user;
                }
            }
            return {};
        };
        const auto vaultJson = [&](const Server::Vault& vault) {
            QJsonArray keys;
            for (const auto& access : vault.access) {
                if (access.principal == caller
                    && (access.keyVersion == vault.current || access.keyVersion == vault.pending)) {
                    keys << QJsonObject{{"key_version", access.keyVersion}, {"sealed_key", access.sealedKey}};
                }
            }
            return QJsonObject{
                {"id", vault.id},
                {"org_id", vault.orgId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(vault.orgId)},
                {"owner_id", vault.ownerId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(vault.ownerId)},
                {"encrypted_name", vault.encryptedName},
                {"role", server.effectiveRole(vault.id, caller)},
                {"current_key_version", vault.current},
                {"pending_key_version", vault.pending < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(vault.pending)},
                {"rotation_required", vault.rotationRequired},
                {"keys", keys}};
        };
        const auto addGrants = [&](Server::Vault& vault, const QJsonArray& grants) {
            for (const auto& value : grants) {
                const auto grant = value.toObject();
                Server::Access access;
                access.principal = grant.value("principal").toString();
                access.role = grant.value("role").toString();
                access.keyVersion = grant.value("key_version").toInt();
                access.sealedKey = grant.value("sealed_key").toString();
                access.grantedBy = caller;
                // A new grant for the same principal and version replaces the old one (e.g. a role change)
                for (int i = vault.access.size() - 1; i >= 0; --i) {
                    if (vault.access[i].principal == access.principal
                        && vault.access[i].keyVersion == access.keyVersion) {
                        vault.access.removeAt(i);
                    }
                }
                vault.access << access;
            }
        };
        const auto isOrgAdmin = [&](const QString& orgId) { return server.isOrgManager(orgId, caller); };
        const auto memberJson = [&](const Server::OrgMember& member) {
            auto keys = server.directory.value("user:" + member.userId);
            const auto email = keys.take("email").toString();
            keys.remove("principal");
            keys.remove("name");
            return QJsonObject{{"user_id", member.userId},
                               {"email", email.isEmpty() ? callerUser().email : email},
                               {"role", member.role},
                               {"status", member.status},
                               {"public", keys.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(keys)}};
        };
        const auto invitationJson = [&](const Server::Invitation& invitation) {
            return QJsonObject{{"id", invitation.id},
                               {"email", invitation.email},
                               {"role", invitation.role},
                               {"expires_at", invitation.expiresAt.toString(Qt::ISODate)},
                               {"invited_by", invitation.invitedBy}};
        };
        // The SA master keys a user holds in an org go when they stop being an owner or admin
        const auto dropMasterKeys = [&](const QString& orgId, const QString& principal) {
            for (auto& sa : server.serviceAccounts) {
                if (sa.json.value("org_id").toString() == orgId) {
                    sa.masterKeys.remove(principal);
                }
            }
        };

        if (path == "auth/register/start") {
            const auto session = server.next("reg-");
            server.registrations.insert(session, json.value("email").toString());
            reply(200, {{"session_id", session}, {"response", toBase64Url(QByteArray("pub"))}, {"user_id", uuid()}});
        } else if (path == "auth/register/finish") {
            Server::User user;
            user.id = uuid();
            user.email = server.registrations.take(json.value("session_id").toString());
            user.record = b64("record");
            user.kdf = json.value("kdf").toObject();
            user.keyset = json.value("keyset").toObject();
            server.users.insert(user.email.toLower(), user);
            auto entry = user.keyset;
            entry.remove("wrapped");
            entry["principal"] = "user:" + user.id;
            entry["email"] = user.email;
            server.directory.insert("user:" + user.id, entry);
            reply(201, server.newSession(user));
        } else if (path == "auth/login/start") {
            const auto email = json.value("email").toString().toLower();
            const bool ok = server.users.contains(email) && server.users.value(email).record == b64("ke1");
            const auto session = server.next("login-");
            server.logins.insert(session, {email, ok});
            reply(200, {{"session_id", session}, {"ke2", toBase64Url(QByteArray(ok ? "ok" : "bad"))}});
        } else if (path == "auth/login/finish") {
            const auto login = server.logins.take(json.value("session_id").toString());
            if (!login.second) {
                error(401, "unauthorized");
            } else if (server.users.value(login.first).mfa) {
                const auto token = server.next("mfa-");
                server.mfaTokens.insert(token, login.first);
                reply(200, {{"mfa_required", true}, {"mfa_token", token}, {"methods", QJsonArray{"totp"}}});
            } else {
                reply(200, server.newSession(server.users.value(login.first)));
            }
        } else if (path == "auth/login/mfa") {
            const auto token = json.value("mfa_token").toString();
            if (!server.mfaTokens.contains(token)) {
                error(401, "unauthorized");
            } else if (json.value("totp").toString() != Totp) {
                if (++server.mfaAttempts[token] >= 5) {
                    server.mfaTokens.remove(token);
                }
                error(401, "invalid_mfa");
            } else {
                reply(200, server.newSession(server.users.value(server.mfaTokens.take(token))));
            }
        } else if (path == "auth/refresh") {
            const auto token = json.value("refresh_token").toString();
            if (!server.refreshTokens.contains(token)) {
                error(401, "unauthorized");
            } else {
                const auto userId = server.refreshTokens.take(token);
                const auto refresh = server.next("kpx_rt_");
                server.refreshTokens.insert(refresh, userId);
                reply(200,
                      {{"access_token", server.issueAccessToken("user:" + userId)},
                       {"refresh_token", refresh},
                       {"expires_in", 600}});
            }
        } else if (path == "auth/service-account") {
            const auto saId = json.value("sa_id").toString();
            const auto hash = toHex(sha256(b64("secret")));
            const Server::SaSecret* match = nullptr;
            for (const auto& secret : server.secrets) {
                if (secret.saId == saId && secret.hash == hash) {
                    match = &secret;
                }
            }
            const auto sa = server.serviceAccounts.value(saId);
            if (!match || sa.json.value("disabled").toBool()) {
                error(401, "unauthorized");
            } else {
                reply(200,
                      {{"access_token", server.issueAccessToken("sa:" + saId)},
                       {"expires_in", 600},
                       {"wrapped_master", match->wrappedMaster},
                       {"keyset", sa.json.value("keyset")}});
            }
        } else if (caller.isEmpty()) {
            error(401, "unauthorized");
        } else if (path == "auth/logout") {
            reply(204);
        } else if (path == "me") {
            const auto user = callerUser();
            QJsonArray orgs;
            for (const auto& org : server.orgs) {
                for (const auto& member : org.members) {
                    if (member.userId == user.id) {
                        orgs << QJsonObject{
                            {"id", org.id}, {"name", org.name}, {"role", member.role}, {"status", member.status}};
                    }
                }
            }
            reply(200,
                  {{"id", user.id},
                   {"email", user.email},
                   {"kdf", user.kdf},
                   {"keyset", user.keyset},
                   {"mfa", QJsonObject{{"totp", user.mfa}}},
                   {"orgs", orgs}});
        } else if (path == "users/lookup") {
            const auto email = QUrlQuery(url).queryItemValue("email", QUrl::FullyDecoded).toLower();
            const auto id = server.users.value(email).id;
            if (id.isEmpty() || !server.directory.contains("user:" + id)) {
                error(404, "not_found");
            } else {
                reply(200, server.directory.value("user:" + id));
            }
        } else if (parts.value(0) == "principals" && parts.value(2) == "public-key") {
            const auto principal = QUrl::fromPercentEncoding(parts.value(1).toUtf8());
            if (server.directory.contains(principal)) {
                reply(200, server.directory.value(principal));
            } else {
                error(404, "not_found");
            }
        } else if (path == "vaults" && verb == "GET") {
            QJsonArray list;
            for (const auto& vault : server.vaults) {
                if (!server.effectiveRole(vault.id, caller).isEmpty()) {
                    list << vaultJson(vault);
                }
            }
            reply(200, {{"vaults", list}});
        } else if (path == "vaults" && verb == "POST") {
            const auto id = json.value("id").toString();
            const auto orgId = json.value("org_id").toString();
            const auto grants = json.value("grants").toArray();
            bool callerGranted = false;
            bool allFirstVersion = !grants.isEmpty();
            QSet<QString> managing;
            for (const auto& value : grants) {
                const auto grant = value.toObject();
                callerGranted |= grant.value("principal").toString() == caller;
                allFirstVersion &= grant.value("key_version").toInt() == 1;
                if (grant.value("role").toString() == "manage") {
                    managing.insert(grant.value("principal").toString());
                }
            }
            // SPEC §7: an org vault is sealed with manage to every confirmed owner and admin
            QStringList missing;
            QString notMember;
            if (!orgId.isEmpty()) {
                for (const auto& member : server.orgs.value(orgId).members) {
                    const auto principal = "user:" + member.userId;
                    if (member.status == "confirmed" && isManagerRole(member.role) && !managing.contains(principal)) {
                        missing << principal;
                    }
                }
                missing.sort();
                for (const auto& value : grants) {
                    const auto principal = value.toObject().value("principal").toString();
                    const auto* member = server.orgMember(orgId, principal.mid(5));
                    if (!principal.startsWith("user:") || !member || member->status != "confirmed") {
                        notMember = principal;
                    }
                }
            }
            const auto* self = orgId.isEmpty() ? nullptr : server.orgMember(orgId, caller.mid(5));
            if (!orgId.isEmpty() && (!server.orgs.contains(orgId) || !self || self->status == "invited")) {
                error(404, "not_found", "Organization not found");
            } else if (!orgId.isEmpty() && !isOrgAdmin(orgId)) {
                error(403, "forbidden", "Only owners and admins can do that");
            } else if (!allFirstVersion) {
                error(400, "invalid_request", "Every grant of a new vault has key_version 1");
            } else if (!callerGranted) {
                // SPEC §7: a new vault starts at key version 1 and the caller is one of the grantees
                error(400, "invalid_request", "grants must include the caller");
            } else if (!missing.isEmpty()) {
                error(400,
                      "invalid_request",
                      "An org vault needs a manage grant for every confirmed owner and admin; missing: "
                          + missing.join(", "));
            } else if (server.vaults.contains(id)) {
                error(409, "conflict", "A vault with this id exists");
            } else if (!notMember.isEmpty()) {
                error(400, "invalid_request", notMember + " is not a confirmed member of the org");
            } else {
                Server::Vault vault;
                vault.id = id;
                vault.orgId = orgId;
                // Org vaults have no owner; the org's owners and admins manage them
                vault.ownerId = orgId.isEmpty() ? caller.mid(5) : QString();
                vault.encryptedName = json.value("encrypted_name").toString();
                vault.current = 1;
                addGrants(vault, grants);
                server.vaults.insert(id, vault);
                for (const auto& value : grants) {
                    server.log(caller, "vault.access.grant", value.toObject().value("principal").toString(), orgId);
                }
                server.log(caller, "vault.create", id, orgId);
                reply(201, vaultJson(vault));
            }
        } else if (parts.value(0) == "vaults" && !server.vaults.contains(parts.value(1))) {
            error(404, "not_found");
        } else if (parts.value(0) == "vaults" && server.effectiveRole(parts.value(1), caller).isEmpty()) {
            error(404, "not_found", "Vault not found");
        } else if (parts.value(0) == "vaults" && parts.size() == 2 && verb == "GET") {
            reply(200, vaultJson(server.vaults.value(parts.value(1))));
        } else if (parts.value(0) == "vaults" && parts.value(2) == "access" && verb == "GET") {
            QJsonArray access;
            const auto vault = server.vaults.value(parts.value(1));
            for (const auto& row : vault.access) {
                const auto entry = server.directory.value(row.principal);
                auto display = entry.value("email").toString();
                if (row.principal.startsWith("sa:")) {
                    display = server.serviceAccounts.value(row.principal.mid(3)).json.value("name").toString();
                }
                access << QJsonObject{{"principal", row.principal},
                                      {"role", row.role},
                                      {"key_version", row.keyVersion},
                                      {"display", display},
                                      {"granted_by", row.grantedBy},
                                      {"granted_at", now()}};
            }
            reply(200, {{"access", access}});
        } else if (parts.value(0) == "vaults" && server.effectiveRole(parts.value(1), caller) != "manage") {
            error(403, "forbidden", "This needs the manage role on the vault");
        } else if (parts.value(0) == "vaults" && parts.value(2) == "access" && verb == "POST") {
            auto& vault = server.vaults[parts.value(1)];
            for (const auto& grant : json.value("grants").toArray()) {
                if (grant.toObject().value("key_version").toInt() != vault.current) {
                    error(409, "conflict");
                    deliverLater(response, std::move(callback));
                    return;
                }
            }
            addGrants(vault, json.value("grants").toArray());
            for (const auto& value : json.value("grants").toArray()) {
                server.log(caller, "vault.access.grant", value.toObject().value("principal").toString(), vault.orgId);
            }
            reply(200);
        } else if (parts.value(0) == "vaults" && parts.value(2) == "keys" && parts.size() == 3) {
            auto& vault = server.vaults[parts.value(1)];
            const auto version = json.value("key_version").toInt();
            QSet<QString> remaining;
            for (const auto& row : vault.access) {
                remaining.insert(row.principal);
            }
            for (const auto& principal : json.value("remove").toArray()) {
                remaining.remove(principal.toString());
            }
            QSet<QString> granted;
            for (const auto& grant : json.value("grants").toArray()) {
                if (grant.toObject().value("key_version").toInt() == version) {
                    granted.insert(grant.toObject().value("principal").toString());
                }
            }
            if (vault.pending >= 0 || version != vault.current + 1 || granted != remaining) {
                error(409, "conflict");
            } else {
                for (const auto& principal : json.value("remove").toArray()) {
                    for (int i = vault.access.size() - 1; i >= 0; --i) {
                        if (vault.access[i].principal == principal.toString()) {
                            vault.access.removeAt(i);
                        }
                    }
                }
                addGrants(vault, json.value("grants").toArray());
                vault.pending = version;
                vault.rotationRequired = false;
                server.log(caller, "vault.key.rotate", vault.id);
                reply(202);
            }
        } else if (parts.value(0) == "vaults" && parts.value(2) == "keys" && parts.value(4) == "activate") {
            auto& vault = server.vaults[parts.value(1)];
            const auto version = parts.value(3).toInt();
            if (vault.pending != version) {
                error(409, "conflict");
            } else {
                vault.current = version;
                vault.pending = -1;
                for (int i = vault.access.size() - 1; i >= 0; --i) {
                    if (vault.access[i].keyVersion < version) {
                        vault.access.removeAt(i);
                    }
                }
                server.log(caller, "vault.key.activate", vault.id);
                reply(204);
            }
        } else if (path == "orgs" && verb == "POST") {
            const auto name = json.value("name").toString().trimmed();
            if (name.isEmpty() || name.size() > 128) {
                error(400, "invalid_request", "name must be 1 to 128 characters");
            } else {
                Server::Org org;
                org.id = uuid();
                org.name = name;
                org.members << Server::OrgMember{caller.mid(5), "owner", "confirmed"};
                server.orgs.insert(org.id, org);
                server.log(caller, "org.create", org.id, org.id);
                reply(201, {{"id", org.id}, {"name", org.name}, {"role", "owner"}, {"created_at", now()}});
            }
        } else if (path == "invitations/accept") {
            const auto token = json.value("token");
            const auto user = callerUser();
            Server::Invitation* invitation = nullptr;
            for (auto& candidate : server.invitations) {
                if (token.isString() && candidate.tokenHash == toHex(sha256(token.toString().toUtf8()))) {
                    invitation = &candidate;
                }
            }
            if (!token.isString()) {
                error(400, "invalid_request", "token must be a string");
            } else if (!invitation) {
                error(410, "session_expired", "The invitation is invalid");
            } else if (invitation->accepted || invitation->expiresAt < QDateTime::currentDateTimeUtc()) {
                error(410, "session_expired", "The invitation was used or expired");
            } else if (invitation->email != user.email.trimmed().toLower()) {
                error(403, "forbidden", "The invitation is for another e-mail address");
            } else if (server.orgMember(invitation->orgId, user.id)
                       && server.orgMember(invitation->orgId, user.id)->status != "invited") {
                error(409, "conflict", "You are already a member");
            } else {
                auto* member = server.orgMember(invitation->orgId, user.id);
                if (!member) {
                    server.orgs[invitation->orgId].members << Server::OrgMember{user.id, invitation->role, "accepted"};
                    member = &server.orgs[invitation->orgId].members.last();
                }
                member->role = invitation->role;
                member->status = "accepted";
                invitation->accepted = true;
                server.log(caller, "org.member.accept", caller, invitation->orgId);
                reply(200, memberJson(*member));
            }
        } else if (parts.value(0) == "orgs"
                   && (!server.orgs.contains(parts.value(1)) || !server.orgMember(parts.value(1), caller.mid(5))
                       || server.orgMember(parts.value(1), caller.mid(5))->status == "invited")) {
            // Outsiders and invitees don't see the org
            error(404, "not_found", "Organization not found");
        } else if (parts.value(0) == "orgs" && parts.size() == 2 && verb == "GET") {
            const auto org = server.orgs.value(parts.value(1));
            reply(200,
                  {{"id", org.id},
                   {"name", org.name},
                   {"role", server.orgMember(org.id, caller.mid(5))->role},
                   {"created_at", now()}});
        } else if (parts.value(0) == "orgs" && parts.value(2) == "members" && parts.size() == 3 && verb == "GET") {
            QJsonArray members;
            for (const auto& member : server.orgs.value(parts.value(1)).members) {
                members << memberJson(member);
            }
            reply(200, {{"members", members}});
        } else if (parts.value(0) == "orgs" && parts.value(2) == "members" && parts.size() == 4 && verb == "DELETE") {
            const auto orgId = parts.value(1);
            const auto* me = server.orgMember(orgId, caller.mid(5));
            const auto* target = server.orgMember(orgId, parts.value(3));
            int owners = 0;
            for (const auto& member : server.orgs.value(orgId).members) {
                owners += member.role == "owner" && member.status == "confirmed";
            }
            const bool leaving = target && target == me;
            if (!target) {
                error(404, "not_found", "Member not found");
            } else if (!leaving && !isOrgAdmin(orgId)) {
                error(403, "forbidden", "Only owners and admins can remove members");
            } else if (!leaving && target->role == "owner" && me->role != "owner") {
                error(403, "forbidden", "Only owners can remove owners");
            } else if (target->role == "owner" && target->status == "confirmed" && owners <= 1) {
                error(409, "conflict", "An org needs at least one owner");
            } else {
                const auto subject = "user:" + target->userId;
                // Their keys of the org's vaults go, and every vault they could open needs a new key
                QJsonArray affected;
                for (auto& vault : server.vaults) {
                    if (vault.orgId != orgId) {
                        continue;
                    }
                    bool removed = false;
                    for (int i = vault.access.size() - 1; i >= 0; --i) {
                        if (vault.access[i].principal == subject) {
                            vault.access.removeAt(i);
                            removed = true;
                        }
                    }
                    if (removed) {
                        vault.rotationRequired = true;
                        affected << vault.id;
                        server.log(caller, "vault.access.revoke", subject, orgId);
                    }
                }
                dropMasterKeys(orgId, subject);
                auto& members = server.orgs[orgId].members;
                for (int i = members.size() - 1; i >= 0; --i) {
                    if (members[i].userId == parts.value(3)) {
                        members.removeAt(i);
                    }
                }
                server.log(caller, "org.member.remove", subject, orgId);
                reply(204);
            }
        } else if (parts.value(0) == "orgs" && !isOrgAdmin(parts.value(1))) {
            error(403, "forbidden", "Only owners and admins can do that");
        } else if (parts.value(0) == "orgs" && parts.value(2) == "members" && parts.size() == 4 && verb == "PATCH") {
            const auto orgId = parts.value(1);
            const auto role = json.value("role").toString();
            const auto* me = server.orgMember(orgId, caller.mid(5));
            auto* target = server.orgMember(orgId, parts.value(3));
            int owners = 0;
            for (const auto& member : server.orgs.value(orgId).members) {
                owners += member.role == "owner" && member.status == "confirmed";
            }
            const auto masterKeys = json.value("sa_master_keys");
            QString badKey;
            for (const auto& value : masterKeys.toArray()) {
                const auto grant = value.toObject();
                const auto sa = server.serviceAccounts.value(grant.value("sa_id").toString());
                if (grant.value("principal").toString() != "user:" + parts.value(3)) {
                    badKey = "sa_master_keys[].principal must be user:" + parts.value(3);
                } else if (sa.json.value("org_id").toString() != orgId) {
                    badKey = "sa_master_keys[].sa_id is not an SA of this org";
                }
            }
            if (!QStringList{"owner", "admin", "member"}.contains(role)) {
                error(400, "invalid_request", "role must be one of owner, admin, member");
            } else if (!target) {
                error(404, "not_found", "Member not found");
            } else if ((role == "owner" || target->role == "owner") && me->role != "owner") {
                error(403, "forbidden", "Only owners can make or change owners");
            } else if (target->role == "owner" && role != "owner" && target->status == "confirmed" && owners <= 1) {
                error(409, "conflict", "An org needs at least one owner");
            } else if (!masterKeys.isUndefined() && !masterKeys.isNull() && !isManagerRole(role)) {
                error(400, "invalid_request", "Only owners and admins hold SA master keys");
            } else if (!badKey.isEmpty()) {
                error(400, "invalid_request", badKey);
            } else {
                target->role = role;
                const auto subject = "user:" + target->userId;
                if (role == "member") {
                    dropMasterKeys(orgId, subject);
                }
                for (const auto& value : masterKeys.toArray()) {
                    const auto grant = value.toObject();
                    server.serviceAccounts[grant.value("sa_id").toString()].masterKeys.insert(
                        subject, grant.value("sealed_key").toString());
                }
                server.log(caller, "org.member.role", subject, orgId);
                reply(200, memberJson(*target));
            }
        } else if (parts.value(0) == "orgs" && parts.value(2) == "members" && parts.value(4) == "confirm") {
            auto& org = server.orgs[parts.value(1)];
            const auto principal = "user:" + parts.value(3);
            auto* member = server.orgMember(org.id, parts.value(3));
            if (!member) {
                error(404, "not_found", "Member not found");
                deliverLater(response, std::move(callback));
                return;
            }
            if (member->status != "accepted") {
                error(409, "conflict", "The member is " + member->status + ", not accepted");
                deliverLater(response, std::move(callback));
                return;
            }
            if (!server.directory.contains(principal)) {
                error(409, "conflict", "The member has no public keys");
                deliverLater(response, std::move(callback));
                return;
            }
            for (const auto& value : json.value("grants").toArray()) {
                const auto grant = value.toObject();
                const auto vaultId = grant.value("vault_id").toString();
                const auto vault = server.vaults.value(vaultId);
                const auto version = grant.value("key_version").toInt();
                if (grant.value("principal").toString() != principal) {
                    error(400, "invalid_request", "grants must be sealed to the member");
                    deliverLater(response, std::move(callback));
                    return;
                }
                if (!server.vaults.contains(vaultId) || vault.orgId != org.id) {
                    error(400, "invalid_request", "vault_id is not a vault of the org");
                    deliverLater(response, std::move(callback));
                    return;
                }
                if (version != vault.current && version != vault.pending) {
                    error(409, "conflict", "key_version is not current or pending");
                    deliverLater(response, std::move(callback));
                    return;
                }
            }
            member->status = "confirmed";
            for (const auto& value : json.value("grants").toArray()) {
                const auto grant = value.toObject();
                addGrants(server.vaults[grant.value("vault_id").toString()], QJsonArray{grant});
                server.log(caller, "vault.access.grant", principal, org.id);
            }
            for (const auto& value : json.value("sa_master_keys").toArray()) {
                const auto grant = value.toObject();
                server.serviceAccounts[grant.value("sa_id").toString()].masterKeys.insert(
                    grant.value("principal").toString(), grant.value("sealed_key").toString());
            }
            server.log(caller, "org.member.confirm", principal, org.id);
            reply(200, memberJson(*member));
        } else if (parts.value(0) == "orgs" && parts.value(2) == "invitations" && parts.size() == 3 && verb == "GET") {
            QJsonArray list;
            for (const auto& invitation : server.invitations) {
                if (invitation.orgId == parts.value(1) && !invitation.accepted
                    && invitation.expiresAt > QDateTime::currentDateTimeUtc()) {
                    list << invitationJson(invitation);
                }
            }
            reply(200, {{"invitations", list}});
        } else if (parts.value(0) == "orgs" && parts.value(2) == "invitations" && parts.size() == 3 && verb == "POST") {
            const auto orgId = parts.value(1);
            const auto email = json.value("email").toString().trimmed().toLower();
            const auto role = json.value("role").toString();
            const auto user = server.users.value(email);
            const auto* existing = user.id.isEmpty() ? nullptr : server.orgMember(orgId, user.id);
            if (!email.contains('@')) {
                error(400, "invalid_request", "email is not a valid address");
            } else if (!QStringList{"owner", "admin", "member"}.contains(role)) {
                error(400, "invalid_request", "role must be one of owner, admin, member");
            } else if (role == "owner" && server.orgMember(orgId, caller.mid(5))->role != "owner") {
                error(403, "forbidden", "Only owners can invite owners");
            } else if (existing && existing->status != "invited") {
                error(409, "conflict", "That account is already a member");
            } else {
                // kpx_inv_ + base64url(32 random bytes); only its hash is kept (SPEC §12)
                const auto token = "kpx_inv_" + toBase64Url(randomSecure(32));
                Server::Invitation invitation;
                invitation.id = uuid();
                invitation.orgId = orgId;
                invitation.email = email;
                invitation.role = role;
                invitation.tokenHash = toHex(sha256(token.toUtf8()));
                invitation.invitedBy = caller;
                invitation.expiresAt = QDateTime::currentDateTimeUtc().addDays(7);
                server.invitations.insert(invitation.id, invitation);
                if (!user.id.isEmpty() && !existing) {
                    server.orgs[orgId].members << Server::OrgMember{user.id, role, "invited"};
                }
                server.log(caller, "org.member.invite", email, orgId);
                server.sentInvitations << qMakePair(email, InvitationUrl + "#token=" + token);
                reply(201, invitationJson(invitation));
            }
        } else if (parts.value(0) == "orgs" && parts.value(2) == "invitations" && parts.size() == 4
                   && verb == "DELETE") {
            const auto invitation = server.invitations.value(parts.value(3));
            if (invitation.id.isEmpty() || invitation.orgId != parts.value(1) || invitation.accepted) {
                error(404, "not_found", "Invitation not found");
            } else {
                const auto userId = server.users.value(invitation.email).id;
                auto& members = server.orgs[invitation.orgId].members;
                for (int i = members.size() - 1; i >= 0; --i) {
                    if (!userId.isEmpty() && members[i].userId == userId && members[i].status == "invited") {
                        members.removeAt(i);
                    }
                }
                server.invitations.remove(invitation.id);
                server.log(caller, "org.invitation.cancel", invitation.email, invitation.orgId);
                reply(204);
            }
        } else if (parts.value(0) == "orgs" && parts.value(2) == "service-accounts" && verb == "GET") {
            QJsonArray list;
            for (const auto& sa : server.serviceAccounts) {
                if (sa.json.value("org_id").toString() == parts.value(1)) {
                    list << sa.json;
                }
            }
            reply(200, {{"service_accounts", list}});
        } else if (parts.value(0) == "orgs" && parts.value(2) == "service-accounts" && verb == "POST") {
            Server::ServiceAccount sa;
            const auto id = json.value("id").toString();
            auto keyset = json.value("keyset").toObject();
            auto publicKeys = keyset;
            publicKeys.remove("wrapped");
            sa.json = {{"id", id},
                       {"org_id", parts.value(1)},
                       {"name", json.value("name").toString()},
                       {"public", publicKeys},
                       {"keyset", keyset},
                       {"created_by", caller},
                       {"created_at", now()},
                       {"expires_at", json.value("expires_at")},
                       {"disabled", false}};
            for (const auto& value : json.value("master_keys").toArray()) {
                const auto grant = value.toObject();
                sa.masterKeys.insert(grant.value("principal").toString(), grant.value("sealed_key").toString());
            }
            server.serviceAccounts.insert(id, sa);
            auto entry = publicKeys;
            entry["principal"] = "sa:" + id;
            entry["name"] = json.value("name").toString();
            server.directory.insert("sa:" + id, entry);
            const auto secret = json.value("secret").toObject();
            server.secrets.insert(secret.value("id").toString(),
                                  {secret.value("id").toString(),
                                   id,
                                   secret.value("name").toString(),
                                   secret.value("secret_hash").toString(),
                                   secret.value("prefix").toString(),
                                   secret.value("wrapped_master").toString(),
                                   secret.value("expires_at")});
            server.log(caller, "sa.create", "sa:" + id);
            reply(201, sa.json);
        } else if (parts.value(0) == "orgs" && parts.value(2) == "audit") {
            const auto before = QUrlQuery(url).queryItemValue("before");
            const auto limit = QUrlQuery(url).queryItemValue("limit").toInt();
            QJsonArray page;
            bool started = before.isEmpty();
            QString next;
            for (const auto& value : server.audit) {
                const auto id = value.toObject().value("id").toString();
                const auto org = value.toObject().value("org").toString();
                if (!org.isEmpty() && org != parts.value(1)) {
                    continue;
                }
                if (!started) {
                    started = id == before;
                    continue;
                }
                if (page.size() == limit) {
                    next = page.last().toObject().value("id").toString();
                    break;
                }
                page << value;
            }
            reply(200, {{"events", page}, {"next", next.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(next)}});
        } else if (parts.value(0) == "service-accounts" && !server.serviceAccounts.contains(parts.value(1))) {
            error(404, "not_found");
        } else if (parts.value(0) == "service-accounts"
                   && !isOrgAdmin(server.serviceAccounts.value(parts.value(1)).json.value("org_id").toString())) {
            error(403, "forbidden");
        } else if (parts.value(0) == "service-accounts" && parts.size() == 2 && verb == "GET") {
            const auto sa = server.serviceAccounts.value(parts.value(1));
            auto result = sa.json;
            if (sa.masterKeys.contains(caller)) {
                result["master_key"] = sa.masterKeys.value(caller);
            }
            reply(200, result);
        } else if (parts.value(0) == "service-accounts" && parts.size() == 2 && verb == "PATCH") {
            auto& sa = server.serviceAccounts[parts.value(1)];
            for (const auto& key : json.keys()) {
                sa.json[key] = json.value(key);
            }
            server.log(caller, "sa.update", "sa:" + parts.value(1));
            reply(200, sa.json);
        } else if (parts.value(0) == "service-accounts" && parts.size() == 2 && verb == "DELETE") {
            server.serviceAccounts.remove(parts.value(1));
            for (auto& vault : server.vaults) {
                for (int i = vault.access.size() - 1; i >= 0; --i) {
                    if (vault.access[i].principal == "sa:" + parts.value(1)) {
                        vault.access.removeAt(i);
                        vault.rotationRequired = true;
                    }
                }
            }
            reply(204);
        } else if (parts.value(0) == "service-accounts" && parts.value(2) == "secrets" && parts.size() == 3
                   && verb == "GET") {
            QJsonArray list;
            for (const auto& secret : server.secrets) {
                if (secret.saId == parts.value(1)) {
                    list << QJsonObject{{"id", secret.id},
                                        {"prefix", secret.prefix},
                                        {"name", secret.name},
                                        {"created_at", now()},
                                        {"last_used_at", QJsonValue(QJsonValue::Null)},
                                        {"expires_at", secret.expiresAt}};
                }
            }
            reply(200, {{"secrets", list}});
        } else if (parts.value(0) == "service-accounts" && parts.value(2) == "secrets" && verb == "POST") {
            server.secrets.insert(json.value("id").toString(),
                                  {json.value("id").toString(),
                                   parts.value(1),
                                   json.value("name").toString(),
                                   json.value("secret_hash").toString(),
                                   json.value("prefix").toString(),
                                   json.value("wrapped_master").toString(),
                                   json.value("expires_at")});
            server.log(caller, "sa.secret.issue", "sa:" + parts.value(1));
            reply(201);
        } else if (parts.value(0) == "service-accounts" && parts.value(2) == "secrets" && verb == "PATCH") {
            // SPEC.md §9: only the server's expiry changes, the token stays the same
            if (!server.secrets.contains(parts.value(3)) || server.secrets.value(parts.value(3)).saId != parts.value(1)) {
                error(404, "not_found");
            } else if (!json.contains("expires_at")) {
                error(400, "invalid_request");
            } else {
                auto& secret = server.secrets[parts.value(3)];
                secret.expiresAt = json.value("expires_at");
                server.log(caller, "sa.secret.update", "sa:" + parts.value(1));
                reply(200,
                      {{"id", secret.id},
                       {"prefix", secret.prefix},
                       {"name", secret.name},
                       {"created_at", now()},
                       {"last_used_at", QJsonValue(QJsonValue::Null)},
                       {"expires_at", secret.expiresAt}});
            }
        } else if (parts.value(0) == "service-accounts" && parts.value(2) == "secrets" && verb == "DELETE") {
            server.secrets.remove(parts.value(3));
            server.log(caller, "sa.secret.revoke", "sa:" + parts.value(1));
            reply(204);
        } else {
            error(404, "not_found");
        }
        deliverLater(response, std::move(callback));
    }

    FakeSyncClient::FakeSyncClient(const RemoteParams& params, QSharedPointer<Server> server, QObject* parent)
        : SyncServerClient(params, parent)
        , m_server(std::move(server))
    {
    }

    void FakeSyncClient::send(const QByteArray& verb,
                              const QUrl& url,
                              const Headers& headers,
                              const QByteArray& body,
                              int,
                              Callback callback)
    {
        auto& server = *m_server;
        auto path = url.path();
        path = path.mid(path.indexOf("/api/v1/") + QString("/api/v1/").size());
        server.requests << "sync " + QString::fromLatin1(verb) + " " + path;
        QString token, ifMatch, ifNoneMatch;
        for (const auto& header : headers) {
            if (header.first == "Authorization") {
                token = QString::fromLatin1(header.second).mid(QString("Bearer ").size());
            } else if (header.first == "If-Match") {
                ifMatch = QString::fromLatin1(header.second);
            } else if (header.first == "If-None-Match") {
                ifNoneMatch = QString::fromLatin1(header.second);
            }
        }
        server.syncPrincipals << server.accessTokens.value(token).principal;
        Response response;
        const auto parts = path.split('/');
        const auto blobRequest = [&](Server::Blob* blob, bool exists, bool vault) {
            if (verb == "PUT") {
                if (vault && ifMatch.isEmpty() && ifNoneMatch.isEmpty()) {
                    response.status = 428;
                } else if ((ifNoneMatch == "*" && exists) || (!ifMatch.isEmpty() && ifMatch != blob->etag)) {
                    response.status = 412;
                } else if (vault && !ifMatch.isEmpty() && server.conflictsToInject > 0) {
                    --server.conflictsToInject;
                    response.status = 412;
                } else {
                    response.status = exists ? 200 : 201;
                    blob->data = body;
                    blob->etag = etagFor(body);
                    ++blob->revision;
                }
            } else if (!exists) {
                response.status = 404;
            } else if (verb == "HEAD" && ifNoneMatch == blob->etag) {
                response.status = 304;
            } else {
                response.status = 200;
                if (verb == "GET") {
                    response.body = blob->data;
                }
            }
            if (response.status / 100 == 2 || response.status == 304) {
                response.etag = blob->etag;
                response.revision = blob->revision;
            }
        };

        if (path == "health") {
            response.status = 200;
            response.body = R"({"status":"ok"})";
        } else if (path == "devices/pairing/claim") {
            const auto code = QJsonDocument::fromJson(body).object().value("code").toString();
            if (!server.pairingCodes.contains(code)) {
                response.status = 404;
                response.body = R"({"error":"unknown or expired pairing code"})";
            } else {
                const auto deviceToken = server.pairingCodes.take(code);
                server.deviceTokens.insert(deviceToken);
                response.status = 201;
                response.body =
                    QJsonDocument(QJsonObject{{"user", "alice"}, {"deviceId", "dev-1"}, {"token", deviceToken}})
                        .toJson(QJsonDocument::Compact);
            }
        } else if (parts.value(0) == "vaults" && parts.size() == 2) {
            const auto vaultId = parts.value(1);
            const auto access = server.accessTokens.value(token);
            const auto role = access.vaults.value(vaultId);
            const bool exists = server.blobs.contains(vaultId);
            if (access.principal.isEmpty()) {
                response.status = 401;
            } else if (role.isEmpty() || (verb == "PUT" && roleRank(role) < roleRank(exists ? "write" : "manage"))) {
                response.status = 403;
                response.body = R"({"error":"vault not in token","code":"forbidden"})";
            } else {
                blobRequest(&server.blobs[vaultId], exists, true);
                if (!exists && response.status != 201) {
                    server.blobs.remove(vaultId);
                }
            }
        } else if (parts.value(0) == "databases" && parts.size() == 2) {
            const bool exists = server.databases.contains(parts.value(1));
            if (!server.deviceTokens.contains(token)) {
                response.status = 401;
            } else {
                blobRequest(&server.databases[parts.value(1)], exists, false);
                if (!exists && response.status != 201) {
                    server.databases.remove(parts.value(1));
                }
            }
        } else {
            response.status = 404;
        }
        deliverLater(response, std::move(callback));
    }

    AccountManager* createManager(const QSharedPointer<Server>& server, QObject* parent)
    {
        auto* manager = new AccountManager(new FakeAuthClient(server), std::make_unique<FakeOpaqueClient>(), parent);
        manager->setNewAccountKdf(testKdf());
        manager->setEventStreamEnabled(false);
        return manager;
    }

    void installSyncFactory(const QSharedPointer<Server>& server)
    {
        SyncServerClient::setFactory(
            [server](const RemoteParams& params) -> SyncServerClient* { return new FakeSyncClient(params, server); });
    }

    void restoreSyncFactory()
    {
        SyncServerClient::setFactory(nullptr);
    }
} // namespace FakeAccount
