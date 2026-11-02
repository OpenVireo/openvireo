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

#ifndef KEEPASSXC_ACCOUNTOFFLINECACHE_H
#define KEEPASSXC_ACCOUNTOFFLINECACHE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/**
 * What an account keeps on this device to unlock without the network (SPEC §20): the wrapped keyset
 * and its KDF settings and the org list as GET /me returned them, and the vault list as GET /vaults
 * returned it (encrypted names, sealed keys). Nothing in it opens without the master password and the
 * Secret Key, which stays in the keychain.
 *
 * It lives in <accounts root>/<account id>/offline.json beside the vault copies, readable by the user
 * only. Writes are skipped and the file is removed while Account/AllowOfflineUnlock is off.
 */
class AccountOfflineCache
{
public:
    // AppDataLocation/accounts: the vault copies and caches of every account on this device
    static QString accountsRoot();
    static QString path(const QString& accountId);
    static bool isEnabled();

    // The cached GET /me fields; `orgs` may be empty
    static void saveProfile(const QString& accountId,
                            const QJsonObject& kdf,
                            const QJsonObject& keyset,
                            const QJsonArray& orgs);
    static void saveVaults(const QString& accountId, const QJsonArray& vaults);

    // Empty objects/arrays when nothing is cached (or caching is off)
    static QJsonObject load(const QString& accountId);
    static bool hasProfile(const QString& accountId);

    static void clear(const QString& accountId);
    // Every account's cache on this device, when offline unlock is turned off
    static void clearAll();

private:
    static void update(const QString& accountId, const QJsonObject& fields);
};

#endif // KEEPASSXC_ACCOUNTOFFLINECACHE_H
