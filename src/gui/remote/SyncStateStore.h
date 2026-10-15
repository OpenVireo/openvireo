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

#ifndef KEEPASSXC_SYNCSTATESTORE_H
#define KEEPASSXC_SYNCSTATESTORE_H

#include <QDateTime>
#include <QString>

struct RemoteParams;

/**
 * What this device last saw of a database on a sync server.
 */
struct SyncState
{
    // ETag of the server version the local file was last synced with
    QString etag;
    // Highest X-Database-Revision seen, 0 when unknown
    qint64 revision = 0;
    // When the last sync finished successfully
    QDateTime lastSync;

    bool isEmpty() const
    {
        return etag.isEmpty();
    }
};

/**
 * Keeps SyncState per local database file and server database, in one JSON file next to the
 * local (per-device) config.
 *
 * This state is deliberately kept out of the KDBX: writing it into the database would change
 * the file on every sync, and other devices would then see a new version and sync again.
 */
class SyncStateStore
{
public:
    explicit SyncStateStore(const QString& filePath = defaultFilePath());

    SyncState load(const QString& databasePath, const RemoteParams& params) const;
    bool save(const QString& databasePath, const RemoteParams& params, const SyncState& state) const;
    bool remove(const QString& databasePath, const RemoteParams& params) const;

    QString filePath() const;
    static QString defaultFilePath();

private:
    static QString keyFor(const QString& databasePath, const RemoteParams& params);

    QString m_filePath;
};

#endif // KEEPASSXC_SYNCSTATESTORE_H
