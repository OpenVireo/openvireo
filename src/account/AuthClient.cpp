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

#include "AuthClient.h"

#include "AccountCrypto.h"

#include <QJsonDocument>
#include <QPointer>
#include <QSysInfo>
#include <QThread>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#endif

namespace
{
    QString encoded(const QString& pathPart)
    {
        return QString::fromLatin1(QUrl::toPercentEncoding(pathPart));
    }
} // namespace

QJsonObject AuthClient::Response::json() const
{
    return QJsonDocument::fromJson(body).object();
}

QString AuthClient::Response::errorCode() const
{
    return json().value("code").toString();
}

QJsonObject AuthClient::DeviceInfo::toJson() const
{
    QJsonObject json{{"name", name.left(64)}, {"platform", platform}};
    if (!id.isEmpty()) {
        json["id"] = id;
    }
    return json;
}

AuthClient::DeviceInfo AuthClient::DeviceInfo::current(const QString& id)
{
    DeviceInfo device;
    device.id = id;
    device.name = QSysInfo::machineHostName();
    if (device.name.isEmpty()) {
        device.name = QSysInfo::prettyProductName();
    }
#if defined(Q_OS_MACOS)
    device.platform = "qt-macos";
#elif defined(Q_OS_WIN)
    device.platform = "qt-windows";
#else
    device.platform = "qt-linux";
#endif
    return device;
}

AuthClient::AuthClient(const QString& apiUrl, QObject* parent)
    : QObject(parent)
{
    setApiUrl(apiUrl);
}

AuthClient::~AuthClient()
{
    abort();
}

void AuthClient::setApiUrl(const QString& apiUrl)
{
    m_apiUrl = apiUrl.trimmed();
    while (m_apiUrl.endsWith('/')) {
        m_apiUrl.chop(1);
    }
}

QString AuthClient::apiUrl() const
{
    return m_apiUrl;
}

void AuthClient::setTokenProvider(AccessTokenProvider provider)
{
    m_tokenProvider = std::move(provider);
}

QUrl AuthClient::endpoint(const QString& path) const
{
    return QUrl(m_apiUrl + "/" + path);
}

QJsonObject
AuthClient::grant(const QString& principal, const QString& role, int keyVersion, const QByteArray& sealedKey)
{
    return QJsonObject{{"principal", principal},
                       {"role", role},
                       {"key_version", keyVersion},
                       {"sealed_key", AccountCrypto::toBase64Url(sealedKey)}};
}

void AuthClient::discover(const QString& issuerUrl, Callback callback)
{
    auto base = issuerUrl.trimmed();
    while (base.endsWith('/')) {
        base.chop(1);
    }
    request("GET", QUrl(base + "/.well-known/openvireo-configuration"), {}, false, std::move(callback));
}

void AuthClient::registerStart(const QString& email, const QByteArray& request, Callback callback)
{
    post("auth/register/start",
         {{"email", email.trimmed()}, {"request", AccountCrypto::toBase64Url(request)}},
         std::move(callback));
}

void AuthClient::registerFinish(const QString& sessionId,
                                const QByteArray& record,
                                const QJsonObject& kdf,
                                const QJsonObject& keyset,
                                const DeviceInfo& device,
                                Callback callback)
{
    post("auth/register/finish",
         {{"session_id", sessionId},
          {"record", AccountCrypto::toBase64Url(record)},
          {"kdf", kdf},
          {"keyset", keyset},
          {"device", device.toJson()}},
         std::move(callback));
}

void AuthClient::loginStart(const QString& email, const QByteArray& ke1, Callback callback)
{
    post("auth/login/start",
         {{"email", email.trimmed()}, {"ke1", AccountCrypto::toBase64Url(ke1)}},
         std::move(callback));
}

void AuthClient::loginFinish(const QString& sessionId,
                             const QByteArray& authU,
                             const DeviceInfo& device,
                             Callback callback)
{
    post("auth/login/finish",
         {{"session_id", sessionId}, {"auth_u", AccountCrypto::toBase64Url(authU)}, {"device", device.toJson()}},
         std::move(callback));
}

void AuthClient::loginMfa(const QString& mfaToken, const QString& totp, Callback callback)
{
    post("auth/login/mfa", {{"mfa_token", mfaToken}, {"totp", totp.trimmed()}}, std::move(callback));
}

void AuthClient::refresh(const QString& refreshToken, Callback callback)
{
    post("auth/refresh", {{"refresh_token", refreshToken}}, std::move(callback));
}

void AuthClient::logout(Callback callback)
{
    post("auth/logout", {}, std::move(callback), true);
}

void AuthClient::exchangeServiceAccount(const QString& saId, const QString& secret, Callback callback)
{
    post("auth/service-account", {{"sa_id", saId}, {"secret", secret}}, std::move(callback));
}

void AuthClient::me(Callback callback)
{
    get("me", std::move(callback));
}

void AuthClient::lookupUser(const QString& email, Callback callback)
{
    get("users/lookup?email=" + encoded(email.trimmed()), std::move(callback));
}

void AuthClient::publicKey(const QString& principal, Callback callback)
{
    get("principals/" + encoded(principal) + "/public-key", std::move(callback));
}

void AuthClient::listDevices(Callback callback)
{
    get("devices", std::move(callback));
}

void AuthClient::revokeDevice(const QString& deviceId, Callback callback)
{
    request("DELETE", endpoint("devices/" + encoded(deviceId)), {}, true, std::move(callback));
}

void AuthClient::listVaults(Callback callback)
{
    get("vaults", std::move(callback));
}

void AuthClient::getVault(const QString& vaultId, Callback callback)
{
    get("vaults/" + encoded(vaultId), std::move(callback));
}

void AuthClient::createVault(const QString& vaultId,
                             const QString& orgId,
                             const QByteArray& encryptedName,
                             const QJsonArray& grants,
                             Callback callback)
{
    QJsonObject body{
        {"id", vaultId}, {"encrypted_name", AccountCrypto::toBase64Url(encryptedName)}, {"grants", grants}};
    body["org_id"] = orgId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(orgId);
    post("vaults", body, std::move(callback), true);
}

void AuthClient::renameVault(const QString& vaultId, const QByteArray& encryptedName, Callback callback)
{
    QJsonObject body{{"encrypted_name", AccountCrypto::toBase64Url(encryptedName)}};
    request("PATCH",
            endpoint("vaults/" + encoded(vaultId)),
            QJsonDocument(body).toJson(QJsonDocument::Compact),
            true,
            std::move(callback));
}

void AuthClient::deleteVault(const QString& vaultId, Callback callback)
{
    request("DELETE", endpoint("vaults/" + encoded(vaultId)), {}, true, std::move(callback));
}

void AuthClient::vaultAccess(const QString& vaultId, Callback callback)
{
    get("vaults/" + encoded(vaultId) + "/access", std::move(callback));
}

void AuthClient::grantAccess(const QString& vaultId, const QJsonArray& grants, Callback callback)
{
    post("vaults/" + encoded(vaultId) + "/access", {{"grants", grants}}, std::move(callback), true);
}

void AuthClient::startRotation(const QString& vaultId,
                               int keyVersion,
                               const QJsonArray& grants,
                               const QStringList& remove,
                               Callback callback)
{
    QJsonObject body{{"key_version", keyVersion}, {"grants", grants}};
    if (!remove.isEmpty()) {
        body["remove"] = QJsonArray::fromStringList(remove);
    }
    post("vaults/" + encoded(vaultId) + "/keys", body, std::move(callback), true);
}

void AuthClient::activateKey(const QString& vaultId, int keyVersion, Callback callback)
{
    post("vaults/" + encoded(vaultId) + "/keys/" + QString::number(keyVersion) + "/activate",
         {},
         std::move(callback),
         true);
}

void AuthClient::createOrg(const QString& name, Callback callback)
{
    post("orgs", {{"name", name}}, std::move(callback), true);
}

void AuthClient::orgMembers(const QString& orgId, Callback callback)
{
    get("orgs/" + encoded(orgId) + "/members", std::move(callback));
}

void AuthClient::setMemberRole(const QString& orgId,
                               const QString& userId,
                               const QString& role,
                               const QJsonArray& saMasterKeys,
                               Callback callback)
{
    QJsonObject body{{"role", role}};
    if (!saMasterKeys.isEmpty()) {
        body["sa_master_keys"] = saMasterKeys;
    }
    sendJson("PATCH", "orgs/" + encoded(orgId) + "/members/" + encoded(userId), body, std::move(callback));
}

void AuthClient::removeMember(const QString& orgId, const QString& userId, Callback callback)
{
    request("DELETE",
            endpoint("orgs/" + encoded(orgId) + "/members/" + encoded(userId)),
            {},
            true,
            std::move(callback));
}

void AuthClient::listInvitations(const QString& orgId, Callback callback)
{
    get("orgs/" + encoded(orgId) + "/invitations", std::move(callback));
}

void AuthClient::invite(const QString& orgId, const QString& email, const QString& role, Callback callback)
{
    post("orgs/" + encoded(orgId) + "/invitations", {{"email", email}, {"role", role}}, std::move(callback), true);
}

void AuthClient::cancelInvitation(const QString& orgId, const QString& invitationId, Callback callback)
{
    request("DELETE",
            endpoint("orgs/" + encoded(orgId) + "/invitations/" + encoded(invitationId)),
            {},
            true,
            std::move(callback));
}

void AuthClient::acceptInvitation(const QString& token, Callback callback)
{
    post("invitations/accept", {{"token", token}}, std::move(callback), true);
}

void AuthClient::confirmMember(const QString& orgId,
                               const QString& userId,
                               const QJsonArray& grants,
                               const QJsonArray& saMasterKeys,
                               Callback callback)
{
    QJsonObject body{{"grants", grants}};
    if (!saMasterKeys.isEmpty()) {
        body["sa_master_keys"] = saMasterKeys;
    }
    post("orgs/" + encoded(orgId) + "/members/" + encoded(userId) + "/confirm", body, std::move(callback), true);
}

void AuthClient::listServiceAccounts(const QString& orgId, Callback callback)
{
    get("orgs/" + encoded(orgId) + "/service-accounts", std::move(callback));
}

void AuthClient::createServiceAccount(const QString& orgId, const QJsonObject& body, Callback callback)
{
    post("orgs/" + encoded(orgId) + "/service-accounts", body, std::move(callback), true);
}

void AuthClient::getServiceAccount(const QString& saId, Callback callback)
{
    get("service-accounts/" + encoded(saId), std::move(callback));
}

void AuthClient::updateServiceAccount(const QString& saId, const QJsonObject& changes, Callback callback)
{
    sendJson("PATCH", "service-accounts/" + encoded(saId), changes, std::move(callback));
}

void AuthClient::deleteServiceAccount(const QString& saId, Callback callback)
{
    request("DELETE", endpoint("service-accounts/" + encoded(saId)), {}, true, std::move(callback));
}

void AuthClient::listServiceAccountSecrets(const QString& saId, Callback callback)
{
    get("service-accounts/" + encoded(saId) + "/secrets", std::move(callback));
}

void AuthClient::issueServiceAccountSecret(const QString& saId, const QJsonObject& secret, Callback callback)
{
    post("service-accounts/" + encoded(saId) + "/secrets", secret, std::move(callback), true);
}

void AuthClient::updateServiceAccountSecret(const QString& saId,
                                            const QString& secretId,
                                            const QJsonObject& changes,
                                            Callback callback)
{
    sendJson("PATCH", "service-accounts/" + encoded(saId) + "/secrets/" + encoded(secretId), changes, std::move(callback));
}

void AuthClient::revokeServiceAccountSecret(const QString& saId, const QString& secretId, Callback callback)
{
    request("DELETE",
            endpoint("service-accounts/" + encoded(saId) + "/secrets/" + encoded(secretId)),
            {},
            true,
            std::move(callback));
}

void AuthClient::auditLog(const QString& orgId, const QString& before, int limit, Callback callback)
{
    auto path = "orgs/" + encoded(orgId) + "/audit?limit=" + QString::number(limit);
    if (!before.isEmpty()) {
        path += "&before=" + encoded(before);
    }
    get(path, std::move(callback));
}

void AuthClient::post(const QString& path, const QJsonObject& body, Callback callback, bool authenticated)
{
    request(
        "POST", endpoint(path), QJsonDocument(body).toJson(QJsonDocument::Compact), authenticated, std::move(callback));
}

void AuthClient::get(const QString& path, Callback callback)
{
    request("GET", endpoint(path), {}, true, std::move(callback));
}

void AuthClient::sendJson(const QByteArray& verb, const QString& path, const QJsonObject& body, Callback callback)
{
    request(verb, endpoint(path), QJsonDocument(body).toJson(QJsonDocument::Compact), true, std::move(callback));
}

void AuthClient::request(const QByteArray& verb,
                         const QUrl& url,
                         const QByteArray& body,
                         bool authenticated,
                         Callback callback)
{
    Headers headers{{"Accept", "application/json"}};
    if (!body.isEmpty() || verb == "POST") {
        headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    }
    const auto generation = m_generation;
    auto proceed = [this, generation, verb, url, headers, body, callback](const QString& token) {
        if (generation != m_generation) {
            return;
        }
        auto withToken = headers;
        if (!token.isEmpty()) {
            withToken.prepend(qMakePair(QByteArray("Authorization"), "Bearer " + token.toUtf8()));
        }
        send(verb, url, withToken, body.isEmpty() && verb == "POST" ? QByteArray("{}") : body, TimeoutMsec, callback);
    };
    if (!authenticated || !m_tokenProvider) {
        proceed({});
        return;
    }
    QPointer<AuthClient> guard(this);
    auto* thread = QThread::currentThread();
    m_tokenProvider([guard, thread, proceed](const QString& token) {
        if (QThread::currentThread() == thread) {
            if (guard) {
                proceed(token);
            }
        } else if (auto* client = guard.data()) {
            QMetaObject::invokeMethod(
                client,
                [guard, proceed, token] {
                    if (guard) {
                        proceed(token);
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

void AuthClient::abort()
{
    ++m_generation;
#ifdef KPXC_FEATURE_NETWORK
    const auto replies = m_replies;
    m_replies.clear();
    for (auto* reply : replies) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
#endif
}

void AuthClient::deliverLater(const Response& response, Callback callback)
{
    const auto generation = m_generation;
    QMetaObject::invokeMethod(
        this,
        [this, generation, response, callback] {
            if (generation == m_generation) {
                callback(response);
            }
        },
        Qt::QueuedConnection);
}

void AuthClient::send(const QByteArray& verb,
                      const QUrl& url,
                      const Headers& headers,
                      const QByteArray& body,
                      int timeoutMsec,
                      Callback callback)
{
    Response response;
#ifdef KPXC_FEATURE_NETWORK
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
        response.networkError = tr("Invalid account server URL: %1").arg(url.toString());
        deliverLater(response, std::move(callback));
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "OpenVireo");
    // Never follow redirects, they could forward tokens to another host
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(timeoutMsec);
    for (const auto& header : headers) {
        request.setRawHeader(header.first, header.second);
    }

    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    auto* reply = m_nam->sendCustomRequest(request, verb, body);
    m_replies.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
        m_replies.remove(reply);
        reply->deleteLater();

        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        if (response.status == 0) {
            response.networkError = reply->errorString();
        }
        callback(response);
    });
#else
    Q_UNUSED(verb)
    Q_UNUSED(url)
    Q_UNUSED(headers)
    Q_UNUSED(body)
    Q_UNUSED(timeoutMsec)
    response.networkError = tr("This build of OpenVireo was compiled without network support.");
    deliverLater(response, std::move(callback));
#endif
}

QString AuthClient::errorMessage(const Response& response)
{
    if (response.status == 0) {
        return tr("Could not reach the account server: %1").arg(response.networkError);
    }
    if (response.status == 429) {
        return tr("Too many attempts. Wait a minute and try again.");
    }
    if (response.status >= 300 && response.status < 400) {
        return tr("The account server responded with a redirect, check the server URL.");
    }
    auto message = response.json().value("error").toString();
    if (message.isEmpty()) {
        message = tr("Unexpected response.");
    }
    return tr("The account server responded with HTTP %1: %2").arg(response.status).arg(message);
}
