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

#ifndef KEEPASSXC_SYNCEVENTSTREAM_H
#define KEEPASSXC_SYNCEVENTSTREAM_H

#include "config-keepassx.h"

#include "RemoteSettings.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Listens to the sync server's change notifications (GET /api/v1/events, server-sent events),
 * so a device can sync as soon as another one uploads instead of waiting for its next poll.
 *
 * Reconnects by itself after errors, waiting longer each time, and when the server has been
 * silent for longer than its keep-alive interval.
 */
class SyncEventStream : public QObject
{
    Q_OBJECT

public:
    struct Event
    {
        QString databaseId;
        QString etag;
        qint64 revision = -1;
        bool deleted = false;
    };

    static constexpr int SilenceTimeoutMsec = 60 * 1000;
    static constexpr int FirstRetryMsec = 5 * 1000;
    static constexpr int MaxRetryMsec = 5 * 60 * 1000;

    explicit SyncEventStream(const RemoteParams& params, QObject* parent = nullptr);
    ~SyncEventStream() override;

    void start();
    void stop();
    bool isConnected() const;
    const RemoteParams& params() const;

    // Takes the complete events off the front of `buffer` and leaves a partial one for later
    static QList<Event> takeEvents(QByteArray& buffer);

signals:
    void databaseChanged(const SyncEventStream::Event& event);
    void connectedChanged(bool connected);

private:
    void connectToServer();
    void readEvents();
    void connectionEnded();
    void setConnected(bool connected);

    RemoteParams m_params;
    bool m_running = false;
    bool m_connected = false;
    int m_retryMsec = FirstRetryMsec;
    QByteArray m_buffer;
    QTimer m_retryTimer;
    QTimer m_silenceTimer;
#ifdef KPXC_FEATURE_NETWORK
    QNetworkAccessManager* m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
#endif

    Q_DISABLE_COPY(SyncEventStream)
};

#endif // KEEPASSXC_SYNCEVENTSTREAM_H
