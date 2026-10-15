/*
 *  Copyright (C) 2023 KeePassXC Team <team@keepassxc.org>
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

#ifndef KEEPASSXC_REMOTEHANDLER_H
#define KEEPASSXC_REMOTEHANDLER_H

#include <QDateTime>
#include <QObject>

class Database;
class RemoteProcess;
struct RemoteParams;

class RemoteHandler : public QObject
{
    Q_OBJECT

public:
    explicit RemoteHandler(QObject* parent = nullptr);
    ~RemoteHandler() override = default;

    struct RemoteResult
    {
        bool success;
        QString errorMessage;
        QString filePath;
        QString stdOutput;
        QString stdError;
        // Sync server only: version of the downloaded database, used to detect concurrent uploads
        QString etag;
        // Sync server only: the database does not exist on the server yet
        bool remoteMissing = false;
    };

    struct SyncServerListing
    {
        bool success = false;
        QString errorMessage;
        QStringList databaseIds;
    };

    struct SyncServerPairing
    {
        bool success = false;
        QString errorMessage;
        // From createSyncServerPairingCode
        QString code;
        QDateTime expiresAt;
        // From claimSyncServerPairingCode
        QString token;
    };

    RemoteResult download(const RemoteParams* params);
    RemoteResult upload(const QString& filePath, const RemoteParams* params, const QString& etag = {});
    // Names of the databases the access token can see on a sync server
    SyncServerListing listSyncServerDatabases(const RemoteParams* params);
    // A code that adds another device of the same user, for params' token
    SyncServerPairing createSyncServerPairingCode(const RemoteParams* params);
    // Trades a code from another device for this device's own token
    SyncServerPairing claimSyncServerPairingCode(const RemoteParams* params, const QString& code);
    // How this device introduces itself to the server when pairing
    static QString deviceName();

    // Used for testing only
    static void setRemoteProcessFunc(std::function<QScopedPointer<RemoteProcess>(QObject*)> func);

private:
    static RemoteResult downloadFromSyncServer(const RemoteParams* params);
    static RemoteResult uploadToSyncServer(const QString& filePath, const RemoteParams* params, const QString& etag);

    static std::function<QScopedPointer<RemoteProcess>(QObject*)> m_createRemoteProcess;
    static QString m_tempFileLocation;

    Q_DISABLE_COPY(RemoteHandler)
};

#endif // KEEPASSXC_REMOTEHANDLER_H
