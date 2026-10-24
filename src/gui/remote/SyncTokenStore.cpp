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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
// clang-format off
#include <windows.h>
#include <wincred.h>
// clang-format on
#elif defined(Q_OS_UNIX) && !defined(QT_NO_DBUS)
#include "SyncTokenSecretService.h"
#define KPXC_SYNC_TOKEN_SECRET_SERVICE
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
     * credential store can be reached.
     */
    class FileBackend : public SyncTokenStore::Backend
    {
    public:
        explicit FileBackend(QString filePath = {})
            : m_filePath(std::move(filePath))
        {
        }

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
        QString filePath() const
        {
            if (!m_filePath.isEmpty()) {
                return m_filePath;
            }
            return QFileInfo(SyncStateStore::defaultFilePath()).absolutePath() + "/sync-tokens.json";
        }
        QJsonObject read() const
        {
            QFile file(filePath());
            if (!file.open(QIODevice::ReadOnly)) {
                return {};
            }
            return QJsonDocument::fromJson(file.readAll()).object();
        }
        bool write(const QJsonObject& tokens) const
        {
            if (tokens.isEmpty()) {
                // Leave no file behind once every token has moved to the system store
                return !QFile::exists(filePath()) || QFile::remove(filePath());
            }
            // The settings folder may not exist yet on a fresh install
            QDir().mkpath(QFileInfo(filePath()).absolutePath());
            QSaveFile file(filePath());
            if (!file.open(QIODevice::WriteOnly)) {
                return false;
            }
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            file.write(QJsonDocument(tokens).toJson());
            return file.commit();
        }

        QString m_filePath;
    };

    /*
     * The system store when it can be reached, the file otherwise. A token is only ever in one
     * of them: saving to the system store removes the file's copy.
     */
    class FallbackBackend : public SyncTokenStore::Backend
    {
    public:
        FallbackBackend(std::unique_ptr<SyncTokenStore::Backend> system, std::unique_ptr<SyncTokenStore::Backend> file)
            : m_system(std::move(system))
            , m_file(std::move(file))
        {
        }

        QString load(const QString& key) override
        {
            if (!m_system->isAvailable()) {
                return m_file->load(key);
            }
            auto token = m_system->load(key);
            if (token.isEmpty()) {
                // Kept in the file while no system store could be reached
                token = m_file->load(key);
                if (!token.isEmpty() && m_system->save(key, token)) {
                    m_file->remove(key);
                }
            }
            return token;
        }

        bool save(const QString& key, const QString& token) override
        {
            if (m_system->isAvailable() && m_system->save(key, token)) {
                m_file->remove(key);
                return true;
            }
            return m_file->save(key, token);
        }

        bool remove(const QString& key) override
        {
            const auto removedFromFile = m_file->remove(key);
            if (!m_system->isAvailable()) {
                return removedFromFile;
            }
            return m_system->remove(key) && removedFromFile;
        }

        QString description() const override
        {
            return m_system->isAvailable() ? m_system->description() : m_file->description();
        }

    private:
        std::unique_ptr<SyncTokenStore::Backend> m_system;
        std::unique_ptr<SyncTokenStore::Backend> m_file;
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
                CFDictionarySetValue(query, kSecAttrLabel, CFSTR("OpenVireo sync server access token"));
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
            CFDictionarySetValue(query, kSecAttrService, CFSTR("OpenVireo Sync"));
            CFDictionarySetValue(query, kSecAttrAccount, account);
            CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
            CFRelease(account);
            return query;
        }
    };
#elif defined(Q_OS_WIN)
    /*
     * Generic credentials in the Windows Credential Manager, kept on this computer only (not
     * roamed with the user's profile): each token belongs to one device.
     */
    class CredentialManagerBackend : public SyncTokenStore::Backend
    {
    public:
        QString load(const QString& key) override
        {
            PCREDENTIALW credential = nullptr;
            if (!CredReadW(target(key).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
                return {};
            }
            const auto token = QString::fromUtf8(reinterpret_cast<const char*>(credential->CredentialBlob),
                                                 static_cast<int>(credential->CredentialBlobSize));
            CredFree(credential);
            return token;
        }

        bool save(const QString& key, const QString& token) override
        {
            auto bytes = token.toUtf8();
            const auto name = target(key);
            CREDENTIALW credential = {};
            credential.Type = CRED_TYPE_GENERIC;
            credential.TargetName = const_cast<LPWSTR>(name.c_str());
            credential.Comment = const_cast<LPWSTR>(L"OpenVireo sync server access token");
            credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
            credential.CredentialBlob = reinterpret_cast<LPBYTE>(bytes.data());
            credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
            credential.UserName = const_cast<LPWSTR>(L"OpenVireo");
            return CredWriteW(&credential, 0);
        }

        bool remove(const QString& key) override
        {
            return CredDeleteW(target(key).c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
        }

        QString description() const override
        {
            return QCoreApplication::translate("SyncTokenStore", "the Windows Credential Manager");
        }

    private:
        static std::wstring target(const QString& key)
        {
            return QStringLiteral("OpenVireo Sync/%1").arg(key).toStdWString();
        }
    };
#endif

    std::unique_ptr<SyncTokenStore::Backend>& currentBackend()
    {
        static std::unique_ptr<SyncTokenStore::Backend> backend;
        return backend;
    }

    // Tokens already read, so that reloading the remote settings after every change doesn't
    // ask the system store again
    QHash<QString, QString>& cache()
    {
        static QHash<QString, QString> tokens;
        return tokens;
    }

    std::unique_ptr<SyncTokenStore::Backend> createPlatformBackend()
    {
#ifdef Q_OS_MACOS
        return std::make_unique<KeychainBackend>();
#elif defined(Q_OS_WIN)
        return SyncTokenStore::createFallbackBackend(std::make_unique<CredentialManagerBackend>(),
                                                     std::make_unique<FileBackend>());
#elif defined(KPXC_SYNC_TOKEN_SECRET_SERVICE)
        return SyncTokenStore::createFallbackBackend(createSecretServiceBackend(), std::make_unique<FileBackend>());
#else
        return std::make_unique<FileBackend>();
#endif
    }
} // namespace

QString SyncTokenStore::keyFor(const QUuid& databaseId, const RemoteParams& params)
{
    auto identity =
        QStringList{databaseId.toString(QUuid::WithoutBraces), params.serverUrl.trimmed(), params.remoteDatabaseId()}
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
    auto it = cache().constFind(key);
    if (it != cache().constEnd()) {
        return it.value();
    }
    // A missing token isn't kept: it may be added from another window or after the keyring unlocks
    const auto token = backend().load(key);
    if (!token.isEmpty()) {
        cache().insert(key, token);
    }
    return token;
}

bool SyncTokenStore::save(const QString& key, const QString& token)
{
    if (token.isEmpty()) {
        return remove(key);
    }
    if (cache().value(key) == token) {
        return true;
    }
    cache().remove(key);
    if (!backend().save(key, token)) {
        return false;
    }
    cache().insert(key, token);
    return true;
}

bool SyncTokenStore::remove(const QString& key)
{
    cache().remove(key);
    return backend().remove(key);
}

QString SyncTokenStore::description()
{
    return backend().description();
}

void SyncTokenStore::setBackend(std::unique_ptr<Backend> backend)
{
    cache().clear();
    currentBackend() = std::move(backend);
}

std::unique_ptr<SyncTokenStore::Backend> SyncTokenStore::createMemoryBackend()
{
    return std::make_unique<MemoryBackend>();
}

std::unique_ptr<SyncTokenStore::Backend> SyncTokenStore::createFileBackend(const QString& filePath)
{
    return std::make_unique<FileBackend>(filePath);
}

std::unique_ptr<SyncTokenStore::Backend> SyncTokenStore::createFallbackBackend(std::unique_ptr<Backend> system,
                                                                               std::unique_ptr<Backend> file)
{
    return std::make_unique<FallbackBackend>(std::move(system), std::move(file));
}
