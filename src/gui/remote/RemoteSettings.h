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

#include <functional>

class Database;

/**
 * Hands out the bearer token for one request. It is asked again before every request (and every
 * event stream connection), so it can refresh an expiring token first. `done` may be called later
 * and on any thread; an empty token means there is none.
 */
using AccessTokenProvider = std::function<void(std::function<void(const QString& token)> done)>;

struct RemoteParams
{
    enum class Type
    {
        // Run user-supplied shell commands (scp, sftp, rclone, ...)
        Command,
        // Talk to a KeePassXC sync server over its REST API (see keepassxc-sync-server)
        SyncServer,
        // A vault of a KeePassXC account: the account's sync server, vaults/<vaultId>, with the account's
        // access tokens (see docs/plans/accounts/SPEC.md)
        AccountVault
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
    // Account vaults: the account (its user id, or the service account id) and the vault id. The token
    // comes from the account's AccessTokenProvider, see SyncServerClient::setAccountTokenProvider().
    QString accountId;
    QString vaultId;
    // Overrides serverToken and the account's provider when set; not saved
    AccessTokenProvider tokenProvider;
    // Sync server only: sync in the background after saves, on unlock and periodically
    bool autoSync = false;

    // Whether the remote is a sync server, whose own or an account's
    bool isServer() const
    {
        return type == Type::SyncServer || type == Type::AccountVault;
    }
    // The id of the database on the server: the vault id for account vaults
    QString remoteDatabaseId() const
    {
        return (type == Type::AccountVault ? vaultId : serverDatabaseId).trimmed();
    }
    bool hasDownload() const
    {
        return isServer() || !downloadCommand.isEmpty();
    }
    bool hasUpload() const
    {
        return isServer() || !uploadCommand.isEmpty();
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
