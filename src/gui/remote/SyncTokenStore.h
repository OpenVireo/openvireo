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

#ifndef KEEPASSXC_SYNCTOKENSTORE_H
#define KEEPASSXC_SYNCTOKENSTORE_H

#include <QString>
#include <QUuid>

#include <memory>

struct RemoteParams;

/**
 * Keeps sync server access tokens on this device only.
 *
 * Every device has its own token (see sync_server's "devices add"), so a lost device can be
 * revoked on its own. The token therefore can't live in the database file, which every device
 * shares: it goes to the macOS Keychain, or elsewhere to a file only the user can read next to
 * the local config.
 */
class SyncTokenStore
{
public:
    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual QString load(const QString& key) = 0;
        virtual bool save(const QString& key, const QString& token) = 0;
        virtual bool remove(const QString& key) = 0;
        // Where tokens are kept, for the settings page
        virtual QString description() const = 0;
    };

    // Identifies one remote of one database: the same server database can be synced by
    // several local databases, and one database can sync with several servers
    static QString keyFor(const QUuid& databaseId, const RemoteParams& params);

    static QString load(const QString& key);
    static bool save(const QString& key, const QString& token);
    static bool remove(const QString& key);
    static QString description();

    // Replaces the platform's store, for tests; nullptr restores it
    static void setBackend(std::unique_ptr<Backend> backend);
    // An in-memory store, for tests
    static std::unique_ptr<Backend> createMemoryBackend();

private:
    static Backend& backend();
};

#endif // KEEPASSXC_SYNCTOKENSTORE_H
