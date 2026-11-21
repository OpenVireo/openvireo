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

#include "AuthClient.h"

#include "AccountCrypto.h"
#include "core/Tools.h"

#include <ovcore.h>
#include <QCoreApplication>
#include <QtConcurrent>

#include <QJsonDocument>
#include <QPointer>
#include <QSysInfo>
#include <QThread>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#endif

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
    // Requests under way through the core transport finish first: the transport may not outlive its owner. A test
    // transport can need this thread's event loop to answer, so it keeps turning (abort() muted the callbacks).
    while (!m_pool.waitForDone(5)) {
        if (QThread::currentThread() == thread()) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
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

void AuthClient::setTransport(std::shared_ptr<ov::HttpTransport> transport)
{
    // One request at a time: answers come back in the order they were asked, as from one connection
    m_pool.setMaxThreadCount(1);
    m_transport = std::move(transport);
}

void AuthClient::setTokenProvider(AccessTokenProvider provider)
{
    m_tokenProvider = std::move(provider);
}

QUrl AuthClient::endpoint(const QString& path) const
{
    return QUrl(m_apiUrl + "/" + path);
}

void AuthClient::discover(const QString& issuerUrl, Callback callback)
{
    auto base = issuerUrl.trimmed();
    while (base.endsWith('/')) {
        base.chop(1);
    }
    request("GET", QUrl(base + "/.well-known/openvireo-configuration"), {}, false, std::move(callback));
}

void AuthClient::exchangeServiceAccount(const QString& saId, const QString& secret, Callback callback)
{
    post("auth/service-account", {{"sa_id", saId}, {"secret", secret}}, std::move(callback));
}

void AuthClient::me(Callback callback)
{
    get("me", std::move(callback));
}

void AuthClient::listVaults(Callback callback)
{
    get("vaults", std::move(callback));
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
    if (m_transport) {
        ov::HttpRequest req;
        const auto v = verb.toUpper();
        req.method = v == "GET"      ? ov::Method::Get
                     : v == "HEAD"   ? ov::Method::Head
                     : v == "PUT"    ? ov::Method::Put
                     : v == "PATCH"  ? ov::Method::Patch
                     : v == "DELETE" ? ov::Method::Delete
                                     : ov::Method::Post;
        const auto urlText = url.toString(QUrl::FullyEncoded).toStdString();
        req.url = rust::String(urlText.data(), urlText.size());
        for (const auto& header : headers) {
            ov::Header h;
            h.name = rust::String(header.first.constData(), static_cast<size_t>(header.first.size()));
            h.value = rust::String(header.second.constData(), static_cast<size_t>(header.second.size()));
            req.headers.push_back(std::move(h));
        }
        req.has_body = !body.isEmpty();
        for (auto c : body) {
            req.body.push_back(static_cast<uint8_t>(c));
        }
        Q_UNUSED(timeoutMsec)
        auto transport = m_transport;
        auto future = QtConcurrent::run(&m_pool, [transport, req]() {
            Response out;
            ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
            try {
                auto res = transport->send(req, failure);
                if (failure.kind == ov::TransportErrorKind::None) {
                    out.status = res.status;
                    out.body = QByteArray(reinterpret_cast<const char*>(res.body.data()),
                                          static_cast<int>(res.body.size()));
                } else {
                    out.networkError = QString::fromUtf8(failure.message.data(), static_cast<int>(failure.message.size()));
                    if (out.networkError.isEmpty()) {
                        out.networkError = QStringLiteral("network error");
                    }
                }
            } catch (const std::exception& e) {
                out.networkError = QString::fromUtf8(e.what());
            }
            return out;
        });
        auto* watcher = new QFutureWatcher<Response>(this);
        const auto generation = m_generation;
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, callback] {
            watcher->deleteLater();
            if (generation == m_generation) {
                callback(watcher->result());
            }
        });
        watcher->setFuture(future);
        return;
    }
#ifdef KPXC_FEATURE_NETWORK
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
        response.networkError = tr("Invalid account server URL: %1").arg(url.toString());
        deliverLater(response, std::move(callback));
        return;
    }
    if (!Tools::isSecureServerUrl(url)) {
        response.networkError =
            tr("The account server must use https:// (plain http:// is only allowed for this computer): %1")
                .arg(url.toString(QUrl::RemovePath | QUrl::RemoveQuery));
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
    if (response.status == 402 && response.errorCode() == "plan_limit" && !message.isEmpty()) {
        // SPEC.md §19: the server's message names the limit, upgrade_url is where to change plan
        const auto upgradeUrl = response.json().value("upgrade_url").toString();
        if (upgradeUrl.isEmpty()) {
            return tr("%1. Change your plan on the account website.").arg(message);
        }
        return tr("%1. Change your plan at %2").arg(message, upgradeUrl);
    }
    // Account deletion (SPEC §23)
    const auto code = response.errorCode();
    if (code == "confirm_mismatch") {
        return tr("That isn't this account's email. Nothing was deleted.");
    }
    if (code == "deletion_blocked") {
        QStringList names;
        for (const auto& org : response.json().value("orgs").toArray()) {
            names << org.toObject().value("name").toString();
        }
        return tr("You're the only owner of %1. Make someone else an owner, or delete it, first.")
            .arg(names.isEmpty() ? tr("an organization") : names.join(", "));
    }
    if (code == "not_a_member") {
        return tr("They need access to the vault before it can be given to them.");
    }
    if (code == "account_pending_deletion") {
        return tr("This account is scheduled for deletion. Restore it first.");
    }
    if (message.isEmpty()) {
        message = tr("Unexpected response.");
    }
    return tr("The account server responded with HTTP %1: %2").arg(response.status).arg(message);
}
