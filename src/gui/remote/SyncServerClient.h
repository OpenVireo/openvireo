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

#ifndef KEEPASSXC_SYNCSERVERCLIENT_H
#define KEEPASSXC_SYNCSERVERCLIENT_H

#include "config-keepassx.h"

#include "RemoteSettings.h"

#include <QObject>
#include <QSet>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Asynchronous REST client for the KeePassXC sync server (see sync_server/).
 *
 * Every request finishes by calling its callback on the thread that owns the client,
 * unless abort() was called first. The client copies the remote parameters, so later
 * edits to the Remote Sync settings don't affect requests already in flight.
 */
class SyncServerClient : public QObject
{
    Q_OBJECT

public:
    struct Response
    {
        // HTTP status, 0 when the request never got a response
        int status = 0;
        QByteArray body;
        QString etag;
        // X-Database-Revision, -1 when the server didn't send one
        qint64 revision = -1;
        QString networkError;
    };
    using Callback = std::function<void(const Response&)>;
    using Headers = QList<QPair<QByteArray, QByteArray>>;

    explicit SyncServerClient(const RemoteParams& params, QObject* parent = nullptr);
    ~SyncServerClient() override;

    // HEAD with If-None-Match: 304 when the server still has that version
    void check(const QString& etag, Callback callback);
    void download(Callback callback);
    // Conditional PUT: replaces exactly `etag`, or only creates the database when `etag` is empty
    void upload(const QByteArray& data, const QString& etag, Callback callback);
    void listDatabases(Callback callback);
    // The current and kept previous versions: {"versions": [{revision, etag, size, updatedAt}, ...]}, newest first
    void history(Callback callback);
    void downloadRevision(qint64 revision, Callback callback);

    // Drop all requests in flight; their callbacks are never called
    void abort();

    const RemoteParams& params() const;
    // An API URL on the server, e.g. endpoint(params, "events") for <server>/api/v1/events
    static QUrl endpoint(const RemoteParams& params, const QString& path);

    static QString errorMessage(const Response& response);
    // The server's ETag for this content: the quoted SHA-256 of the file
    static QString etagFor(const QByteArray& data);

protected:
    // Overridden by tests to answer without a network
    virtual void send(const QByteArray& verb,
                      const QUrl& url,
                      const Headers& headers,
                      const QByteArray& body,
                      int timeoutMsec,
                      Callback callback);

    // Call a callback from the event loop, unless abort() runs first
    void deliverLater(const Response& response, Callback callback);

private:
    QUrl databaseUrl(const QString& suffix = {}) const;

    RemoteParams m_params;
    // Bumped by abort() so late replies are ignored
    quint64 m_generation = 0;
#ifdef KPXC_FEATURE_NETWORK
    QNetworkAccessManager* m_nam = nullptr;
    QSet<QNetworkReply*> m_replies;
#endif

    Q_DISABLE_COPY(SyncServerClient)
};

#endif // KEEPASSXC_SYNCSERVERCLIENT_H
