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

#include "SyncServerClient.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#endif

namespace
{
    std::function<SyncServerClient*(const RemoteParams&)> g_factory;

    // Account id -> the account's token provider; read from worker threads too (RemoteHandler)
    QMutex g_accountProvidersMutex;
    QHash<QString, AccessTokenProvider> g_accountProviders;
    // Vault id -> the account on this device that holds it (same mutex)
    QHash<QString, QString> g_vaultAccounts;

    // The provider for an account vault; call with the mutex held
    AccessTokenProvider accountProviderLocked(const QString& accountId, const QString& vaultId)
    {
        const auto holder = g_vaultAccounts.value(vaultId);
        if (!holder.isEmpty() && g_accountProviders.contains(holder)) {
            return g_accountProviders.value(holder);
        }
        return g_accountProviders.value(accountId);
    }
} // namespace

SyncServerClient::SyncServerClient(const RemoteParams& params, QObject* parent)
    : QObject(parent)
    , m_params(params)
    , m_tokenProvider(tokenProviderFor(params))
{
}

SyncServerClient* SyncServerClient::create(const RemoteParams& params, QObject* parent)
{
    auto* client = g_factory ? g_factory(params) : new SyncServerClient(params);
    if (parent) {
        client->setParent(parent);
    }
    return client;
}

void SyncServerClient::setFactory(std::function<SyncServerClient*(const RemoteParams&)> factory)
{
    g_factory = std::move(factory);
}

void SyncServerClient::setTokenProvider(AccessTokenProvider provider)
{
    m_tokenProvider = std::move(provider);
}

AccessTokenProvider SyncServerClient::tokenProviderFor(const RemoteParams& params)
{
    if (params.tokenProvider) {
        return params.tokenProvider;
    }
    if (params.type == RemoteParams::Type::AccountVault) {
        const auto accountId = params.accountId;
        const auto vaultId = params.vaultId;
        // Looked up at each request, so signing in later or again takes effect for existing clients
        return [accountId, vaultId](std::function<void(const QString&)> done) {
            AccessTokenProvider provider;
            {
                QMutexLocker locker(&g_accountProvidersMutex);
                provider = accountProviderLocked(accountId, vaultId);
            }
            if (provider) {
                provider(std::move(done));
            } else {
                done({});
            }
        };
    }
    const auto token = params.serverToken.trimmed();
    return [token](std::function<void(const QString&)> done) { done(token); };
}

bool SyncServerClient::hasCredentials(const RemoteParams& params)
{
    if (params.tokenProvider) {
        return true;
    }
    if (params.type == RemoteParams::Type::AccountVault) {
        QMutexLocker locker(&g_accountProvidersMutex);
        return static_cast<bool>(accountProviderLocked(params.accountId, params.vaultId));
    }
    return !params.serverToken.trimmed().isEmpty();
}

void SyncServerClient::setAccountTokenProvider(const QString& accountId, AccessTokenProvider provider)
{
    QMutexLocker locker(&g_accountProvidersMutex);
    if (provider) {
        g_accountProviders.insert(accountId, std::move(provider));
    } else {
        g_accountProviders.remove(accountId);
    }
}

void SyncServerClient::setVaultAccount(const QString& vaultId, const QString& accountId)
{
    QMutexLocker locker(&g_accountProvidersMutex);
    if (accountId.isEmpty()) {
        g_vaultAccounts.remove(vaultId);
    } else {
        g_vaultAccounts.insert(vaultId, accountId);
    }
}

SyncServerClient::~SyncServerClient()
{
    abort();
}

const RemoteParams& SyncServerClient::params() const
{
    return m_params;
}

QUrl SyncServerClient::endpoint(const RemoteParams& params, const QString& path)
{
    auto base = params.serverUrl.trimmed();
    while (base.endsWith('/')) {
        base.chop(1);
    }
    // Discovery hands out the sync server's API URL, which already ends in /api/v1
    if (!base.endsWith("/api/v1")) {
        base += "/api/v1";
    }
    return QUrl(base + "/" + path);
}

QUrl SyncServerClient::databaseUrl(const QString& suffix) const
{
    // Account vaults live in their own namespace on the server, never mixed with a user's databases
    const auto collection = m_params.type == RemoteParams::Type::AccountVault ? "vaults/" : "databases/";
    return endpoint(m_params,
                    collection + QString::fromLatin1(QUrl::toPercentEncoding(m_params.remoteDatabaseId())) + suffix);
}

void SyncServerClient::check(const QString& etag, Callback callback)
{
    Headers headers;
    if (!etag.isEmpty()) {
        headers.append(qMakePair(QByteArray("If-None-Match"), etag.toLatin1()));
    }
    request("HEAD", databaseUrl(), headers, {}, m_params.downloadTimeoutMsec, std::move(callback));
}

void SyncServerClient::download(Callback callback)
{
    request("GET", databaseUrl(), {}, {}, m_params.downloadTimeoutMsec, std::move(callback));
}

void SyncServerClient::upload(const QByteArray& data, const QString& etag, Callback callback)
{
    Headers headers{{"Content-Type", "application/octet-stream"}};
    // Only overwrite the exact version we merged with, or only create if we saw none
    if (etag.isEmpty()) {
        headers.append(qMakePair(QByteArray("If-None-Match"), QByteArray("*")));
    } else {
        headers.append(qMakePair(QByteArray("If-Match"), etag.toLatin1()));
    }
    request("PUT", databaseUrl(), headers, data, m_params.uploadTimeoutMsec, std::move(callback));
}

void SyncServerClient::listDatabases(Callback callback)
{
    const auto collection = m_params.type == RemoteParams::Type::AccountVault ? "vaults" : "databases";
    request("GET", endpoint(m_params, collection), {}, {}, m_params.downloadTimeoutMsec, std::move(callback));
}

void SyncServerClient::history(Callback callback)
{
    request("GET", databaseUrl("/history"), {}, {}, m_params.downloadTimeoutMsec, std::move(callback));
}

void SyncServerClient::downloadRevision(qint64 revision, Callback callback)
{
    request("GET",
            databaseUrl("/history/" + QString::number(revision)),
            {},
            {},
            m_params.downloadTimeoutMsec,
            std::move(callback));
}

void SyncServerClient::createPairingCode(Callback callback)
{
    request("POST",
            endpoint(m_params, "devices/pairing"),
            {{"Content-Type", "application/json"}},
            "{}",
            m_params.downloadTimeoutMsec,
            std::move(callback));
}

void SyncServerClient::health(Callback callback)
{
    send("GET", endpoint(m_params, "health"), {}, {}, m_params.downloadTimeoutMsec, std::move(callback));
}

void SyncServerClient::claimPairingCode(const QString& code, const QString& deviceName, Callback callback)
{
    QJsonObject body{{"code", code.trimmed()}, {"deviceName", deviceName}};
    request("POST",
            endpoint(m_params, "devices/pairing/claim"),
            {{"Content-Type", "application/json"}},
            QJsonDocument(body).toJson(QJsonDocument::Compact),
            m_params.downloadTimeoutMsec,
            std::move(callback));
}

void SyncServerClient::abort()
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

void SyncServerClient::deliverLater(const Response& response, Callback callback)
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

void SyncServerClient::request(const QByteArray& verb,
                               const QUrl& url,
                               const Headers& headers,
                               const QByteArray& body,
                               int timeoutMsec,
                               Callback callback)
{
    const auto generation = m_generation;
    auto proceed = [this, generation, verb, url, headers, body, timeoutMsec, callback](const QString& token) {
        if (generation != m_generation) {
            return;
        }
        auto withToken = headers;
        if (!token.trimmed().isEmpty()) {
            withToken.prepend(qMakePair(QByteArray("Authorization"), "Bearer " + token.trimmed().toUtf8()));
        }
        send(verb, url, withToken, body, timeoutMsec, callback);
    };
    if (!m_tokenProvider) {
        proceed({});
        return;
    }
    // The provider may answer later and on another thread (an account refreshing its token)
    QPointer<SyncServerClient> guard(this);
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

void SyncServerClient::send(const QByteArray& verb,
                            const QUrl& url,
                            const Headers& headers,
                            const QByteArray& body,
                            int timeoutMsec,
                            Callback callback)
{
    Response response;
#ifdef KPXC_FEATURE_NETWORK
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
        response.networkError = tr("Invalid sync server URL: %1").arg(m_params.serverUrl);
        deliverLater(response, std::move(callback));
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "OpenVireo");
    // Never follow redirects, they could forward the access token to another host
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
        response.etag = QString::fromLatin1(reply->rawHeader("ETag"));
        bool ok = false;
        auto revision = reply->rawHeader("X-Database-Revision").toLongLong(&ok);
        if (ok) {
            response.revision = revision;
        }
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

QString SyncServerClient::errorMessage(const Response& response)
{
    if (response.status == 0) {
        return tr("Could not reach the sync server: %1").arg(response.networkError);
    }
    if (response.status == 401 || response.status == 403) {
        return tr("The sync server rejected the access token.");
    }
    if (response.status >= 300 && response.status < 400) {
        return tr("The sync server responded with a redirect, check the server URL.");
    }
    auto message = QJsonDocument::fromJson(response.body).object().value("error").toString();
    if (message.isEmpty()) {
        message = tr("Unexpected response.");
    }
    return tr("The sync server responded with HTTP %1: %2").arg(response.status).arg(message);
}

bool SyncServerClient::isPairingCode(const QString& text)
{
    static const QRegularExpression code("^[A-Z2-9]{4}[- ]?[A-Z2-9]{4}$", QRegularExpression::CaseInsensitiveOption);
    return code.match(text.trimmed()).hasMatch();
}

QString SyncServerClient::etagFor(const QByteArray& data)
{
    return "\"" + QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) + "\"";
}
