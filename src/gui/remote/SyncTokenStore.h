/*
 *  Copyright (C) 2026 Bongani Mbigi
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

#ifndef OPENVIREO_SYNCTOKENSTORE_H
#define OPENVIREO_SYNCTOKENSTORE_H

#include <QString>
#include <QUuid>

#include <memory>

struct RemoteParams;

/**
 * Keeps sync server access tokens on this device only.
 *
 * Every device has its own token (see keepassxc-sync-server's "devices add"), so a lost device can be
 * revoked on its own. The token therefore can't live in the database file, which every device
 * shares: it goes to the macOS Keychain, the Windows Credential Manager or the desktop's keyring
 * (Secret Service). Without one of those it goes to a file only the user can read next to the
 * local config, and moves to the system store once that becomes available.
 */
class SyncTokenStore
{
public:
    class Backend
    {
    public:
        virtual ~Backend() = default;
        // False when the store can't be reached on this system right now
        virtual bool isAvailable()
        {
            return true;
        }
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

    // Replaces the platform's store, for tests; nullptr restores the default (memory in tests, see below)
    static void setBackend(std::unique_ptr<Backend> backend);
    // The system store (Keychain, Credential Manager, keyring or the token file). Never the default in
    // tests: only for a test that opts in to writing to the real store.
    static std::unique_ptr<Backend> createPlatformBackend();
    // True in a test (QStandardPaths test mode, or a binary that sets TestBinaryVariable, as every unit
    // test does): the default store is then an in-memory one, with a warning, so that a test that forgot
    // setBackend() never reads or overwrites the user's real tokens and account keys
    static bool refusesSystemStore();
    // Set by every test binary at start-up (tests/util/TestBinaryMarker.cpp), inherited by what it runs
    static constexpr const char* TestBinaryVariable = "OPENVIREO_TEST_BINARY";
    // An in-memory store, for tests
    static std::unique_ptr<Backend> createMemoryBackend();
    // The private file, for tests
    static std::unique_ptr<Backend> createFileBackend(const QString& filePath);
    // Uses the system store when it is available and the file otherwise; tokens found in the
    // file move to the system store when they are next read
    static std::unique_ptr<Backend> createFallbackBackend(std::unique_ptr<Backend> system,
                                                          std::unique_ptr<Backend> file);

private:
    static Backend& backend();
};

#endif // OPENVIREO_SYNCTOKENSTORE_H
