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

#include "AutofillHost.h"

#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>

AutofillHost::AutofillHost(QObject* parent)
    : QObject(parent)
    , m_server(new QLocalServer(this))
{
    // Only the user's own processes can connect; the code signature check narrows that to the extension
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &AutofillHost::acceptConnection);
}

AutofillHost::~AutofillHost()
{
    stop();
}

bool AutofillHost::start(const QString& path)
{
    if (m_server->isListening()) {
        return true;
    }
    if (!m_validator) {
        qWarning() << "AutoFill: refusing to listen without a peer validator";
        return false;
    }

    // A socket file left behind by a crash would make listen() fail
    QLocalServer::removeServer(path);
    if (!m_server->listen(path)) {
        qWarning() << "AutoFill: could not listen on" << path << m_server->errorString();
        return false;
    }
    return true;
}

void AutofillHost::stop()
{
    for (auto* socket : m_readers.keys()) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_readers.clear();
    m_server->close();
}

bool AutofillHost::isListening() const
{
    return m_server->isListening();
}

void AutofillHost::setPeerValidator(PeerValidator validator)
{
    m_validator = std::move(validator);
}

void AutofillHost::acceptConnection()
{
    while (auto* socket = m_server->nextPendingConnection()) {
        QString reason;
        if (!m_validator || !m_validator(static_cast<int>(socket->socketDescriptor()), &reason)) {
            qWarning() << "AutoFill: refused a connection:" << reason;
            socket->abort();
            socket->deleteLater();
            continue;
        }

        m_readers.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, &AutofillHost::readyRead);
        connect(socket, &QLocalSocket::disconnected, this, &AutofillHost::dropConnection);
        // Data may have arrived before the signals were connected
        if (socket->bytesAvailable() > 0) {
            QMetaObject::invokeMethod(this, [this, socket] { readRequests(socket); }, Qt::QueuedConnection);
        }
    }
}

void AutofillHost::readyRead()
{
    readRequests(qobject_cast<QLocalSocket*>(sender()));
}

void AutofillHost::readRequests(QLocalSocket* socket)
{
    if (!socket || !m_readers.contains(socket)) {
        return;
    }

    auto& reader = m_readers[socket];
    reader.append(socket->readAll());
    for (auto request = reader.next(); !request.isEmpty(); request = reader.next()) {
        emit requestReceived(socket, request);
    }
    if (reader.hasError()) {
        qWarning() << "AutoFill: dropping a connection that sent a malformed frame";
        socket->abort();
    }
}

void AutofillHost::dropConnection()
{
    auto* socket = qobject_cast<QLocalSocket*>(sender());
    if (socket) {
        m_readers.remove(socket);
        socket->deleteLater();
    }
}

void AutofillHost::sendResponse(QLocalSocket* socket, const QJsonObject& response)
{
    if (!socket || !m_readers.contains(socket) || socket->state() != QLocalSocket::ConnectedState) {
        return;
    }
    socket->write(AutofillProtocol::encodeFrame(response));
    socket->flush();
}
