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

#include "RemoteSettings.h"

#include "SyncTokenStore.h"

#include "core/Database.h"
#include "core/Group.h"
#include "core/Metadata.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

RemoteSettings::RemoteSettings(const QSharedPointer<Database>& db, QObject* parent)
    : QObject(parent)
{
    setDatabase(db);
}

RemoteSettings::~RemoteSettings() = default;

void RemoteSettings::setDatabase(const QSharedPointer<Database>& db)
{
    m_remoteParams.clear();
    m_db = db;
    loadSettings();
}

void RemoteSettings::addRemoteParams(RemoteParams* params)
{
    if (params->name.isEmpty()) {
        qWarning() << "RemoteSettings::addRemoteParams: Remote parameters name is empty";
        return;
    }
    m_remoteParams.insert(params->name, params);
}

void RemoteSettings::removeRemoteParams(const QString& name)
{
    m_remoteParams.remove(name);
}

RemoteParams* RemoteSettings::getRemoteParams(const QString& name) const
{
    if (m_remoteParams.contains(name)) {
        return m_remoteParams.value(name);
    }
    return nullptr;
}

QList<RemoteParams*> RemoteSettings::getAllRemoteParams() const
{
    return m_remoteParams.values();
}

void RemoteSettings::loadSettings()
{
    if (m_db) {
        fromConfig(m_db->metadata()->customData()->value(CustomData::RemoteProgramSettings));
    }
}

void RemoteSettings::saveSettings() const
{
    if (!m_db) {
        return;
    }
    m_db->metadata()->customData()->set(CustomData::RemoteProgramSettings, toConfig());

    // Tokens stay on this device; forget the ones of remotes that were removed or moved
    QSet<QString> keys;
    for (const auto* params : m_remoteParams) {
        if (params->type == RemoteParams::Type::SyncServer) {
            const auto key = tokenKey(*params);
            keys.insert(key);
            if (!SyncTokenStore::save(key, params->serverToken.trimmed())) {
                qWarning("Could not store the sync server access token for %s", qPrintable(params->name));
            }
        }
    }
    for (const auto& key : m_tokenKeys - keys) {
        SyncTokenStore::remove(key);
    }
    m_tokenKeys = keys;
}

QString RemoteSettings::tokenKey(const RemoteParams& params) const
{
    return SyncTokenStore::keyFor(m_db->rootGroup()->uuid(), params);
}

QString RemoteSettings::toConfig() const
{
    QJsonArray config;
    for (const auto params : m_remoteParams.values()) {
        QJsonObject object;
        object["name"] = params->name;
        if (params->type == RemoteParams::Type::SyncServer) {
            object["type"] = "syncServer";
            object["serverUrl"] = params->serverUrl;
            object["serverDatabaseId"] = params->serverDatabaseId;
            object["autoSync"] = params->autoSync;
        }
        object["downloadCommand"] = params->downloadCommand;
        object["downloadCommandInput"] = params->downloadInput;
        object["downloadTimeoutMsec"] = params->downloadTimeoutMsec;
        object["uploadCommand"] = params->uploadCommand;
        object["uploadCommandInput"] = params->uploadInput;
        object["uploadTimeoutMsec"] = params->uploadTimeoutMsec;
        config << object;
    }
    QJsonDocument doc(config);
    return doc.toJson(QJsonDocument::Compact);
}

void RemoteSettings::fromConfig(const QString& data)
{
    m_remoteParams.clear();
    m_tokenKeys.clear();

    QJsonDocument json = QJsonDocument::fromJson(data.toUtf8());
    for (const auto& item : json.array().toVariantList()) {
        auto itemMap = item.toMap();
        auto* params = new RemoteParams();
        params->name = itemMap["name"].toString();
        if (itemMap["type"].toString() == "syncServer") {
            params->type = RemoteParams::Type::SyncServer;
            params->serverUrl = itemMap["serverUrl"].toString();
            params->serverDatabaseId = itemMap["serverDatabaseId"].toString();
            // Each device keeps its own token; databases from before that carry one inside,
            // which this device keeps using until it gets its own
            const auto key = tokenKey(*params);
            params->serverToken = SyncTokenStore::load(key);
            const auto embeddedToken = itemMap["serverToken"].toString();
            if (params->serverToken.isEmpty() && !embeddedToken.isEmpty()) {
                params->serverToken = embeddedToken;
                SyncTokenStore::save(key, embeddedToken);
            }
            m_tokenKeys.insert(key);
            params->autoSync = itemMap["autoSync"].toBool();
        }
        params->downloadCommand = itemMap["downloadCommand"].toString();
        params->downloadInput = itemMap["downloadCommandInput"].toString();
        params->downloadTimeoutMsec = itemMap.value("downloadTimeoutMsec", 10000).toInt();
        params->uploadCommand = itemMap["uploadCommand"].toString();
        params->uploadInput = itemMap["uploadCommandInput"].toString();
        params->uploadTimeoutMsec = itemMap.value("uploadTimeoutMsec", 10000).toInt();

        m_remoteParams.insert(params->name, params);
    }
}
