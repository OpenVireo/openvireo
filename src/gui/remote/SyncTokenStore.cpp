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

#include "SyncTokenStore.h"

#include "RemoteSettings.h"
#include "SyncStateStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace
{
    class MemoryBackend : public SyncTokenStore::Backend
    {
    public:
        QString load(const QString& key) override
        {
            return m_tokens.value(key);
        }
        bool save(const QString& key, const QString& token) override
        {
            m_tokens.insert(key, token);
            return true;
        }
        bool remove(const QString& key) override
        {
            m_tokens.remove(key);
            return true;
        }
        QString description() const override
        {
            return QCoreApplication::translate("SyncTokenStore", "memory");
        }

    private:
        QHash<QString, QString> m_tokens;
    };

    /*
     * A JSON file next to the local config that only the user can read. Used where no system
     * credential store is wired up yet.
     */
    class FileBackend : public SyncTokenStore::Backend
    {
    public:
        QString load(const QString& key) override
        {
            return read().value(key).toString();
        }
        bool save(const QString& key, const QString& token) override
        {
            auto tokens = read();
            tokens[key] = token;
            return write(tokens);
        }
        bool remove(const QString& key) override
        {
            auto tokens = read();
            if (!tokens.contains(key)) {
                return true;
            }
            tokens.remove(key);
            return write(tokens);
        }
        QString description() const override
        {
            return QCoreApplication::translate("SyncTokenStore", "a file only you can read, next to the settings");
        }

    private:
        static QString filePath()
        {
            return QFileInfo(SyncStateStore::defaultFilePath()).absolutePath() + "/sync-tokens.json";
        }
        static QJsonObject read()
        {
            QFile file(filePath());
            if (!file.open(QIODevice::ReadOnly)) {
                return {};
            }
            return QJsonDocument::fromJson(file.readAll()).object();
        }
        static bool write(const QJsonObject& tokens)
        {
            QSaveFile file(filePath());
            if (!file.open(QIODevice::WriteOnly)) {
                return false;
            }
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            file.write(QJsonDocument(tokens).toJson());
            return file.commit();
        }
    };

#ifdef Q_OS_MACOS
    /*
     * Generic passwords in the login keychain, never synced through iCloud: each token belongs
     * to this Mac only.
     */
    class KeychainBackend : public SyncTokenStore::Backend
    {
    public:
        QString load(const QString& key) override
        {
            auto query = baseQuery(key);
            CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
            CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
            CFTypeRef result = nullptr;
            const auto status = SecItemCopyMatching(query, &result);
            CFRelease(query);
            if (status != errSecSuccess || !result) {
                return {};
            }
            const auto data = static_cast<CFDataRef>(result);
            const auto token = QString::fromUtf8(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                                                 static_cast<int>(CFDataGetLength(data)));
            CFRelease(result);
            return token;
        }

        bool save(const QString& key, const QString& token) override
        {
            const auto bytes = token.toUtf8();
            auto data =
                CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(bytes.constData()), bytes.size());

            auto query = baseQuery(key);
            auto update = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(update, kSecValueData, data);
            auto status = SecItemUpdate(query, update);
            CFRelease(update);
            if (status == errSecItemNotFound) {
                CFDictionarySetValue(query, kSecValueData, data);
                CFDictionarySetValue(query, kSecAttrLabel, CFSTR("KeePassXC sync server access token"));
                status = SecItemAdd(query, nullptr);
            }
            CFRelease(query);
            CFRelease(data);
            return status == errSecSuccess;
        }

        bool remove(const QString& key) override
        {
            auto query = baseQuery(key);
            const auto status = SecItemDelete(query);
            CFRelease(query);
            return status == errSecSuccess || status == errSecItemNotFound;
        }

        QString description() const override
        {
            return QCoreApplication::translate("SyncTokenStore", "the Keychain");
        }

    private:
        static CFMutableDictionaryRef baseQuery(const QString& key)
        {
            auto query = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            const auto account = key.toCFString();
            CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
            CFDictionarySetValue(query, kSecAttrService, CFSTR("KeePassXC Sync"));
            CFDictionarySetValue(query, kSecAttrAccount, account);
            CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
            CFRelease(account);
            return query;
        }
    };
#endif

    std::unique_ptr<SyncTokenStore::Backend>& currentBackend()
    {
        static std::unique_ptr<SyncTokenStore::Backend> backend;
        return backend;
    }

    std::unique_ptr<SyncTokenStore::Backend> createPlatformBackend()
    {
#ifdef Q_OS_MACOS
        return std::make_unique<KeychainBackend>();
#else
        return std::make_unique<FileBackend>();
#endif
    }
} // namespace

QString SyncTokenStore::keyFor(const QUuid& databaseId, const RemoteParams& params)
{
    auto identity = QStringList{databaseId.toString(QUuid::WithoutBraces),
                                params.serverUrl.trimmed(),
                                params.serverDatabaseId.trimmed()}
                        .join('\n');
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

SyncTokenStore::Backend& SyncTokenStore::backend()
{
    auto& backend = currentBackend();
    if (!backend) {
        backend = createPlatformBackend();
    }
    return *backend;
}

QString SyncTokenStore::load(const QString& key)
{
    return backend().load(key);
}

bool SyncTokenStore::save(const QString& key, const QString& token)
{
    if (token.isEmpty()) {
        return backend().remove(key);
    }
    return backend().save(key, token);
}

bool SyncTokenStore::remove(const QString& key)
{
    return backend().remove(key);
}

QString SyncTokenStore::description()
{
    return backend().description();
}

void SyncTokenStore::setBackend(std::unique_ptr<Backend> backend)
{
    currentBackend() = std::move(backend);
}

std::unique_ptr<SyncTokenStore::Backend> SyncTokenStore::createMemoryBackend()
{
    return std::make_unique<MemoryBackend>();
}
