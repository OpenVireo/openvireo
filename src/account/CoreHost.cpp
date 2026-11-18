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

#include "CoreHost.h"

#include "AccountOfflineCache.h"
#include "core/Clock.h"
#include "gui/remote/SyncTokenStore.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLoggingCategory>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <botan/mem_ops.h>

Q_LOGGING_CATEGORY(lcOpenVireoCore, "openvireo.core")

namespace CoreHost
{
    namespace
    {
        QByteArray verbOf(ov::Method method)
        {
            switch (method) {
            case ov::Method::Get:
                return "GET";
            case ov::Method::Head:
                return "HEAD";
            case ov::Method::Post:
                return "POST";
            case ov::Method::Put:
                return "PUT";
            case ov::Method::Patch:
                return "PATCH";
            case ov::Method::Delete:
                return "DELETE";
            }
            return "GET";
        }

        QNetworkRequest requestOf(const ov::HttpRequest& req)
        {
            QNetworkRequest request(QUrl(toQString(req.url)));
            // The core decides about redirects and keeps no cookies (host.h)
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
            request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
            for (const auto& header : req.headers) {
                request.setRawHeader(QByteArray(header.name.data(), static_cast<int>(header.name.size())),
                                     QByteArray(header.value.data(), static_cast<int>(header.value.size())));
            }
            return request;
        }

        QNetworkReply* start(QNetworkAccessManager& nam, const ov::HttpRequest& req)
        {
            const auto request = requestOf(req);
            const auto verb = verbOf(req.method);
            if (req.has_body) {
                const QByteArray body(reinterpret_cast<const char*>(req.body.data()), static_cast<int>(req.body.size()));
                return nam.sendCustomRequest(request, verb, body);
            }
            return nam.sendCustomRequest(request, verb);
        }

        // A reply that never got an HTTP status is a transport failure
        bool failed(QNetworkReply* reply, ov::TransportFailure& failure)
        {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) {
                return false;
            }
            switch (reply->error()) {
            case QNetworkReply::NoError:
                return false;
            case QNetworkReply::OperationCanceledError:
                failure.kind = ov::TransportErrorKind::Cancelled;
                break;
            case QNetworkReply::TimeoutError:
                failure.kind = ov::TransportErrorKind::Timeout;
                break;
            case QNetworkReply::SslHandshakeFailedError:
            case QNetworkReply::InsecureRedirectError:
                failure.kind = ov::TransportErrorKind::Tls;
                break;
            case QNetworkReply::ConnectionRefusedError:
            case QNetworkReply::RemoteHostClosedError:
            case QNetworkReply::HostNotFoundError:
            case QNetworkReply::TemporaryNetworkFailureError:
            case QNetworkReply::NetworkSessionFailedError:
            case QNetworkReply::ProxyConnectionRefusedError:
            case QNetworkReply::ProxyNotFoundError:
                failure.kind = ov::TransportErrorKind::Offline;
                break;
            default:
                failure.kind = ov::TransportErrorKind::Other;
                break;
            }
            const auto message = reply->errorString().toStdString();
            failure.message = rust::String(message.data(), message.size());
            return true;
        }

        void copyHeaders(QNetworkReply* reply, rust::Vec<ov::Header>& headers)
        {
            for (const auto& pair : reply->rawHeaderPairs()) {
                ov::Header header;
                header.name = rust::String(pair.first.constData(), static_cast<size_t>(pair.first.size()));
                header.value = rust::String(pair.second.constData(), static_cast<size_t>(pair.second.size()));
                headers.push_back(std::move(header));
            }
        }

        rust::Vec<uint8_t> toVec(const QByteArray& data)
        {
            rust::Vec<uint8_t> vec;
            vec.reserve(static_cast<size_t>(data.size()));
            for (auto c : data) {
                vec.push_back(static_cast<uint8_t>(c));
            }
            return vec;
        }

        /*
         * A server-sent-events stream. The reply lives on its own thread with its own event loop, so the core
         * may read the stream from any worker thread (and from a different one than opened it).
         */
        class Stream : public ov::ByteStream
        {
        public:
            Stream(const ov::HttpRequest& req, ov::TransportFailure& failure)
            {
                m_thread.start();
                auto* worker = new QObject();
                worker->moveToThread(&m_thread);
                QObject::connect(&m_thread, &QThread::finished, worker, &QObject::deleteLater);
                m_worker = worker;
                const auto copy = req;
                QMetaObject::invokeMethod(worker, [this, copy] { open(copy); }, Qt::QueuedConnection);

                QMutexLocker lock(&m_mutex);
                while (!m_headersSeen && !m_done) {
                    m_wake.wait(&m_mutex);
                }
                if (!m_headersSeen) {
                    failure = m_failure;
                }
            }

            ~Stream() override
            {
                close();
                m_thread.quit();
                m_thread.wait();
            }

            bool opened() const
            {
                QMutexLocker lock(&m_mutex);
                return m_headersSeen;
            }

            std::uint16_t status() const override
            {
                QMutexLocker lock(&m_mutex);
                return m_status;
            }

            bool next_chunk(rust::Vec<std::uint8_t>& chunk, ov::TransportFailure& failure) const override
            {
                QMutexLocker lock(&m_mutex);
                while (m_buffer.isEmpty() && !m_done) {
                    m_wake.wait(&m_mutex);
                }
                if (!m_buffer.isEmpty()) {
                    chunk = toVec(m_buffer);
                    m_buffer.clear();
                    return true;
                }
                if (m_failure.kind != ov::TransportErrorKind::None && !m_closed) {
                    failure = m_failure;
                }
                return false;
            }

            void close() const override
            {
                {
                    QMutexLocker lock(&m_mutex);
                    if (m_closed) {
                        return;
                    }
                    m_closed = true;
                }
                QMetaObject::invokeMethod(
                    m_worker,
                    [this] {
                        if (m_reply) {
                            m_reply->abort();
                        }
                    },
                    Qt::BlockingQueuedConnection);
            }

        private:
            // On m_thread
            void open(const ov::HttpRequest& req)
            {
                m_nam = new QNetworkAccessManager(m_worker);
                m_reply = start(*m_nam, req);
                auto* reply = m_reply;
                const auto seeHeaders = [this, reply] {
                    QMutexLocker lock(&m_mutex);
                    if (!m_headersSeen && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) {
                        m_status = static_cast<std::uint16_t>(
                            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
                        m_headersSeen = true;
                        m_wake.wakeAll();
                    }
                };
                QObject::connect(reply, &QNetworkReply::metaDataChanged, m_worker, seeHeaders);
                QObject::connect(reply, &QNetworkReply::readyRead, m_worker, [this, reply, seeHeaders] {
                    seeHeaders();
                    const auto data = reply->readAll();
                    QMutexLocker lock(&m_mutex);
                    m_buffer += data;
                    m_wake.wakeAll();
                });
                QObject::connect(reply, &QNetworkReply::finished, m_worker, [this, reply, seeHeaders] {
                    seeHeaders();
                    const auto data = reply->readAll();
                    ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
                    failed(reply, failure);
                    QMutexLocker lock(&m_mutex);
                    m_buffer += data;
                    m_failure = failure;
                    m_done = true;
                    m_wake.wakeAll();
                    m_reply = nullptr;
                    reply->deleteLater();
                });
            }

            mutable QThread m_thread;
            QObject* m_worker = nullptr;
            QNetworkAccessManager* m_nam = nullptr;
            QNetworkReply* m_reply = nullptr;

            mutable QMutex m_mutex;
            mutable QWaitCondition m_wake;
            mutable QByteArray m_buffer;
            std::uint16_t m_status = 0;
            bool m_headersSeen = false;
            bool m_done = false;
            mutable bool m_closed = false;
            ov::TransportFailure m_failure{ov::TransportErrorKind::None, {}};
        };

        // File names from ids the core hands over: anything but [A-Za-z0-9_-] is hex-encoded, so an id can't
        // leave its directory
        QString safeName(rust::Str id)
        {
            const QByteArray raw(id.data(), static_cast<int>(id.size()));
            for (auto c : raw) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
                    return "x" + QString::fromLatin1(raw.toHex());
                }
            }
            return raw.isEmpty() ? QStringLiteral("_") : QString::fromLatin1(raw);
        }

        QString fromSafeName(const QString& name)
        {
            if (name.startsWith('x') && name.size() > 1) {
                const auto decoded = QByteArray::fromHex(name.mid(1).toLatin1());
                if (!decoded.isEmpty()) {
                    return QString::fromUtf8(decoded);
                }
            }
            return name;
        }

        QMutex& cacheMutex()
        {
            static QMutex mutex;
            return mutex;
        }

        bool readFile(const QString& path, rust::Vec<uint8_t>& out)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return false;
            }
            out = toVec(file.readAll());
            return true;
        }

        void writeFile(const QString& path, const uint8_t* data, size_t size)
        {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly)
                || file.write(reinterpret_cast<const char*>(data), static_cast<qint64>(size))
                       != static_cast<qint64>(size)
                || !file.commit()) {
                throw std::runtime_error("internal: writing the offline cache failed");
            }
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }

        const char* const VaultParts[] = {"state", "base", "pending", "autofill"};
    } // namespace

    QString toQString(rust::Str text)
    {
        return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
    }

    QString toQString(const rust::String& text)
    {
        return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
    }

    // ---------------------------------------------------------------------------------------- HTTP

    Transport::Transport(int timeoutMsec)
        : m_timeoutMsec(timeoutMsec)
    {
    }

    ov::HttpResponse Transport::send(const ov::HttpRequest& req, ov::TransportFailure& failure) const
    {
        // One manager per call, on the calling worker thread: it picks up the application's proxy settings
        QNetworkAccessManager nam;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        auto* reply = start(nam, req);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(m_timeoutMsec);
        if (!reply->isFinished()) {
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }

        ov::HttpResponse response;
        if (!reply->isFinished()) {
            reply->abort();
            failure.kind = ov::TransportErrorKind::Timeout;
            failure.message = "the server didn't answer in time";
        } else if (!failed(reply, failure)) {
            response.status =
                static_cast<std::uint16_t>(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
            copyHeaders(reply, response.headers);
            response.body = toVec(reply->readAll());
        }
        delete reply;
        return response;
    }

    std::unique_ptr<ov::ByteStream> Transport::open_stream(const ov::HttpRequest& req,
                                                           ov::TransportFailure& failure) const
    {
        auto stream = std::make_unique<Stream>(req, failure);
        if (!stream->opened()) {
            if (failure.kind == ov::TransportErrorKind::None) {
                failure.kind = ov::TransportErrorKind::Other;
                failure.message = "the stream closed before it started";
            }
            return nullptr;
        }
        return stream;
    }

    // ------------------------------------------------------------------------------------ keychain

    const QString SecretStore::KeyPrefix = QStringLiteral("openvireo-core/");

    bool SecretStore::get(rust::Str key, rust::Vec<std::uint8_t>& value) const
    {
        auto text = SyncTokenStore::load(KeyPrefix + toQString(key));
        if (text.isEmpty()) {
            return false;
        }
        auto bytes = QByteArray::fromBase64(text.toLatin1());
        value = toVec(bytes);
        Botan::secure_scrub_memory(bytes.data(), static_cast<size_t>(bytes.size()));
        return true;
    }

    void SecretStore::set(rust::Str key, rust::Slice<const std::uint8_t> value) const
    {
        auto bytes = QByteArray(reinterpret_cast<const char*>(value.data()), static_cast<int>(value.size()));
        const auto saved = SyncTokenStore::save(KeyPrefix + toQString(key), QString::fromLatin1(bytes.toBase64()));
        Botan::secure_scrub_memory(bytes.data(), static_cast<size_t>(bytes.size()));
        if (!saved) {
            throw std::runtime_error("internal: the keychain refused the secret");
        }
    }

    void SecretStore::remove(rust::Str key) const
    {
        SyncTokenStore::remove(KeyPrefix + toQString(key));
    }

    // ---------------------------------------------------------------------------------- the cache

    FileCache::FileCache(QString root)
        : m_root(root.isEmpty() ? AccountOfflineCache::accountsRoot() : std::move(root))
    {
    }

    QString FileCache::userDir(rust::Str userId) const
    {
        return m_root + "/" + safeName(userId) + "/core";
    }

    bool FileCache::load(rust::Str user_id, rust::Str vault_id, ov::CachedVault& out) const
    {
        QMutexLocker lock(&cacheMutex());
        const auto base = userDir(user_id) + "/vaults/" + safeName(vault_id) + ".";
        if (!readFile(base + VaultParts[0], out.state)) {
            return false;
        }
        out.has_base = readFile(base + VaultParts[1], out.base);
        out.has_pending = readFile(base + VaultParts[2], out.pending);
        out.has_autofill = readFile(base + VaultParts[3], out.autofill);
        return true;
    }

    void FileCache::save(rust::Str user_id, rust::Str vault_id, const ov::CachedVault& v) const
    {
        QMutexLocker lock(&cacheMutex());
        const auto base = userDir(user_id) + "/vaults/" + safeName(vault_id) + ".";
        writeFile(base + VaultParts[0], v.state.data(), v.state.size());
        const std::pair<bool, const rust::Vec<uint8_t>*> optional[] = {
            {v.has_base, &v.base}, {v.has_pending, &v.pending}, {v.has_autofill, &v.autofill}};
        for (int i = 0; i < 3; ++i) {
            const auto path = base + VaultParts[i + 1];
            if (optional[i].first) {
                writeFile(path, optional[i].second->data(), optional[i].second->size());
            } else {
                QFile::remove(path);
            }
        }
    }

    void FileCache::remove(rust::Str user_id, rust::Str vault_id) const
    {
        QMutexLocker lock(&cacheMutex());
        const auto base = userDir(user_id) + "/vaults/" + safeName(vault_id) + ".";
        for (const auto* part : VaultParts) {
            QFile::remove(base + part);
        }
    }

    rust::Vec<rust::String> FileCache::list(rust::Str user_id) const
    {
        QMutexLocker lock(&cacheMutex());
        rust::Vec<rust::String> ids;
        const auto suffix = QStringLiteral(".") + VaultParts[0];
        for (const auto& file : QDir(userDir(user_id) + "/vaults").entryList({"*" + suffix}, QDir::Files)) {
            const auto id = fromSafeName(file.chopped(suffix.size())).toStdString();
            ids.push_back(rust::String(id.data(), id.size()));
        }
        return ids;
    }

    bool FileCache::load_meta(rust::Str user_id, rust::Str name, rust::Vec<std::uint8_t>& value) const
    {
        QMutexLocker lock(&cacheMutex());
        return readFile(userDir(user_id) + "/meta/" + safeName(name), value);
    }

    void FileCache::save_meta(rust::Str user_id, rust::Str name, rust::Slice<const std::uint8_t> value) const
    {
        QMutexLocker lock(&cacheMutex());
        writeFile(userDir(user_id) + "/meta/" + safeName(name), value.data(), value.size());
    }

    void FileCache::clear_user(rust::Str user_id) const
    {
        QMutexLocker lock(&cacheMutex());
        QDir(userDir(user_id)).removeRecursively();
    }

    // ------------------------------------------------------------------------------ clock and log

    std::int64_t SystemClock::now_unix_ms() const
    {
        return ::Clock::currentMilliSecondsSinceEpoch();
    }

    void QtLog::log(ov::LogLevel level, rust::Str target, rust::Str msg) const
    {
        const auto line = QStringLiteral("%1: %2").arg(toQString(target), toQString(msg));
        switch (level) {
        case ov::LogLevel::Error:
            qCCritical(lcOpenVireoCore).noquote() << line;
            break;
        case ov::LogLevel::Warn:
            qCWarning(lcOpenVireoCore).noquote() << line;
            break;
        case ov::LogLevel::Info:
            qCInfo(lcOpenVireoCore).noquote() << line;
            break;
        default:
            qCDebug(lcOpenVireoCore).noquote() << line;
            break;
        }
    }

    ov::Platform platform()
    {
#if defined(Q_OS_MACOS)
        return ov::Platform::QtMacos;
#elif defined(Q_OS_WIN)
        return ov::Platform::QtWindows;
#else
        return ov::Platform::QtLinux;
#endif
    }

    rust::Box<ov::Client> newClient(ov::ClientConfig config,
                                    std::unique_ptr<ov::HttpTransport> http,
                                    std::unique_ptr<ov::SecureStore> store,
                                    std::unique_ptr<ov::BlobCache> cache)
    {
        config.platform = platform();
        return ov::call([&] {
            return ov::new_client(config,
                                  http ? std::move(http) : std::make_unique<Transport>(),
                                  store ? std::move(store) : std::make_unique<SecretStore>(),
                                  cache ? std::move(cache) : std::make_unique<FileCache>(),
                                  std::make_unique<SystemClock>(),
                                  std::make_unique<QtLog>());
        });
    }
} // namespace CoreHost
