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

#ifndef KEEPASSXC_AUTHCLIENT_H
#define KEEPASSXC_AUTHCLIENT_H

#include "config-keepassx.h"

#include "gui/remote/RemoteSettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QUrl>

#include <QThreadPool>

#include <functional>
#include <memory>

namespace ov
{
    class HttpTransport;
}
class QNetworkAccessManager;
class QNetworkReply;

/**
 * Asynchronous JSON client for the few auth server routes (docs/plans/accounts/openapi.yaml) that openvireo-core
 * doesn't cover for the desktop yet: a service account's session and GET /me. Everything else (sign-in, sessions,
 * vaults, sharing, orgs, share links) runs on the core's ov::Account. Every request ends by calling its callback on
 * the client's thread, unless abort() runs first; tests override send() to answer without a network.
 *
 * Routes that need a signed-in caller get their bearer token from the token provider; the others
 * (service account exchange, discovery) never send one.
 */
class AuthClient : public QObject
{
    Q_OBJECT

public:
    struct Response
    {
        // HTTP status, 0 when the request never got a response
        int status = 0;
        QByteArray body;
        QString networkError;

        bool isSuccess() const
        {
            return status >= 200 && status < 300;
        }
        QJsonObject json() const;
        // The machine readable error code, e.g. token_reused
        QString errorCode() const;
        // No answer, or a gateway's: the server can't be reached, as opposed to refusing (SPEC §20)
        bool isUnreachable() const
        {
            return status == 0 || status == 502 || status == 503 || status == 504;
        }
    };
    using Callback = std::function<void(const Response&)>;
    using Headers = QList<QPair<QByteArray, QByteArray>>;

    struct DeviceInfo
    {
        // Empty on this device's first sign-in; the server assigns one
        QString id;
        QString name;
        // qt-linux, qt-macos or qt-windows
        QString platform;

        QJsonObject toJson() const;
        static DeviceInfo current(const QString& id = {});
    };

    static constexpr int TimeoutMsec = 30000;

    // `apiUrl` is the auth server's API, e.g. https://accounts.openvireo.com/api/v1
    explicit AuthClient(const QString& apiUrl = {}, QObject* parent = nullptr);
    ~AuthClient() override;

    void setApiUrl(const QString& apiUrl);
    QString apiUrl() const;
    void setTokenProvider(AccessTokenProvider provider);
    // Sends every request through this core Host transport (on a worker thread) instead of the client's own
    // QNetworkAccessManager, so tests that fake the Host fake this client too. Interim, until AuthClient goes.
    void setTransport(std::shared_ptr<ov::HttpTransport> transport);

    // GET <issuer>/.well-known/openvireo-configuration: {issuer, api, sync, jwks_uri, opaque, kdf_min}
    void discover(const QString& issuerUrl, Callback callback);

    // Service accounts (SPEC §9): the token's secret for an access token. CONTRACT-REQUEST: the core's
    // ServiceAccount can't sync KDBX files yet (vault_kdbx_credentials, sync_vault_kdbx), so a service account
    // signed in on the desktop still runs here; see AccountManager::signInServiceAccount().
    void exchangeServiceAccount(const QString& saId, const QString& secret, Callback callback);
    // The service account's vaults with their sealed keys
    void listVaults(Callback callback);
    // GET /me: the profile with the keyset and KDF the offline cache keeps. CONTRACT-REQUEST: Account::me's Profile
    // carries neither (nor the public keys that show a recovery replaced the keyset).
    void me(Callback callback);

    // Drop all requests in flight; their callbacks are never called
    void abort();

    QUrl endpoint(const QString& path) const;
    static QString errorMessage(const Response& response);

protected:
    // Overridden by tests to answer without a network. Authenticated requests carry the Authorization header.
    virtual void send(const QByteArray& verb,
                      const QUrl& url,
                      const Headers& headers,
                      const QByteArray& body,
                      int timeoutMsec,
                      Callback callback);

    void deliverLater(const Response& response, Callback callback);

private:
    void post(const QString& path, const QJsonObject& body, Callback callback, bool authenticated = false);
    void get(const QString& path, Callback callback);
    void
    request(const QByteArray& verb, const QUrl& url, const QByteArray& body, bool authenticated, Callback callback);

    QString m_apiUrl;
    std::shared_ptr<ov::HttpTransport> m_transport;
    QThreadPool m_pool;
    AccessTokenProvider m_tokenProvider;
    quint64 m_generation = 0;
#ifdef KPXC_FEATURE_NETWORK
    QNetworkAccessManager* m_nam = nullptr;
    QSet<QNetworkReply*> m_replies;
#endif

    Q_DISABLE_COPY(AuthClient)
};

#endif // KEEPASSXC_AUTHCLIENT_H
