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

#ifndef OPENVIREO_COREHOST_H
#define OPENVIREO_COREHOST_H

#include <QString>

#include <ovcore.h>

#include <functional>
#include <memory>

/**
 * The Host of openvireo-core on the desktop (rust-core-tasks.md §2.2): the core does no I/O of its own and
 * calls these back for it.
 *
 *   HttpTransport  QNetworkAccessManager, so Qt's proxy settings apply as they do to the sync server client
 *   SecureStore    the keychain the account code already uses (SyncTokenStore)
 *   BlobCache      files beside AccountOfflineCache, in <accounts root>/<user>/core/
 *   Clock          core/Clock (so MockClock works in tests)
 *   LogSink        qDebug and friends, category "openvireo.core"
 *
 * Core calls block, and so do these callbacks: make core calls from a worker thread (QtConcurrent), never
 * from the GUI thread. Every callback is safe to call from several threads at once.
 */
namespace CoreHost
{
    class Transport : public ov::HttpTransport
    {
    public:
        // `timeoutMsec` bounds one request without a response; streams wait as long as the server keeps them
        explicit Transport(int timeoutMsec = 60 * 1000);
        ov::HttpResponse send(const ov::HttpRequest& req, ov::TransportFailure& failure) const override;
        std::unique_ptr<ov::ByteStream> open_stream(const ov::HttpRequest& req,
                                                    ov::TransportFailure& failure) const override;

    private:
        int m_timeoutMsec;
    };

    class SecretStore : public ov::SecureStore
    {
    public:
        // Keys go to SyncTokenStore under this prefix
        static const QString KeyPrefix;
        bool get(rust::Str key, rust::Vec<std::uint8_t>& value) const override;
        void set(rust::Str key, rust::Slice<const std::uint8_t> value) const override;
        void remove(rust::Str key) const override;
    };

    class FileCache : public ov::BlobCache
    {
    public:
        // AccountOfflineCache::accountsRoot() when empty
        explicit FileCache(QString root = {});
        bool load(rust::Str user_id, rust::Str vault_id, ov::CachedVault& out) const override;
        void save(rust::Str user_id, rust::Str vault_id, const ov::CachedVault& v) const override;
        void remove(rust::Str user_id, rust::Str vault_id) const override;
        rust::Vec<rust::String> list(rust::Str user_id) const override;
        bool load_meta(rust::Str user_id, rust::Str name, rust::Vec<std::uint8_t>& value) const override;
        void save_meta(rust::Str user_id, rust::Str name, rust::Slice<const std::uint8_t> value) const override;
        void clear_user(rust::Str user_id) const override;

    private:
        QString userDir(rust::Str userId) const;
        QString m_root;
    };

    class SystemClock : public ov::Clock
    {
    public:
        std::int64_t now_unix_ms() const override;
    };

    class QtLog : public ov::LogSink
    {
    public:
        void log(ov::LogLevel level, rust::Str target, rust::Str msg) const override;
    };

    // This device's platform for the core
    ov::Platform platform();

    // A client of the core with the Qt Host. `config.platform` is filled in.
    rust::Box<ov::Client> newClient(ov::ClientConfig config,
                                    std::unique_ptr<ov::HttpTransport> http = {},
                                    std::unique_ptr<ov::SecureStore> store = {},
                                    std::unique_ptr<ov::BlobCache> cache = {},
                                    std::unique_ptr<ov::Clock> clock = {});

    /**
     * Makes the Host parts for a core client. Each part left empty is the production one; tests plug in
     * ov::TestServers (tests/util/CoreTestServers) or another fake here. AccountManager takes one of these.
     */
    struct Factory
    {
        std::function<std::unique_ptr<ov::HttpTransport>()> http;
        std::function<std::unique_ptr<ov::SecureStore>()> store;
        std::function<std::unique_ptr<ov::BlobCache>()> cache;
        std::function<std::unique_ptr<ov::Clock>()> clock;

        std::unique_ptr<ov::HttpTransport> makeTransport() const;
        std::unique_ptr<ov::Clock> makeClock() const;
        rust::Box<ov::Client> newClient(ov::ClientConfig config) const;
    };

    QString toQString(rust::Str text);
    QString toQString(const rust::String& text);
} // namespace CoreHost

#endif // OPENVIREO_COREHOST_H
