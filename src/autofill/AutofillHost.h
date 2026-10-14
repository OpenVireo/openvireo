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

#ifndef KEEPASSXC_AUTOFILLHOST_H
#define KEEPASSXC_AUTOFILLHOST_H

#include "AutofillProtocol.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <functional>

class QLocalServer;
class QLocalSocket;

/**
 * Listens on the App Group socket for the AutoFill extension. Connections from
 * any process other than our signed extension are closed before a byte is read.
 */
class AutofillHost : public QObject
{
    Q_OBJECT

public:
    // Decides whether to accept a connected socket; replaced in tests
    using PeerValidator = std::function<bool(int socketDescriptor, QString* reason)>;

    explicit AutofillHost(QObject* parent = nullptr);
    ~AutofillHost() override;

    bool start(const QString& path);
    void stop();
    bool isListening() const;

    void setPeerValidator(PeerValidator validator);
    void sendResponse(QLocalSocket* socket, const QJsonObject& response);

signals:
    void requestReceived(QLocalSocket* socket, const QJsonObject& request);

private slots:
    void acceptConnection();
    void readyRead();
    void dropConnection();

private:
    void readRequests(QLocalSocket* socket);

    QPointer<QLocalServer> m_server;
    QHash<QLocalSocket*, AutofillProtocol::FrameReader> m_readers;
    PeerValidator m_validator;
};

#endif // KEEPASSXC_AUTOFILLHOST_H
