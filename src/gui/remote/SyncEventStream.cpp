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

#include "SyncEventStream.h"

#include "SyncServerClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#endif

SyncEventStream::SyncEventStream(const RemoteParams& params, QObject* parent)
    : QObject(parent)
    , m_params(params)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &SyncEventStream::connectToServer);
    // The server sends a keep-alive comment every 25 s; a longer silence means the connection is gone
    m_silenceTimer.setSingleShot(true);
    m_silenceTimer.setInterval(SilenceTimeoutMsec);
#ifdef KPXC_FEATURE_NETWORK
    connect(&m_silenceTimer, &QTimer::timeout, this, [this] {
        if (m_reply) {
            m_reply->abort();
        }
    });
#endif
}

SyncEventStream::~SyncEventStream()
{
    // The owner may be half destroyed already (this is often deleted as its child), so don't call back into it
    blockSignals(true);
    stop();
}

const RemoteParams& SyncEventStream::params() const
{
    return m_params;
}

bool SyncEventStream::isConnected() const
{
    return m_connected;
}

void SyncEventStream::start()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_retryMsec = FirstRetryMsec;
    connectToServer();
}

void SyncEventStream::stop()
{
    m_running = false;
    m_connecting = false;
    ++m_generation;
    m_retryTimer.stop();
    m_silenceTimer.stop();
#ifdef KPXC_FEATURE_NETWORK
    if (m_reply) {
        auto* reply = m_reply.data();
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
#endif
    setConnected(false);
}

void SyncEventStream::connectToServer()
{
#ifdef KPXC_FEATURE_NETWORK
    if (!m_running || m_reply || m_connecting) {
        return;
    }
    m_connecting = true;
    const auto generation = m_generation;
    QPointer<SyncEventStream> guard(this);
    auto* thread = QThread::currentThread();
    auto proceed = [guard, generation](const QString& token) {
        if (guard && guard->m_generation == generation) {
            guard->m_connecting = false;
            guard->openConnection(token);
        }
    };
    SyncServerClient::tokenProviderFor(m_params)([guard, thread, proceed](const QString& token) {
        if (QThread::currentThread() == thread) {
            proceed(token);
        } else if (auto* stream = guard.data()) {
            QMetaObject::invokeMethod(stream, [proceed, token] { proceed(token); }, Qt::QueuedConnection);
        }
    });
#endif
}

void SyncEventStream::openConnection(const QString& token)
{
#ifdef KPXC_FEATURE_NETWORK
    if (!m_running || m_reply) {
        return;
    }
    if (token.trimmed().isEmpty()) {
        // Signed out, or the account couldn't refresh its token: try again later
        connectionEnded();
        return;
    }
    QNetworkRequest request(SyncServerClient::endpoint(m_params, "events"));
    request.setRawHeader("Authorization", "Bearer " + token.trimmed().toUtf8());
    request.setRawHeader("User-Agent", "OpenVireo");
    request.setRawHeader("Accept", "text/event-stream");
    // Never follow redirects, they could forward the access token to another host
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    // The response never ends on its own; the silence timer notices dead connections instead
    request.setTransferTimeout(0);

    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    m_buffer.clear();
    m_reply = m_nam->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &SyncEventStream::readEvents);
    connect(m_reply, &QNetworkReply::finished, this, &SyncEventStream::connectionEnded);
    m_silenceTimer.start();
#else
    Q_UNUSED(token)
#endif
}

void SyncEventStream::readEvents()
{
#ifdef KPXC_FEATURE_NETWORK
    if (!m_reply) {
        return;
    }
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        // An error page, e.g. a rejected token; connectionEnded() retries later
        m_reply->readAll();
        return;
    }
    m_silenceTimer.start();
    m_retryMsec = FirstRetryMsec;
    setConnected(true);

    m_buffer.append(m_reply->readAll());
    for (const auto& event : takeEvents(m_buffer)) {
        if (event.type == Event::Type::Access) {
            emit accessChanged(event);
        } else {
            emit databaseChanged(event);
        }
    }
#endif
}

void SyncEventStream::connectionEnded()
{
#ifdef KPXC_FEATURE_NETWORK
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
#endif
    m_silenceTimer.stop();
    setConnected(false);
    if (m_running) {
        m_retryTimer.start(m_retryMsec);
        m_retryMsec = qMin(m_retryMsec * 2, MaxRetryMsec);
    }
}

void SyncEventStream::setConnected(bool connected)
{
    if (m_connected != connected) {
        m_connected = connected;
        emit connectedChanged(connected);
    }
}

QList<SyncEventStream::Event> SyncEventStream::takeEvents(QByteArray& buffer)
{
    QList<Event> events;
    buffer.replace("\r\n", "\n");
    int end;
    // Events end with a blank line
    while ((end = buffer.indexOf("\n\n")) >= 0) {
        const auto block = buffer.left(end);
        buffer.remove(0, end + 2);

        QByteArray type = "message";
        QByteArray data;
        for (const auto& line : block.split('\n')) {
            if (line.startsWith(':')) {
                continue;
            }
            const auto colon = line.indexOf(':');
            const auto field = colon < 0 ? line : line.left(colon);
            auto value = colon < 0 ? QByteArray() : line.mid(colon + 1);
            if (value.startsWith(' ')) {
                value.remove(0, 1);
            }
            if (field == "event") {
                type = value;
            } else if (field == "data") {
                data += (data.isEmpty() ? QByteArray() : QByteArray("\n")) + value;
            }
        }
        const auto json = QJsonDocument::fromJson(data).object();
        Event event;
        if (type == "access") {
            event.type = Event::Type::Access;
            event.vault = json.value("vault").toString();
            event.change = json.value("change").toString();
            events << event;
            continue;
        }
        // Device-token streams send `database`, account streams `vault`, with the same data
        if (type != "database" && type != "vault") {
            continue;
        }
        event.databaseId = json.value("id").toString();
        event.etag = json.value("etag").toString();
        event.revision = json.value("revision").toVariant().toLongLong();
        event.deleted = json.value("deleted").toBool();
        if (!event.databaseId.isEmpty()) {
            events << event;
        }
    }
    return events;
}
