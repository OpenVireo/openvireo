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

#include "TestAccountCore.h"

#include "config-keepassx-tests.h"

#include "account/CoreHost.h"
#include "account/OpaqueClient.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "gui/remote/SyncEngine.h"
#include "gui/remote/SyncTokenStore.h"
#include "mock/MockClock.h"

#include <ovcore.h>

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtConcurrent>

QTEST_GUILESS_MAIN(TestAccountCore)

namespace
{
    rust::Slice<const uint8_t> slice(const QByteArray& data)
    {
        return {reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size())};
    }

    QByteArray toByteArray(const rust::Vec<uint8_t>& data)
    {
        return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    }

    rust::String toRust(const QString& text)
    {
        const auto utf8 = text.toUtf8();
        return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
    }

    ov::HttpRequest request(ov::Method method, const QString& url, const QByteArray& body = {})
    {
        ov::HttpRequest req;
        req.method = method;
        req.url = toRust(url);
        req.has_body = !body.isNull();
        for (auto c : body) {
            req.body.push_back(static_cast<uint8_t>(c));
        }
        req.keepalive = false;
        return req;
    }

    QByteArray headerOf(const ov::HttpResponse& response, const QByteArray& name)
    {
        for (const auto& header : response.headers) {
            if (QByteArray(header.name.data(), static_cast<int>(header.name.size())).toLower() == name.toLower()) {
                return QByteArray(header.value.data(), static_cast<int>(header.value.size()));
            }
        }
        return {};
    }

    // Runs `f` on a worker thread, as the desktop calls the core, while this thread serves the test server
    template <class F> auto onWorker(F f) -> decltype(f())
    {
        auto future = QtConcurrent::run(f);
        QElapsedTimer timer;
        timer.start();
        while (!future.isFinished() && timer.elapsed() < 20000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        if (!future.isFinished()) {
            qFatal("the worker didn't finish");
        }
        return future.result();
    }

    QJsonObject loadTranscripts()
    {
        QFile file(QStringLiteral(KEEPASSX_TEST_DATA_DIR "/opaque-transcripts.json"));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    }

    QByteArray hex(const QJsonObject& object, const QString& key)
    {
        return QByteArray::fromHex(object.value(key).toString().toLatin1());
    }
} // namespace

void TestAccountCore::initTestCase()
{
    QVERIFY(Crypto::init());
    QStandardPaths::setTestModeEnabled(true);

    // A small HTTP/1.1 server: one request per connection
    m_server = new QTcpServer(this);
    QVERIFY(m_server->listen(QHostAddress::LocalHost));
    m_base = QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());
    connect(m_server, &QTcpServer::newConnection, this, [this] {
        while (auto* socket = m_server->nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
                socket->setProperty("buffer", buffer);
                const auto end = buffer.indexOf("\r\n\r\n");
                if (end < 0) {
                    return;
                }
                const auto lines = buffer.left(end).split('\n');
                const auto requestLine = lines.first().trimmed().split(' ');
                QHash<QByteArray, QByteArray> headers;
                for (int i = 1; i < lines.size(); ++i) {
                    const auto colon = lines.at(i).indexOf(':');
                    headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
                }
                const auto length = headers.value("content-length").toInt();
                if (buffer.size() < end + 4 + length) {
                    return;
                }
                const auto method = requestLine.value(0);
                const auto path = requestLine.value(1);
                const auto body = buffer.mid(end + 4, length);
                socket->setProperty("buffer", QByteArray());

                const auto reply = [socket](const QByteArray& status, const QByteArray& extra, const QByteArray& data) {
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(data.size())
                                  + "\r\nConnection: close\r\n" + extra + "\r\n" + data);
                    socket->disconnectFromHost();
                };
                if (path == "/hello") {
                    reply("200 OK", "X-Test: yes\r\nContent-Type: text/plain\r\n", "hi");
                } else if (path == "/redirect") {
                    reply("302 Found", "Location: /hello\r\n", {});
                } else if (path == "/echo") {
                    reply("201 Created", "X-Seen-Auth: " + headers.value("authorization") + "\r\n", method + " " + body);
                } else if (path == "/.well-known/openvireo-configuration") {
                    const QJsonObject doc{
                        {"issuer", m_base},
                        {"api", m_base + "/api/v1"},
                        {"sync", m_base + "/sync/v1"},
                        {"jwks_uri", m_base + "/.well-known/jwks.json"},
                        {"opaque",
                         QJsonObject{{"lib", "libopaque"},
                                     {"version", "1.0.1-6e9ac92"},
                                     {"context", "ov-opaque-v1"},
                                     {"ids", "127.0.0.1"}}},
                        {"kdf_min", QJsonObject{{"m", 19456}, {"t", 2}}}};
                    reply("200 OK",
                          "Content-Type: application/json\r\n",
                          QJsonDocument(doc).toJson(QJsonDocument::Compact));
                } else if (path == "/events") {
                    // Two events in two writes, then the server ends the stream
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                                  "data: one\n\n");
                    QTimer::singleShot(100, socket, [socket] {
                        socket->write("data: two\n\n");
                        socket->disconnectFromHost();
                    });
                } else {
                    reply("404 Not Found", {}, "missing");
                }
            });
        }
    });
}

void TestAccountCore::cleanupTestCase()
{
    SyncTokenStore::setBackend(nullptr);
}

void TestAccountCore::testTransport()
{
    const CoreHost::Transport transport;

    auto response = onWorker([&] {
        ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
        auto r = transport.send(request(ov::Method::Get, m_base + "/hello"), failure);
        return std::make_pair(failure.kind, r);
    });
    QCOMPARE(response.first, ov::TransportErrorKind::None);
    QCOMPARE(response.second.status, std::uint16_t(200));
    QCOMPARE(toByteArray(response.second.body), QByteArray("hi"));
    QCOMPARE(headerOf(response.second, "x-test"), QByteArray("yes"));

    // Redirects go back to the core as they are
    response = onWorker([&] {
        ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
        auto r = transport.send(request(ov::Method::Get, m_base + "/redirect"), failure);
        return std::make_pair(failure.kind, r);
    });
    QCOMPARE(response.first, ov::TransportErrorKind::None);
    QCOMPARE(response.second.status, std::uint16_t(302));
    QCOMPARE(headerOf(response.second, "location"), QByteArray("/hello"));

    // Bodies and headers go out, and an error status is a response, not a failure
    response = onWorker([&] {
        ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
        auto req = request(ov::Method::Post, m_base + "/echo", "payload");
        req.headers.push_back(ov::Header{"Authorization", "Bearer abc"});
        req.headers.push_back(ov::Header{"Content-Type", "text/plain"});
        auto r = transport.send(req, failure);
        return std::make_pair(failure.kind, r);
    });
    QCOMPARE(response.second.status, std::uint16_t(201));
    QCOMPARE(toByteArray(response.second.body), QByteArray("POST payload"));
    QCOMPARE(headerOf(response.second, "x-seen-auth"), QByteArray("Bearer abc"));

    response = onWorker([&] {
        ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
        auto r = transport.send(request(ov::Method::Get, m_base + "/nothing"), failure);
        return std::make_pair(failure.kind, r);
    });
    QCOMPARE(response.first, ov::TransportErrorKind::None);
    QCOMPARE(response.second.status, std::uint16_t(404));
}

void TestAccountCore::testTransportOffline()
{
    // A port nothing listens on
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const auto url = QStringLiteral("http://127.0.0.1:%1/").arg(closed.serverPort());
    closed.close();

    const CoreHost::Transport transport;
    const auto kind = onWorker([&] {
        ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
        transport.send(request(ov::Method::Get, url), failure);
        return failure.kind;
    });
    QCOMPARE(kind, ov::TransportErrorKind::Offline);
}

void TestAccountCore::testStream()
{
    const CoreHost::Transport transport;
    const auto result = onWorker([&] {
        ov::TransportFailure failure{ov::TransportErrorKind::None, {}};
        auto stream = transport.open_stream(request(ov::Method::Get, m_base + "/events"), failure);
        if (!stream) {
            return std::make_pair(std::uint16_t(0), QByteArray());
        }
        QByteArray all;
        rust::Vec<uint8_t> chunk;
        while (stream->next_chunk(chunk, failure)) {
            all += toByteArray(chunk);
        }
        return std::make_pair(stream->status(), all);
    });
    QCOMPARE(result.first, std::uint16_t(200));
    QCOMPARE(result.second, QByteArray("data: one\n\ndata: two\n\n"));
}

void TestAccountCore::testDiscoveryThroughTheCore()
{
    QTemporaryDir cacheDir;
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    ov::ClientConfig config;
    config.device_id = "device";
    config.device_name = "test";
    auto client = CoreHost::newClient(config, {}, {}, std::make_unique<CoreHost::FileCache>(cacheDir.path()));

    const auto server = onWorker([&] {
        auto cancel = ov::new_cancel_token();
        return ov::call([&] { return client->discover(toRust(m_base), *cancel); });
    });
    QCOMPARE(CoreHost::toQString(server.api), m_base + "/api/v1");
    QCOMPARE(CoreHost::toQString(server.sync), m_base + "/sync/v1");
    QCOMPARE(CoreHost::toQString(server.opaque_context), QString("ov-opaque-v1"));
    QVERIFY(server.has_kdf_min);
    QCOMPARE(server.kdf_min_m, 19456u);

    // An issuer that isn't there comes back as the core's network error
    const auto code = onWorker([&] {
        auto cancel = ov::new_cancel_token();
        try {
            ov::call([&] { return client->discover("http://127.0.0.1:1", *cancel); });
        } catch (const ov::Error& e) {
            return QString::fromStdString(e.code());
        }
        return QString();
    });
    QCOMPARE(code, QString("network_error"));
}

void TestAccountCore::testSecretStore()
{
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    const CoreHost::SecretStore store;
    rust::Vec<uint8_t> value;
    QVERIFY(!store.get("session", value));

    const QByteArray secret("\x00\x01secret\xff", 9);
    store.set("session", slice(secret));
    QVERIFY(store.get("session", value));
    QCOMPARE(toByteArray(value), secret);
    // Kept in the keychain the account code uses, under its own prefix
    QVERIFY(!SyncTokenStore::load(CoreHost::SecretStore::KeyPrefix + "session").isEmpty());

    store.remove("session");
    QVERIFY(!store.get("session", value));
    store.remove("session");
}

void TestAccountCore::testFileCache()
{
    QTemporaryDir root;
    const CoreHost::FileCache cache(root.path());
    ov::CachedVault vault;
    QVERIFY(!cache.load("user-1", "vault-1", vault));

    vault.state.push_back(1);
    vault.has_pending = true;
    vault.pending.push_back(2);
    cache.save("user-1", "vault-1", vault);
    // An id that isn't a plain name stays inside the cache
    cache.save("user-1", "../escape", vault);

    ov::CachedVault loaded;
    QVERIFY(cache.load("user-1", "vault-1", loaded));
    QCOMPARE(loaded.state.size(), size_t(1));
    QVERIFY(!loaded.has_base);
    QVERIFY(loaded.has_pending);
    QCOMPARE(loaded.pending[0], uint8_t(2));
    QVERIFY(!QFile::exists(root.path() + "/user-1/escape.state"));

    QStringList ids;
    for (const auto& id : cache.list("user-1")) {
        ids << CoreHost::toQString(id);
    }
    ids.sort();
    QCOMPARE(ids, QStringList({"../escape", "vault-1"}));

    const QByteArray meta("config");
    cache.save_meta("user-1", "discovery", slice(meta));
    rust::Vec<uint8_t> value;
    QVERIFY(cache.load_meta("user-1", "discovery", value));
    QCOMPARE(toByteArray(value), meta);

    cache.remove("user-1", "vault-1");
    QVERIFY(!cache.load("user-1", "vault-1", loaded));
    cache.clear_user("user-1");
    QVERIFY(!cache.load("user-1", "../escape", loaded));
    QVERIFY(!cache.load_meta("user-1", "discovery", value));
}

void TestAccountCore::testLibopaqueAccountSignsIn()
{
    // An account registered by libopaque (the desktop's OPAQUE before openvireo-core, and the auth server's),
    // signing in with the core's opaque-ke client: openvireo-core vectors/opaque-transcripts.json
    const auto transcripts = loadTranscripts();
    const auto libopaque = transcripts.value("libopaque").toObject();
    QVERIFY(!libopaque.isEmpty());
    const auto login = libopaque.value("login_opaque_ke_client").toObject();
    const auto password = libopaque.value("password").toString().toUtf8();

    // The desktop's ids and context: the email as OpaqueClient sends it, the issuer host
    const auto user = OpaqueClient::userId(libopaque.value("id_u").toString());
    const auto server = libopaque.value("id_s").toString().toUtf8();
    QCOMPARE(QString::fromUtf8(OpaqueClient::Context), libopaque.value("context").toString());

    auto start = ov::opaque_start_login_seeded(slice(password), slice(hex(login, "client_start_seed_hex")));
    QCOMPARE(toByteArray(start.message).toHex(), hex(login, "ke1_hex").toHex());

    // ke2 is what the libopaque server answered for its stored record
    auto finish = ov::opaque_finish_login_seeded(slice(toByteArray(start.state)),
                                                  slice(hex(login, "ke2_hex")),
                                                  slice(user),
                                                  slice(server),
                                                  slice(OpaqueClient::Context),
                                                  slice(hex(login, "client_finish_seed_hex")));
    QCOMPARE(toByteArray(finish.auth_u).toHex(), hex(login, "expected_auth_u_hex").toHex());
    QCOMPARE(toByteArray(finish.session_key).toHex(), hex(login, "session_key_hex").toHex());
    QCOMPARE(toByteArray(finish.export_key).toHex(),
             hex(libopaque.value("registration").toObject(), "export_key_hex").toHex());

    // A wrong password fails like a wrong record
    auto wrong = ov::opaque_start_login_seeded(slice(QByteArray("wrong password")),
                                                slice(hex(login, "client_start_seed_hex")));
    bool refused = false;
    try {
        ov::opaque_finish_login_seeded(slice(toByteArray(wrong.state)),
                                        slice(hex(login, "ke2_hex")),
                                        slice(user),
                                        slice(server),
                                        slice(OpaqueClient::Context),
                                        slice(hex(login, "client_finish_seed_hex")));
    } catch (const rust::Error& e) {
        refused = QByteArray(e.what()).startsWith("opaque_failed");
    }
    QVERIFY(refused);
}

void TestAccountCore::testAccountVaultMergeKeepsObjects()
{
    auto* clock = new MockClock(2026, 11, 1, 12, 0, 0);
    MockClock::setup(clock);

    // The copy both devices last synced: a group with two entries
    auto base = QSharedPointer<Database>::create();
    base->rootGroup()->setName("Vault");
    auto* work = new Group();
    work->setUuid(QUuid::createUuid());
    work->setName("Work");
    work->setParent(base->rootGroup());
    const auto addEntry = [](Group* group, const QString& title, const QString& password) {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(title);
        entry->setPassword(password);
        entry->setGroup(group);
        return entry;
    };
    const auto sharedUuid = addEntry(work, "Shared", "one")->uuid();
    const auto goneUuid = addEntry(work, "Gone", "x")->uuid();
    const auto copyOf = [](const Database* db) {
        auto copy = QSharedPointer<Database>::create();
        delete copy->setRootGroup(db->rootGroup()->clone(Entry::CloneIncludeHistory, Group::CloneIncludeEntries));
        return copy;
    };
    auto local = copyOf(base.data());
    auto remote = copyOf(base.data());

    // The other device changed the password, deleted an entry and added one
    clock->advanceMinute(5);
    auto* remoteShared = remote->rootGroup()->findEntryByUuid(sharedUuid);
    remoteShared->beginUpdate();
    remoteShared->setPassword("two");
    remoteShared->endUpdate();
    delete remote->rootGroup()->findEntryByUuid(goneUuid);
    const auto newUuid = addEntry(remote->rootGroup()->findGroupByUuid(work->uuid()), "New", "n")->uuid();

    // What the window holds on to
    auto* selected = local->rootGroup()->findEntryByUuid(sharedUuid);
    auto* selectedGroup = local->rootGroup()->findGroupByUuid(work->uuid());
    clock->advanceMinute(5);

    bool ok = false;
    const auto changes = SyncEngine::mergeAccountVault(local.data(), remote.data(), base.data(), &ok);
    QVERIFY(ok);
    QVERIFY(!changes.isEmpty());

    // The same objects, with the merged content
    QCOMPARE(local->rootGroup()->findEntryByUuid(sharedUuid), selected);
    QCOMPARE(local->rootGroup()->findGroupByUuid(work->uuid()), selectedGroup);
    QCOMPARE(selected->password(), QString("two"));
    QCOMPARE(selected->group(), selectedGroup);
    QVERIFY(!selected->historyItems().isEmpty());
    QVERIFY(!local->rootGroup()->findEntryByUuid(goneUuid));
    QVERIFY(local->containsDeletedObject(goneUuid));
    auto* added = local->rootGroup()->findEntryByUuid(newUuid);
    QVERIFY(added);
    QCOMPARE(added->group(), selectedGroup);
    QCOMPARE(added->password(), QString("n"));

    // Both copies end up alike, one sync generation on
    QVERIFY(remote->rootGroup()->findEntryByUuid(newUuid));
    QCOMPARE(remote->rootGroup()->findEntryByUuid(sharedUuid)->password(), QString("two"));
    QCOMPARE(SyncEngine::generationOf(local.data()), SyncEngine::generationOf(remote.data()));
    QVERIFY(SyncEngine::generationOf(local.data()) > 0);

    // Merging again changes nothing
    ok = false;
    QVERIFY(SyncEngine::mergeAccountVault(local.data(), remote.data(), nullptr, &ok).isEmpty());
    QVERIFY(ok);
    MockClock::teardown();
}
