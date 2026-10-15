/*
 *  Copyright (C) 2025 KeePassXC Team <team@keepassxc.org>
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

#ifndef KEEPASSXC_REMOTESETTINGS_H
#define KEEPASSXC_REMOTESETTINGS_H

#include <QHash>
#include <QObject>
#include <QSet>
#include <QSharedPointer>

class Database;

struct RemoteParams
{
    enum class Type
    {
        // Run user-supplied shell commands (scp, sftp, rclone, ...)
        Command,
        // Talk to a KeePassXC sync server over its REST API (see sync_server/)
        SyncServer
    };

    QString name;
    Type type = Type::Command;
    QString downloadCommand;
    QString downloadInput;
    int downloadTimeoutMsec = 10000;
    QString uploadCommand;
    QString uploadInput;
    int uploadTimeoutMsec = 10000;
    // Sync server settings
    QString serverUrl;
    QString serverDatabaseId;
    // Kept on this device only (SyncTokenStore), never written into the database
    QString serverToken;
    // Sync server only: sync in the background after saves, on unlock and periodically
    bool autoSync = false;

    bool hasDownload() const
    {
        return type == Type::SyncServer || !downloadCommand.isEmpty();
    }
    bool hasUpload() const
    {
        return type == Type::SyncServer || !uploadCommand.isEmpty();
    }
};
Q_DECLARE_METATYPE(RemoteParams)

class RemoteSettings : public QObject
{
    Q_OBJECT
public:
    explicit RemoteSettings(const QSharedPointer<Database>& db, QObject* parent = nullptr);
    ~RemoteSettings() override;

    void setDatabase(const QSharedPointer<Database>& db);

    void addRemoteParams(RemoteParams* params);
    void removeRemoteParams(const QString& name);
    RemoteParams* getRemoteParams(const QString& name) const;
    QList<RemoteParams*> getAllRemoteParams() const;

    void loadSettings();
    void saveSettings() const;

private:
    void fromConfig(const QString& data);
    QString toConfig() const;
    QString tokenKey(const RemoteParams& params) const;

    QHash<QString, RemoteParams*> m_remoteParams;
    QSharedPointer<Database> m_db;
    // Tokens stored for the remotes as last loaded or saved
    mutable QSet<QString> m_tokenKeys;
};

#endif // KEEPASSXC_REMOTESETTINGS_H
