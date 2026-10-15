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
#include <QJsonDocument>
#include <QJsonObject>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#endif

SyncServerClient::SyncServerClient(const RemoteParams& params, QObject* parent)
    : QObject(parent)
    , m_params(params)
{
}

SyncServerClient::~SyncServerClient()
{
    abort();
}

const RemoteParams& SyncServerClient::params() const
{
    return m_params;
}

QUrl SyncServerClient::databaseUrl() const
{
    auto base = m_params.serverUrl.trimmed();
    while (base.endsWith('/')) {
        base.chop(1);
    }
    return QUrl(base + "/api/v1/databases/" + QUrl::toPercentEncoding(m_params.serverDatabaseId.trimmed()));
}

QUrl SyncServerClient::listingUrl() const
{
    auto base = m_params.serverUrl.trimmed();
    while (base.endsWith('/')) {
        base.chop(1);
    }
    return QUrl(base + "/api/v1/databases");
}

void SyncServerClient::check(const QString& etag, Callback callback)
{
    Headers headers;
    if (!etag.isEmpty()) {
        headers.append(qMakePair(QByteArray("If-None-Match"), etag.toLatin1()));
    }
    send("HEAD", databaseUrl(), headers, {}, m_params.downloadTimeoutMsec, std::move(callback));
}

void SyncServerClient::download(Callback callback)
{
    send("GET", databaseUrl(), {}, {}, m_params.downloadTimeoutMsec, std::move(callback));
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
    send("PUT", databaseUrl(), headers, data, m_params.uploadTimeoutMsec, std::move(callback));
}

void SyncServerClient::listDatabases(Callback callback)
{
    send("GET", listingUrl(), {}, {}, m_params.downloadTimeoutMsec, std::move(callback));
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
    request.setRawHeader("Authorization", "Bearer " + m_params.serverToken.trimmed().toUtf8());
    request.setRawHeader("User-Agent", "KeePassXC");
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
    response.networkError = tr("This build of KeePassXC was compiled without network support.");
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

QString SyncServerClient::etagFor(const QByteArray& data)
{
    return "\"" + QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) + "\"";
}
