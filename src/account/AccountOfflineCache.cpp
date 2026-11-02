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

#include "AccountOfflineCache.h"

#include "core/Config.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace
{
    constexpr int CacheVersion = 1;
} // namespace

QString AccountOfflineCache::accountsRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/accounts";
}

QString AccountOfflineCache::path(const QString& accountId)
{
    return accountsRoot() + "/" + accountId + "/offline.json";
}

bool AccountOfflineCache::isEnabled()
{
    return config()->get(Config::Account_AllowOfflineUnlock).toBool();
}

void AccountOfflineCache::saveProfile(const QString& accountId,
                                      const QJsonObject& kdf,
                                      const QJsonObject& keyset,
                                      const QJsonArray& orgs)
{
    if (kdf.isEmpty() || keyset.isEmpty()) {
        return;
    }
    update(accountId, {{"kdf", kdf}, {"keyset", keyset}, {"orgs", orgs}});
}

void AccountOfflineCache::saveVaults(const QString& accountId, const QJsonArray& vaults)
{
    update(accountId, {{"vaults", vaults}});
}

QJsonObject AccountOfflineCache::load(const QString& accountId)
{
    if (accountId.isEmpty() || !isEnabled()) {
        return {};
    }
    QFile file(path(accountId));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    // A cache from a newer version may mean something else; unlock online until it is rewritten
    if (json.value("version").toInt() != CacheVersion) {
        return {};
    }
    return json;
}

bool AccountOfflineCache::hasProfile(const QString& accountId)
{
    const auto json = load(accountId);
    return !json.value("kdf").toObject().isEmpty() && !json.value("keyset").toObject().isEmpty();
}

void AccountOfflineCache::clear(const QString& accountId)
{
    if (!accountId.isEmpty()) {
        QFile::remove(path(accountId));
    }
}

void AccountOfflineCache::clearAll()
{
    const auto accounts = QDir(accountsRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& accountId : accounts) {
        clear(accountId);
    }
}

void AccountOfflineCache::update(const QString& accountId, const QJsonObject& fields)
{
    if (accountId.isEmpty()) {
        return;
    }
    if (!isEnabled()) {
        clear(accountId);
        return;
    }
    auto json = load(accountId);
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        json.insert(it.key(), it.value());
    }
    json["version"] = CacheVersion;
    json["cached_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    const auto filePath = path(accountId);
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
    if (file.commit()) {
        QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
}
