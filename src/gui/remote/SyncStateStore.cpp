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

#include "SyncStateStore.h"

#include "RemoteSettings.h"

#include "core/Config.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace
{
    QJsonObject readStates(const QString& filePath)
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QJsonDocument::fromJson(file.readAll()).object().value("databases").toObject();
    }

    bool writeStates(const QString& filePath, const QJsonObject& states)
    {
        QDir().mkpath(QFileInfo(filePath).absolutePath());
        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        QJsonObject root;
        root["version"] = 1;
        root["databases"] = states;
        file.write(QJsonDocument(root).toJson());
        return file.commit();
    }
} // namespace

SyncStateStore::SyncStateStore(const QString& filePath)
    : m_filePath(filePath)
{
}

QString SyncStateStore::filePath() const
{
    return m_filePath;
}

QString SyncStateStore::defaultFilePath()
{
    QString suffix;
#ifdef QT_DEBUG
    suffix = "_debug";
#endif
    auto dir = Config::isPortable() ? Config::portableConfigDir()
                                    : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return dir + QString("/sync-state%1.json").arg(suffix);
}

QString SyncStateStore::keyFor(const QString& databasePath, const RemoteParams& params)
{
    // Hashed so the state file doesn't list local paths and server URLs in the clear
    auto canonicalPath = QFileInfo(databasePath).canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        canonicalPath = QFileInfo(databasePath).absoluteFilePath();
    }
    auto identity =
        QStringList{canonicalPath, params.serverUrl.trimmed(), params.serverDatabaseId.trimmed()}.join('\n');
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

SyncState SyncStateStore::load(const QString& databasePath, const RemoteParams& params) const
{
    auto object = readStates(m_filePath).value(keyFor(databasePath, params)).toObject();
    SyncState state;
    state.etag = object.value("etag").toString();
    state.revision = static_cast<qint64>(object.value("revision").toDouble());
    state.lastSync = QDateTime::fromString(object.value("lastSync").toString(), Qt::ISODateWithMs);
    state.generation = static_cast<qint64>(object.value("generation").toDouble());
    const auto entries = object.value("entries").toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        state.entries.insert(QUuid::fromString(it.key()),
                             QDateTime::fromString(it.value().toString(), Qt::ISODateWithMs));
    }
    return state;
}

bool SyncStateStore::save(const QString& databasePath, const RemoteParams& params, const SyncState& state) const
{
    QJsonObject object;
    object["etag"] = state.etag;
    object["revision"] = static_cast<double>(state.revision);
    object["lastSync"] = state.lastSync.toUTC().toString(Qt::ISODateWithMs);
    object["generation"] = static_cast<double>(state.generation);
    QJsonObject entries;
    for (auto it = state.entries.begin(); it != state.entries.end(); ++it) {
        entries[it.key().toString(QUuid::WithoutBraces)] = it.value().toUTC().toString(Qt::ISODateWithMs);
    }
    object["entries"] = entries;

    auto states = readStates(m_filePath);
    states[keyFor(databasePath, params)] = object;
    return writeStates(m_filePath, states);
}

bool SyncStateStore::remove(const QString& databasePath, const RemoteParams& params) const
{
    auto states = readStates(m_filePath);
    states.remove(keyFor(databasePath, params));
    return writeStates(m_filePath, states);
}
