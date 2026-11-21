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

#include "TestAccountManager.h"

#include "account/AccountManager.h"
#include "account/AccountOfflineCache.h"
#include "account/CoreHost.h"
#include "account/VaultKeyring.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "format/KeePass2Writer.h"
#include "gui/remote/SyncEventStream.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/remote/SyncTokenStore.h"
#include "util/CoreTestServers.h"

#include <ovcore.h>

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>

QTEST_GUILESS_MAIN(TestAccountManager)

using namespace AccountCrypto;

/*
 * AccountManager on openvireo-core's in-process auth and sync servers (ov::TestServers): the desktop's Host
 * (keychain in memory, offline cache files) with the servers behind its transport and clock. Server state is
 * made through the facade on "another device" of the same servers, and through the servers' seeding hooks.
 */
namespace
{
    const QString Email = QStringLiteral("Alice@Example.com");
    const QString Password = QStringLiteral("correct horse battery staple");
    // The test servers' issuer is https://auth.test; OPAQUE's idS and the keychain keys use its host
    const QString ServerId = QStringLiteral("auth.test");

    rust::Vec<uint8_t> bytesOf(const QString& text)
    {
        rust::Vec<uint8_t> vec;
        for (auto c : text.toUtf8()) {
            vec.push_back(static_cast<uint8_t>(c));
        }
        return vec;
    }

    rust::String rs(const QString& text)
    {
        const auto utf8 = text.toUtf8();
        return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
    }

    QString qs(const rust::String& text)
    {
        return CoreHost::toQString(text);
    }

    // The cheapest settings clients accept, to keep the tests quick
    KdfParams testKdf()
    {
        auto kdf = KdfParams::generate();
        kdf.memoryKiB = KdfParams::MinMemoryKiB;
        kdf.iterations = KdfParams::MinIterations;
        return kdf;
    }

    struct Fixture
    {
        rust::Box<ov::TestServers> servers = ov::call([] { return ov::test_servers_start(0); });
        // Managers made here go before the servers they talk to
        QList<QPointer<AccountManager>> managers;

        ~Fixture()
        {
            for (const auto& manager : managers) {
                delete manager.data();
            }
        }

        AccountManager::ServerConfig serverConfig() const
        {
            AccountManager::ServerConfig config;
            config.issuer = qs(servers->issuer());
            config.api = qs(servers->api_url());
            config.sync = qs(servers->sync_url());
            return config;
        }

        CoreHost::Factory host() const
        {
            const ov::TestServers* s = &*servers;
            CoreHost::Factory factory;
            factory.http = [s] { return std::make_unique<CoreTestServers::Transport>(s); };
            factory.clock = [s] { return std::make_unique<CoreTestServers::Clock>(s); };
            return factory;
        }

        AccountManager* manager(QObject* parent)
        {
            auto* manager = new AccountManager(host(), parent);
            manager->setServer(serverConfig());
            manager->setNewAccountKdf(testKdf());
            manager->setEventStreamEnabled(false);
            managers << manager;
            return manager;
        }

        // The requests the servers answered since the last clear, "<status> <server> <METHOD> <route>"
        QStringList requests() const
        {
            QStringList list;
            for (const auto& line : servers->requests()) {
                list << qs(line);
            }
            return list;
        }

        // How many requests went to a route, e.g. "POST /auth/refresh"
        int count(const QString& route) const
        {
            int n = 0;
            for (const auto& line : requests()) {
                n += line.endsWith(" " + route) ? 1 : 0;
            }
            return n;
        }

        // Alice (or someone) on another device: a core account on the servers' own Host, signed in
        rust::Box<ov::Account> device(const QString& email, const QString& password, const QString& secretKey) const
        {
            ov::ClientConfig config;
            config.device_name = "other device";
            config.platform = CoreHost::platform();
            auto client = ov::call([&] { return servers->client(config); });
            auto cancel = ov::new_cancel_token();
            auto account = ov::call([&] { return client->connect(servers->issuer(), *cancel); });
            ov::LoginInput input;
            input.email = rs(email);
            input.password = bytesOf(password);
            input.has_secret_key = true;
            input.secret_key = bytesOf(secretKey);
            input.trust_device = true;
            auto step = ov::call([&] { return account->login(std::move(input), *cancel); });
            if (step->needs_totp()) {
                auto challenge = step->challenge();
                step = ov::call([&] { return account->submit_totp(*challenge, servers->totp_now(rs(email)), *cancel); });
            }
            ov::call([&] { return step->take_unlocked(); });
            return account;
        }
    };

    bool waitForState(AccountManager* manager, AccountManager::State state, int timeout = 20000)
    {
        QElapsedTimer timer;
        timer.start();
        while (manager->state() != state && timer.elapsed() < timeout) {
            QTest::qWait(10);
        }
        return manager->state() == state;
    }

    // The session the core keeps in the keychain for a user: the active one, else the one set aside
    QJsonObject coreSession(const QString& userId)
    {
        const auto read = [](const QString& key) {
            const auto raw = SyncTokenStore::load(CoreHost::SecretStore::KeyPrefix + key);
            return QJsonDocument::fromJson(QByteArray::fromBase64(raw.toLatin1())).object();
        };
        auto active = read("ov:session");
        if (!userId.isEmpty() && active.value("userId").toString() == userId) {
            return active;
        }
        return read("ov:parked_session:" + ServerId + ":" + userId);
    }

    QString refreshTokenOf(const QString& userId)
    {
        return coreSession(userId).value("refreshToken").toString();
    }

    // Register Alice and sign her out again, leaving her Secret Key on this device
    QString registerAlice(Fixture& fixture, QObject* parent, bool mfa = false)
    {
        auto* manager = fixture.manager(parent);
        QSignalSpy registered(manager, &AccountManager::registered);
        manager->registerAccount(Email, Password);
        if (!waitForState(manager, AccountManager::State::Unlocked) || registered.count() != 1) {
            return {};
        }
        const auto id = manager->accountId();
        manager->signOut();
        // The sign-out finishes on the core's worker
        QElapsedTimer timer;
        timer.start();
        while (!coreSession(id).isEmpty() && timer.elapsed() < 5000) {
            QTest::qWait(10);
        }
        delete manager;
        if (mfa) {
            fixture.servers->set_totp_code(rs(Email), "123456");
        }
        return registered.first().first().toString();
    }

    // A vault made on another device of the account, with one entry
    QString createVault(const ov::Account& device, const QString& name, const QString& entryTitle = {})
    {
        auto cancel = ov::new_cancel_token();
        auto vault = ov::call([&] { return device.create_vault(rs(name), "", *cancel); });
        if (!entryTitle.isEmpty()) {
            ov::EntryInput input{};
            ov::FieldInput title{};
            title.name = "Title";
            title.value = bytesOf(entryTitle);
            input.fields.push_back(std::move(title));
            const auto root = ov::call([&] { return vault->root_group_id(); });
            ov::call([&] { return vault->add_entry(root, std::move(input)); });
            ov::call([&] { return vault->sync(true, *cancel); });
        }
        return qs(ov::call([&] { return vault->id(); }));
    }

    QByteArray blobOf(Fixture& fixture, const QString& vaultId)
    {
        const auto blob = fixture.servers->vault_blob(rs(vaultId));
        return blob.has ? QByteArray(reinterpret_cast<const char*>(blob.value.data()), static_cast<int>(blob.value.size()))
                        : QByteArray();
    }
} // namespace

void TestAccountManager::initTestCase()
{
    QVERIFY(Crypto::init());
    // The offline cache goes under AppDataLocation
    QStandardPaths::setTestModeEnabled(true);
}

void TestAccountManager::init()
{
    // Never touch the real Keychain
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    config()->set(Config::Account_AllowOfflineUnlock, true);
    QDir(AccountOfflineCache::accountsRoot()).removeRecursively();
}

void TestAccountManager::testRegisterAndSignIn()
{
    Fixture fixture;
    auto* manager = fixture.manager(this);
    QSignalSpy registered(manager, &AccountManager::registered);
    QSignalSpy vaultsChanged(manager, &AccountManager::vaultsChanged);
    manager->registerAccount(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));

    // The Secret Key is shown once, and this device keeps it
    QCOMPARE(registered.count(), 1);
    const auto secretKey = registered.first().first().toString();
    SecureBytes parsed;
    QVERIFY(parseSecretKey(secretKey, &parsed));
    QCOMPARE(AccountManager::storedSecretKey(ServerId, Email), secretKey);
    QCOMPARE(AccountManager::lastAccountId(), manager->accountId());
    QCOMPARE(manager->accountId(), qs(fixture.servers->user_id(rs(Email))));

    // The server has the public keys of the keyset this device holds; the private keys never left it
    QVERIFY(verifyPublicKeys(manager->keyset().publicKeys));
    auto other = fixture.device(Email, Password, secretKey);
    auto cancel = ov::new_cancel_token();
    const auto published =
        ov::call([&] { return other->verified_public_keys(rs(manager->principal()), *cancel); });
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(published.enc_pk.data()), static_cast<int>(published.enc_pk.size())),
             fromSecure(SecureBytes(manager->keyset().publicKeys.encPk.begin(), manager->keyset().publicKeys.encPk.end())));
    QCOMPARE(manager->principal(), "user:" + manager->accountId());
    QVERIFY(!manager->accessTokenNow().isEmpty());
    QVERIFY(!fixture.requests().isEmpty());
    QTRY_VERIFY(vaultsChanged.count() > 0);

    const auto encPk = manager->keyset().publicKeys.encPk;
    fixture.servers->clear_requests();
    manager->signOut();
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY(manager->keyset().isNull());
    QVERIFY(!manager->plan().isKnown());
    QVERIFY(manager->accessTokenNow().isEmpty());
    QTRY_COMPARE(fixture.count("POST /auth/logout"), 1);

    // Signing in again on this device needs only the password
    manager->signIn(Email.toUpper(), Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QCOMPARE(manager->keyset().publicKeys.encPk, encPk);
}

void TestAccountManager::testSignInWithMfa()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this, true);
    QVERIFY(!secretKey.isEmpty());

    // A new device: the Secret Key is entered by hand
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    auto* manager = fixture.manager(this);
    QSignalSpy mfa(manager, &AccountManager::mfaRequired);
    QSignalSpy errors(manager, &AccountManager::error);
    manager->signIn(Email, Password, secretKey);
    QVERIFY(waitForState(manager, AccountManager::State::NeedsMfa));
    QCOMPARE(mfa.count(), 1);
    QCOMPARE(mfa.first().first().toStringList(), QStringList({"totp"}));
    QVERIFY(manager->keyset().isNull());

    // A wrong code can be retried
    manager->submitMfa("000000");
    QTRY_COMPARE(errors.count(), 1);
    QCOMPARE(manager->state(), AccountManager::State::NeedsMfa);

    manager->submitMfa("123456");
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QVERIFY(!manager->keyset().isNull());
    // The device is trusted now
    QCOMPARE(AccountManager::storedSecretKey(ServerId, Email), secretKey);
}

void TestAccountManager::testFifthWrongMfaCodeEndsSignIn()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this, true);
    QVERIFY(!secretKey.isEmpty());

    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    auto* manager = fixture.manager(this);
    QSignalSpy errors(manager, &AccountManager::error);
    manager->signIn(Email, Password, secretKey);
    QVERIFY(waitForState(manager, AccountManager::State::NeedsMfa));

    for (int i = 1; i <= 5; ++i) {
        manager->submitMfa("000000");
        QTRY_COMPARE(errors.count(), i);
        QCOMPARE(manager->state(), AccountManager::State::NeedsMfa);
    }

    // The server dropped the token: the right code no longer works and sign-in starts over
    manager->submitMfa("123456");
    QVERIFY(waitForState(manager, AccountManager::State::SignedOut));
    QCOMPARE(errors.count(), 6);
    QVERIFY(manager->keyset().isNull());
}

void TestAccountManager::testWrongPasswordOrSecretKey()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this);
    QVERIFY(!secretKey.isEmpty());

    auto* manager = fixture.manager(this);
    QSignalSpy errors(manager, &AccountManager::error);
    QSignalSpy signedOut(manager, &AccountManager::signedOut);
    fixture.servers->clear_requests();
    manager->signIn(Email, "wrong password");
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY(errors.first().first().toString().contains("password is wrong"));
    // OPAQUE fails on this device; login/finish is never even sent
    QCOMPARE(fixture.count("POST /auth/login/finish"), 0);

    // Another Secret Key: the server accepts the password, but the keyset doesn't unwrap
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    SecureBytes other = generateSecretKey();
    manager->signIn(Email, Password, formatSecretKey(other));
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 20000);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY2(errors.last().first().toString().contains("Secret Key"), qPrintable(errors.last().first().toString()));
    QVERIFY(manager->keyset().isNull());
    // The core unlocks before it keeps the session: none is kept, and the wrong key isn't remembered
    QVERIFY(coreSession({}).isEmpty());
    QVERIFY(AccountManager::storedSecretKey(ServerId, Email).isEmpty());
    QCOMPARE(signedOut.count(), 0);

    // A malformed Secret Key doesn't reach the server
    fixture.servers->clear_requests();
    manager->signIn(Email, Password, "K1-not-a-key");
    QCOMPARE(errors.count(), 3);
    QVERIFY(fixture.requests().isEmpty());
}

void TestAccountManager::testRefreshRotatesTokens()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));

    const auto firstAccess = manager->accessTokenNow();
    const auto firstRefresh = refreshTokenOf(manager->accountId());
    QVERIFY(!firstRefresh.isEmpty());

    QSignalSpy changed(manager, &AccountManager::accessTokenChanged);
    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 1);
    QVERIFY(manager->accessTokenNow() != firstAccess);
    // The keychain holds the new refresh token
    const auto secondRefresh = refreshTokenOf(manager->accountId());
    QVERIFY(!secondRefresh.isEmpty());
    QVERIFY(secondRefresh != firstRefresh);

    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 2);
    QVERIFY(refreshTokenOf(manager->accountId()) != secondRefresh);
    QCOMPARE(manager->state(), AccountManager::State::Unlocked);
}

void TestAccountManager::testRefreshesExpiringTokens()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    fixture.servers->clear_requests();
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QTRY_COMPARE(fixture.count("GET /vaults"), 1);

    // A token that is about to expire as soon as it is issued
    fixture.servers->set_access_token_ttl_secs(AccountManager::MinTokenLifetimeSec - 5);
    QSignalSpy changed(manager, &AccountManager::accessTokenChanged);
    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 1);
    fixture.servers->clear_requests();
    const auto expiring = manager->accessTokenNow();

    // Asking for a token refreshes first, and everyone waiting gets the new one
    fixture.servers->set_access_token_ttl_secs(600);
    QStringList tokens;
    manager->accessToken([&tokens](const QString& token) { tokens << token; });
    manager->accessToken([&tokens](const QString& token) { tokens << token; });
    QTRY_COMPARE(tokens.size(), 2);
    QCOMPARE(fixture.count("POST /auth/refresh"), 1);
    QVERIFY(tokens.first() != expiring);
    QCOMPARE(tokens.first(), tokens.last());
    QCOMPARE(tokens.first(), manager->accessTokenNow());

    // A fresh token is handed out as it is
    manager->accessToken([&tokens](const QString& token) { tokens << token; });
    QCOMPARE(tokens.size(), 3);
    QCOMPARE(fixture.count("POST /auth/refresh"), 1);

    // Before it expires, the timer refreshes on its own
    fixture.servers->set_access_token_ttl_secs(AccountManager::RefreshMarginSec + 1);
    manager->refreshAccessToken();
    QTRY_COMPARE(fixture.count("POST /auth/refresh"), 2);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.count("POST /auth/refresh"), 3, 5000);
}

void TestAccountManager::testReusedRefreshTokenSignsOut()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    const auto accountId = manager->accountId();

    // Someone copies the keychain entry, then this device refreshes
    const auto key = CoreHost::SecretStore::KeyPrefix + "ov:session";
    const auto stolen = SyncTokenStore::load(key);
    QVERIFY(!stolen.isEmpty());
    QSignalSpy changed(manager, &AccountManager::accessTokenChanged);
    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 1);
    // Past the server's grace period for a lost replacement token (SPEC §9)
    fixture.servers->advance_ms(61 * 1000);

    // The copy's refresh token was rotated already: using it revokes the session...
    auto* thief = fixture.manager(this);
    SyncTokenStore::save(key, stolen);
    QVERIFY(thief->restoreSession(accountId));
    QSignalSpy thiefOut(thief, &AccountManager::signedOut);
    QString thiefToken = "unset";
    thief->accessToken([&thiefToken](const QString& token) { thiefToken = token; });
    QTRY_COMPARE(thiefOut.count(), 1);
    QVERIFY(thiefToken.isEmpty());
    QVERIFY(thiefOut.first().first().toString().contains("used twice"));
    QCOMPARE(thief->state(), AccountManager::State::SignedOut);

    // ...so this device is signed out at its next refresh, and its keys are gone
    QSignalSpy signedOut(manager, &AccountManager::signedOut);
    manager->refreshAccessToken();
    QTRY_COMPARE(signedOut.count(), 1);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY(manager->keyset().isNull());
    QVERIFY(manager->keyring().isEmpty());
    QVERIFY(coreSession(accountId).isEmpty());
    // The Secret Key stays: the device is still trusted for the next sign-in
    QVERIFY(!AccountManager::storedSecretKey(ServerId, Email).isEmpty());
}

void TestAccountManager::testServiceAccountExchange()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this);
    QVERIFY(!secretKey.isEmpty());
    // An org with a service account, made by Alice on another device
    auto alice = fixture.device(Email, Password, secretKey);
    auto cancel = ov::new_cancel_token();
    const auto org = ov::call([&] { return alice->create_org("Acme", *cancel); });
    ov::NewServiceAccount input;
    input.name = "ci";
    auto created = ov::call([&] { return alice->create_service_account(org.id, input, *cancel); });
    const auto token = QString::fromUtf8(reinterpret_cast<const char*>(created.token.data()),
                                         static_cast<int>(created.token.size()));
    const auto saId = qs(created.service_account.id);

    fixture.servers->clear_requests();
    auto* manager = fixture.manager(this);
    QSignalSpy signedIn(manager, &AccountManager::signedIn);
    manager->signInServiceAccount(token);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QCOMPARE(signedIn.count(), 1);
    QVERIFY(manager->isServiceAccount());
    QCOMPARE(manager->principal(), "sa:" + saId);
    // kek -> master key -> keyset, matching the public keys the server holds
    QVERIFY(!manager->keyset().isNull());
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(created.service_account.public_keys.enc_pk.data()),
                        static_cast<int>(created.service_account.public_keys.enc_pk.size())),
             fromSecure(SecureBytes(manager->keyset().publicKeys.encPk.begin(), manager->keyset().publicKeys.encPk.end())));
    // Only the secret went to the server, never the kek, and there is no refresh token
    QCOMPARE(fixture.count("POST /auth/refresh"), 0);

    // No refresh token: an expiring token is exchanged again
    const auto first = manager->accessTokenNow();
    fixture.servers->advance_ms(1000);
    manager->refreshAccessToken();
    QTRY_VERIFY(manager->accessTokenNow() != first);
    QCOMPARE(fixture.count("POST /auth/service-account"), 2);
    QCOMPARE(manager->state(), AccountManager::State::Unlocked);

    // A rejected secret
    ov::call([&] { return alice->delete_service_account(rs(saId), *cancel); });
    auto* rejected = fixture.manager(this);
    QSignalSpy errors(rejected, &AccountManager::error);
    rejected->signInServiceAccount(token);
    QTRY_COMPARE(errors.count(), 1);
    QCOMPARE(rejected->state(), AccountManager::State::SignedOut);
    QVERIFY(rejected->keyset().isNull());
}

void TestAccountManager::testKeyringOpensNewestKeyWithFallback()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this);
    QVERIFY(!secretKey.isEmpty());
    auto alice = fixture.device(Email, Password, secretKey);
    auto cancel = ov::new_cancel_token();

    // "rotating": version 2 is sealed and pending, but the upload under it failed, so the blob is still under 1
    const auto rotating = createVault(*alice, "Team", "Old key entry");
    fixture.servers->fail_next("sync", "PUT", rs("/vaults/" + rotating), 503, 20);
    try {
        ov::call([&] { return alice->rotate_vault_key(rs(rotating), {}, *cancel); });
    } catch (const ov::Error&) {
    }
    fixture.servers->clear_faults();
    // "rotated": the blob is already re-keyed with version 2
    const auto rotated = createVault(*alice, "Infra", "New key entry");
    ov::call([&] { return alice->rotate_vault_key(rs(rotated), {}, *cancel); });

    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QTRY_VERIFY(manager->keyring().contains(rotated));

    const auto& keyring = manager->keyring();
    QCOMPARE(keyring.vaults().size(), 2);
    // The core hands out each vault's newest key; older versions stay inside it
    QCOMPARE(keyring.keyVersions(rotating), QList<int>({2}));
    QCOMPARE(keyring.vault(rotating).pendingKeyVersion, 2);
    QCOMPARE(keyring.vault(rotating).name, QString("Team"));
    QCOMPARE(keyring.vault(rotated).name, QString("Infra"));

    // A copy under the older key opens through a sync in the core, which tries the newest key first, then the
    // older one, and hands the file back with the key it is under
    int version = 0;
    QString error;
    QVERIFY(!keyring.openVault(rotating, blobOf(fixture, rotating), &version, &error));
    std::optional<AccountVaultSync::Result> synced;
    AccountVaultSync::Request request;
    request.vaultId = rotating;
    request.kdbx = blobOf(fixture, rotating);
    request.localChanged = false;
    manager->syncVault(request, [&](const AccountVaultSync::Result& result) { synced = result; });
    QTRY_VERIFY_WITH_TIMEOUT(synced.has_value(), 10000);
    QVERIFY2(synced->ok(), qPrintable(synced->error));
    auto db = VaultKeyring::readVault(synced->kdbx, AccountManager::vaultCompositeKey(synced->keyFile), &error);
    QVERIFY2(db, qPrintable(error));
    QCOMPARE(db->rootGroup()->entries().first()->title(), QString("Old key entry"));

    db = keyring.openVault(rotated, blobOf(fixture, rotated), &version, &error);
    QVERIFY2(db, qPrintable(error));
    QCOMPARE(version, 2);
    QCOMPARE(db->rootGroup()->entries().first()->title(), QString("New key entry"));

    // A blob of another vault doesn't open: the vault key AAD names the vault
    QVERIFY(!keyring.openVault(rotated, blobOf(fixture, rotating), &version, &error));
    QVERIFY(!error.isEmpty());

    // What a database synced with this vault uses
    const auto params = manager->vaultRemoteParams(rotated);
    QCOMPARE(params.type, RemoteParams::Type::AccountVault);
    QCOMPARE(params.vaultId, rotated);
    QCOMPARE(params.accountId, manager->accountId());
    QCOMPARE(params.serverUrl, fixture.serverConfig().sync);
    QVERIFY(params.name.contains("Infra"));

    // A new vault: sealed to this account, and it opens with the key the keyring gets back
    QString createdId;
    QSharedPointer<Database> created;
    manager->createVault("Personal",
                         [&](const QString& id, QSharedPointer<Database> newDb, const QString& createError) {
                             QVERIFY2(createError.isEmpty(), qPrintable(createError));
                             createdId = id;
                             created = newDb;
                         });
    QTRY_VERIFY(created);
    QCOMPARE(created->rootGroup()->name(), QString("Personal"));
    QTRY_VERIFY(manager->keyring().contains(createdId));
    QCOMPARE(manager->keyring().vault(createdId).name, QString("Personal"));
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    KeePass2Writer writer;
    QVERIFY(writer.writeDatabase(&buffer, created.data()));
    QVERIFY(manager->keyring().openVault(createdId, buffer.data(), &version));
    QCOMPARE(version, 1);
}

void TestAccountManager::testAccessEventRefreshesAndReloads()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this);
    QVERIFY(!secretKey.isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QTRY_VERIFY(fixture.count("GET /vaults") >= 1);

    // A vault appears (made on another device): the event comes before the new token and vault list
    auto alice = fixture.device(Email, Password, secretKey);
    const auto vaultId = createVault(*alice, "Shared");
    fixture.servers->clear_requests();
    const auto before = manager->accessTokenNow();

    SyncEventStream::Event event;
    event.type = SyncEventStream::Event::Type::Access;
    event.vault = vaultId;
    event.change = "granted";
    manager->handleAccessEvent(event);
    QTRY_VERIFY(manager->keyring().contains(vaultId));
    QVERIFY(manager->accessTokenNow() != before);
    QCOMPARE(fixture.count("POST /auth/refresh"), 1);
    QCOMPARE(fixture.count("GET /vaults"), 1);
    // The refresh came first, so the vault list was read with the new token
    const auto requests = fixture.requests();
    int refreshAt = -1;
    int vaultsAt = -1;
    for (int i = 0; i < requests.size(); ++i) {
        if (requests.at(i).endsWith(" POST /auth/refresh")) {
            refreshAt = i;
        } else if (requests.at(i).endsWith(" GET /vaults")) {
            vaultsAt = i;
        }
    }
    QVERIFY(refreshAt >= 0 && refreshAt < vaultsAt);
}

void TestAccountManager::testDeleteAccountAndRestore()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));

    // The password is checked on this device first, and the typed email by the server
    QString error;
    QString when;
    bool done = false;
    manager->deleteAccount("wrong", Email, [&](const QString& at, const QString& e) {
        when = at;
        error = e;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
    QVERIFY(!error.isEmpty());
    QCOMPARE(manager->state(), AccountManager::State::Unlocked);
    QVERIFY(manager->knownAccounts().first().deletionScheduledFor.isEmpty());
    done = false;
    manager->deleteAccount(Password, "bob@example.com", [&](const QString& at, const QString& e) {
        when = at;
        error = e;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
    QVERIFY2(error.contains("email"), qPrintable(error));

    QSignalSpy signedOut(manager, &AccountManager::signedOut);
    done = false;
    manager->deleteAccount(Password, Email.toUpper(), [&](const QString& at, const QString& e) {
        when = at;
        error = e;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(QDateTime::fromString(when, Qt::ISODate).isValid());
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QCOMPARE(signedOut.count(), 1);
    // The account stays known with its date, and the Secret Key stays for a restore
    QCOMPARE(manager->knownAccounts().first().deletionScheduledFor, when);
    QVERIFY(!AccountManager::storedSecretKey(ServerId, Email).isEmpty());

    // Signing in again: a pending account opens nothing
    QSignalSpy pending(manager, &AccountManager::pendingDeletionChanged);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QTRY_COMPARE(manager->deletionScheduledFor(), when);
    QVERIFY(pending.count() >= 1);
    QVERIFY(manager->keyring().isEmpty());

    done = false;
    manager->restoreAccount(Password, [&](const QString& e) {
        error = e;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(manager->deletionScheduledFor().isEmpty());
    QVERIFY(manager->knownAccounts().first().deletionScheduledFor.isEmpty());
}

void TestAccountManager::testDeletionEventsSignOut()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QVERIFY(!AccountManager::storedSecretKey(ServerId, Email).isEmpty());

    SyncEventStream::Event event;
    event.type = SyncEventStream::Event::Type::Access;
    event.change = "account_deleted";
    manager->handleAccessEvent(event);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    // Erased: the account and its Secret Key leave this device (the core's sign-out runs on its worker)
    QTRY_VERIFY(AccountManager::storedSecretKey(ServerId, Email).isEmpty());
    QVERIFY(manager->knownAccounts().isEmpty());
}

void TestAccountManager::testLockClearsKeys()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this);
    QVERIFY(!secretKey.isEmpty());
    auto alice = fixture.device(Email, Password, secretKey);
    const auto vaultId = createVault(*alice, "Mine");
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QTRY_VERIFY(manager->keyring().contains(vaultId));
    const auto encSk = manager->keyset().encSk;

    manager->lock();
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    QVERIFY(manager->keyset().isNull());
    QVERIFY(manager->keyring().isEmpty());
    // The session stays: vault sync can still get tokens
    QVERIFY(SyncServerClient::hasCredentials(manager->vaultRemoteParams(vaultId)));

    // Unlocking needs the password, not the Secret Key
    QSignalSpy errors(manager, &AccountManager::error);
    manager->unlock("wrong");
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    manager->unlock(Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QVERIFY(manager->keyset().encSk == encSk);
    QTRY_VERIFY(manager->keyring().contains(vaultId));
}

void TestAccountManager::testSwitchingKeepsTheOtherAccountSignedIn()
{
    Fixture fixture;
    QScopedPointer<QObject> parent(new QObject());
    auto* manager = fixture.manager(parent.data());
    QSignalSpy vaultsChanged(manager, &AccountManager::vaultsChanged);
    manager->registerAccount(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    const auto first = manager->accountId();
    const auto firstRefresh = refreshTokenOf(first);
    QVERIFY(!firstRefresh.isEmpty());
    QTRY_VERIFY(AccountOfflineCache::hasProfile(first));
    QTRY_VERIFY(vaultsChanged.count() > 0);

    // Setting it aside leaves the session and the cache, tells the server nothing, and empties the manager
    fixture.servers->clear_requests();
    bool set = false;
    manager->setAside([&] { set = true; });
    QVERIFY(set);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY(manager->accountId().isEmpty());
    QCOMPARE(refreshTokenOf(first), firstRefresh);
    QVERIFY(AccountOfflineCache::hasProfile(first));
    QVERIFY(fixture.requests().isEmpty());

    const QString second("second@example.com");
    manager->registerAccount(second, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    const auto secondId = manager->accountId();
    QVERIFY(secondId != first);
    QTRY_VERIFY(AccountOfflineCache::hasProfile(secondId));

    auto known = manager->knownAccounts();
    QCOMPARE(known.size(), 2);
    QCOMPARE(known.first().id, secondId);
    QVERIFY(known.first().current);
    QVERIFY(known.last().signedIn);
    QVERIFY(!known.last().current);
    QCOMPARE(known.last().email.toLower(), Email.toLower());

    // Back to the first: locked, with neither a sign-in nor a two-step code, and it's the one restored next launch
    fixture.servers->clear_requests();
    bool switched = false;
    manager->switchTo(first, [&](bool ok) { switched = ok; });
    QTRY_VERIFY(switched);
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    QCOMPARE(manager->email().toLower(), Email.toLower());
    QCOMPARE(AccountManager::lastAccountId(), first);
    for (const auto& request : fixture.requests()) {
        QVERIFY(!request.contains("login"));
    }
    manager->unlock(Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));

    // A session that has ended can't be switched to, and the current account's stays
    manager->signOutKnown(secondId);
    QTRY_VERIFY(coreSession(secondId).isEmpty());
    QVERIFY(!AccountOfflineCache::hasProfile(secondId));
    QVERIFY(AccountOfflineCache::hasProfile(first));
    QVERIFY(!AccountManager::storedSecretKey(ServerId, second).isEmpty());
    bool ok = true;
    bool answered = false;
    manager->switchTo(secondId, [&](bool result) {
        ok = result;
        answered = true;
    });
    QTRY_VERIFY(answered);
    QVERIFY(!ok);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QCOMPARE(manager->knownAccounts().size(), 2);

    // Removing it forgets the Secret Key too
    manager->forgetKnown(secondId);
    QTRY_VERIFY(AccountManager::storedSecretKey(ServerId, second).isEmpty());
    QCOMPARE(manager->knownAccounts().size(), 1);
    QVERIFY(!AccountManager::storedSecretKey(ServerId, Email).isEmpty());
}

void TestAccountManager::testChecksSessionsSetAside()
{
    Fixture fixture;
    QScopedPointer<QObject> parent(new QObject());
    auto* manager = fixture.manager(parent.data());
    manager->registerAccount(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    const auto first = manager->accountId();
    QTRY_VERIFY(AccountOfflineCache::hasProfile(first));
    bool set = false;
    manager->setAside([&] { set = true; });
    QTRY_VERIFY(set);
    manager->registerAccount("second@example.com", Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    const auto second = manager->accountId();

    const auto before = refreshTokenOf(first);
    QVERIFY(!before.isEmpty());
    manager->checkSetAsideAccounts();
    QTRY_VERIFY(refreshTokenOf(first) != before);
    // Once a day at most
    const auto rotated = refreshTokenOf(first);
    QTRY_VERIFY(manager->knownAccounts().last().checkedAt > 0);
    manager->checkSetAsideAccounts();
    QTest::qWait(50);
    QCOMPARE(refreshTokenOf(first), rotated);
    QCOMPARE(manager->accountId(), second);

    // A refusal ends that session alone: the first account was signed out everywhere
    fixture.servers->revoke_sessions(rs("user:" + first));
    auto known = manager->knownAccounts();
    QCOMPARE(known.last().id, first);
    // Past the day's limit, as a day later
    auto raw = QJsonDocument::fromJson(SyncTokenStore::load("ov-account-known").toUtf8()).array();
    for (auto value : raw) {
        auto entry = value.toObject();
        entry["checked_at"] = 1;
        value = entry;
    }
    SyncTokenStore::save("ov-account-known", QJsonDocument(raw).toJson(QJsonDocument::Compact));
    manager->checkSetAsideAccounts();
    QTRY_VERIFY(coreSession(first).isEmpty());
    QTRY_VERIFY(!AccountOfflineCache::hasProfile(first));
    QCOMPARE(manager->accountId(), second);
    QCOMPARE(manager->state(), AccountManager::State::Unlocked);
    QVERIFY(!refreshTokenOf(second).isEmpty());
    QTRY_VERIFY(AccountOfflineCache::hasProfile(second));
}

void TestAccountManager::testRestoreSessionAfterRestart()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    QString accountId;
    {
        auto* manager = fixture.manager(nullptr);
        manager->signIn(Email, Password);
        QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
        accountId = manager->accountId();
        // Quitting the app keeps the session
        delete manager;
    }
    QCOMPARE(AccountManager::lastAccountId(), accountId);

    fixture.servers->clear_requests();
    auto* manager = fixture.manager(this);
    QVERIFY(manager->restoreSession(accountId));
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    QCOMPARE(manager->email().toLower(), Email.toLower());
    QCOMPARE(manager->server().sync, fixture.serverConfig().sync);
    manager->unlock(Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    // The access token came from the stored refresh token
    QCOMPARE(fixture.count("POST /auth/refresh"), 1);
    QVERIFY(!manager->restoreSession("unknown-account"));
}

void TestAccountManager::testAccountVaultUsesTokenProvider()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    const auto vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto sync = fixture.serverConfig().sync;

    RemoteParams params;
    params.type = RemoteParams::Type::AccountVault;
    params.serverUrl = sync;
    params.vaultId = vaultId;
    QVERIFY(params.isServer());
    QVERIFY(params.hasDownload() && params.hasUpload());
    QCOMPARE(params.remoteDatabaseId(), vaultId);

    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    params.accountId = manager->accountId();
    QVERIFY(SyncServerClient::hasCredentials(params));

    // A sync server client for the vault: vaults/<id> with the account's current token
    class RecordingClient : public SyncServerClient
    {
    public:
        using SyncServerClient::SyncServerClient;
        QStringList urls;
        QStringList authorizations;

    protected:
        void send(const QByteArray& verb,
                  const QUrl& url,
                  const Headers& headers,
                  const QByteArray&,
                  int,
                  Callback callback) override
        {
            urls << QString::fromLatin1(verb) + " " + url.toString();
            for (const auto& header : headers) {
                if (header.first == "Authorization") {
                    authorizations << QString::fromLatin1(header.second);
                }
            }
            Response response;
            response.status = 404;
            deliverLater(response, std::move(callback));
        }
    };
    RecordingClient client(params);
    bool done = false;
    client.download([&done](const SyncServerClient::Response&) { done = true; });
    QTRY_VERIFY(done);
    client.listDatabases([](const SyncServerClient::Response&) {});
    QTRY_COMPARE(client.urls.size(), 2);
    QCOMPARE(client.urls.first(), "GET " + sync + "/vaults/" + vaultId);
    QCOMPARE(client.urls.last(), "GET " + sync + "/vaults");
    QCOMPARE(client.authorizations.first(), "Bearer " + manager->accessTokenNow());

    // A refreshed token is used by the same client from then on
    QSignalSpy changed(manager, &AccountManager::accessTokenChanged);
    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 1);
    client.download([](const SyncServerClient::Response&) {});
    QTRY_COMPARE(client.authorizations.size(), 3);
    QCOMPARE(client.authorizations.last(), "Bearer " + manager->accessTokenNow());

    // Signed out: nothing to send with
    manager->signOut();
    QVERIFY(!SyncServerClient::hasCredentials(params));
    client.download([](const SyncServerClient::Response&) {});
    QTRY_COMPARE(client.urls.size(), 4);
    QCOMPARE(client.authorizations.size(), 3);

    // Legacy remotes still send their static device token to databases/<id>
    RemoteParams legacy;
    legacy.type = RemoteParams::Type::SyncServer;
    legacy.serverUrl = "https://sync.example.com";
    legacy.serverDatabaseId = "personal";
    legacy.serverToken = "device-token";
    RecordingClient legacyClient(legacy);
    legacyClient.download([](const SyncServerClient::Response&) {});
    QTRY_COMPARE(legacyClient.urls.size(), 1);
    QCOMPARE(legacyClient.urls.first(), QString("GET https://sync.example.com/api/v1/databases/personal"));
    QCOMPARE(legacyClient.authorizations, QStringList({"Bearer device-token"}));
}

void TestAccountManager::testParsesAccountEvents()
{
    QByteArray buffer =
        "event: vault\n"
        "data: {\"id\":\"3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b\",\"etag\":\"\\\"abc\\\"\",\"revision\":4}\n\n"
        "event: vault\n"
        "data: {\"id\":\"6c7d8e9f-0a1b-4c2d-8e3f-4a5b6c7d8e9f\",\"deleted\":true}\n\n"
        ": keep-alive\n\n"
        "event: access\n"
        "data: {\"vault\":\"3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b\",\"change\":\"rotating\"}\n\n"
        "event: access\n"
        "data: {\"vault\":null,\"change\":\"org\"}\n\n"
        "event: database\n"
        "data: {\"id\":\"personal\",\"etag\":\"\\\"def\\\"\",\"revision\":2}\n\n"
        "event: vault\n"
        "data: {\"id\":\"partial";
    const auto events = SyncEventStream::takeEvents(buffer);
    QCOMPARE(events.size(), 5);
    QCOMPARE(events.at(0).type, SyncEventStream::Event::Type::Database);
    QCOMPARE(events.at(0).databaseId, QString("3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b"));
    QCOMPARE(events.at(0).etag, QString("\"abc\""));
    QCOMPARE(events.at(0).revision, 4);
    QVERIFY(events.at(1).deleted);
    QCOMPARE(events.at(2).type, SyncEventStream::Event::Type::Access);
    QCOMPARE(events.at(2).vault, QString("3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b"));
    QCOMPARE(events.at(2).change, QString("rotating"));
    QCOMPARE(events.at(3).type, SyncEventStream::Event::Type::Access);
    QVERIFY(events.at(3).vault.isEmpty());
    QCOMPARE(events.at(3).change, QString("org"));
    // Legacy device-token events are unchanged
    QCOMPARE(events.at(4).type, SyncEventStream::Event::Type::Database);
    QCOMPARE(events.at(4).databaseId, QString("personal"));
    // The partial event waits for the rest
    QVERIFY(buffer.startsWith("event: vault"));
}

void TestAccountManager::testServerConfigWebUrl()
{
    auto json = QJsonObject{{"issuer", "http://localhost:8000"},
                            {"api", "http://localhost:8000/api/v1"},
                            {"sync", "http://localhost:8787/api/v1"},
                            {"web", "http://localhost:3000"}};
    auto config = AccountManager::ServerConfig::fromJson(json);
    QCOMPARE(config.web, QString("http://localhost:3000"));
    QCOMPARE(config.accountSettingsUrl(), QUrl("http://localhost:3000/?view=account"));
    // Kept with the known account
    QCOMPARE(AccountManager::ServerConfig::fromJson(config.toJson()).web, config.web);

    json.remove("web");
    QVERIFY(!AccountManager::ServerConfig::fromJson(json).accountSettingsUrl().isValid());
    json["web"] = "javascript:alert(1)";
    QVERIFY(!AccountManager::ServerConfig::fromJson(json).accountSettingsUrl().isValid());
    json["web"] = "http://app.example.com";
    QVERIFY(!AccountManager::ServerConfig::fromJson(json).accountSettingsUrl().isValid());

    // Plain http:// only for this computer (SPEC §1): an https issuer can't hand out an http sync URL
    json.remove("web");
    QVERIFY(AccountManager::ServerConfig::fromJson(json).insecureUrl().isEmpty());
    json = QJsonObject{{"issuer", "https://accounts.example.com"},
                       {"api", "https://accounts.example.com/api/v1"},
                       {"sync", "http://sync.example.com/api/v1"}};
    QCOMPARE(AccountManager::ServerConfig::fromJson(json).insecureUrl(), QString("http://sync.example.com/api/v1"));
    json["sync"] = "https://sync.example.com/api/v1";
    QVERIFY(AccountManager::ServerConfig::fromJson(json).insecureUrl().isEmpty());
}

void TestAccountManager::testFollowsAnEmailChangedElsewhere()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this);
    QVERIFY(!secretKey.isEmpty());
    QString accountId;
    {
        auto* manager = fixture.manager(nullptr);
        manager->signIn(Email, Password);
        QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
        accountId = manager->accountId();
        delete manager;
    }
    QCOMPARE(AccountManager::storedSecretKey(ServerId, Email), secretKey);

    // The email changes on another device (the web app, say)
    const QString newEmail("alice.new@example.com");
    auto other = fixture.device(Email, Password, secretKey);
    auto cancel = ov::new_cancel_token();
    ov::call([&] { return other->request_email_change(rs(newEmail), *cancel); });
    const auto code = fixture.servers->email_code(rs(accountId));
    ov::call([&] { return other->change_email(rs(newEmail), code, bytesOf(Password), *cancel); });

    auto* manager = fixture.manager(this);
    QSignalSpy changed(manager, &AccountManager::emailChanged);
    QVERIFY(manager->restoreSession(accountId));
    manager->unlock(Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(manager->email(), newEmail);
    QCOMPARE(AccountManager::storedSecretKey(ServerId, newEmail), secretKey);
    QVERIFY(AccountManager::storedSecretKey(ServerId, Email).isEmpty());

    // The stored session has the new email too
    auto* restored = fixture.manager(this);
    QVERIFY(restored->restoreSession(accountId));
    QCOMPARE(restored->email(), newEmail);
}

void TestAccountManager::testExplainsAPlanLimit()
{
    AuthClient::Response response;
    response.status = 402;
    response.body = QJsonDocument(QJsonObject{{"error", "The free plan allows 1 personal vaults"},
                                              {"code", "plan_limit"},
                                              {"upgrade_url", "https://accounts.example/subscription/pricing"}})
                        .toJson(QJsonDocument::Compact);
    QCOMPARE(AuthClient::errorMessage(response),
             QString("The free plan allows 1 personal vaults. Change your plan at "
                     "https://accounts.example/subscription/pricing"));

    // Any other 402 is shown as it is
    response.body = R"({"error":"nope","code":"invalid_request"})";
    QCOMPARE(AuthClient::errorMessage(response), QString("The account server responded with HTTP 402: nope"));
}

namespace
{
    // Alice signed in with one vault, made on her other device, as listed by GET /vaults
    struct OfflineSetup
    {
        AccountManager* manager = nullptr;
        QString vaultId;
        std::optional<rust::Box<ov::Account>> other;
    };

    OfflineSetup signInWithVault(Fixture& fixture, QObject* parent)
    {
        OfflineSetup setup;
        const auto secretKey = registerAlice(fixture, parent);
        if (secretKey.isEmpty()) {
            return setup;
        }
        setup.other = fixture.device(Email, Password, secretKey);
        setup.vaultId = createVault(**setup.other, "Mine");
        auto* manager = fixture.manager(parent);
        manager->signIn(Email, Password);
        if (!waitForState(manager, AccountManager::State::Unlocked)) {
            return setup;
        }
        QElapsedTimer timer;
        timer.start();
        while ((!manager->keyring().contains(setup.vaultId) || !AccountOfflineCache::hasProfile(manager->accountId()))
               && timer.elapsed() < 5000) {
            QTest::qWait(10);
        }
        setup.manager = manager;
        return setup;
    }
} // namespace

void TestAccountManager::testOfflineUnlockFromCache()
{
    Fixture fixture;
    auto setup = signInWithVault(fixture, this);
    auto* manager = setup.manager;
    QVERIFY(manager);
    QVERIFY(manager->keyring().contains(setup.vaultId));

    // The cache holds what the server sent, nothing in clear
    QFile file(AccountOfflineCache::path(manager->accountId()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto cached = file.readAll();
    file.close();
    QVERIFY(cached.contains("\"keyset\""));
    QVERIFY(cached.contains("\"kdf\""));
    QVERIFY(!cached.contains("Mine"));
    QVERIFY(!cached.contains(toBase64Url(manager->keyset().encSk).toUtf8()));
    QVERIFY(!cached.contains(AccountManager::storedSecretKey(ServerId, Email).toUtf8()));
    // The vault list (encrypted names, sealed keys) is in the core's cache beside it
    QByteArray coreCache;
    QDirIterator files(AccountOfflineCache::accountsRoot() + "/" + manager->accountId() + "/core",
                       QDir::Files,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile part(files.next());
        QVERIFY(part.open(QIODevice::ReadOnly));
        coreCache += part.readAll();
    }
    QVERIFY(coreCache.contains(setup.vaultId.toUtf8()));
    QVERIFY(!coreCache.contains("Mine"));
    QVERIFY(!coreCache.contains(toBase64Url(manager->keyset().encSk).toUtf8()));
    QVERIFY(!coreCache.contains(AccountManager::storedSecretKey(ServerId, Email).toUtf8()));
    QVERIFY(!(file.permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup)));
    QVERIFY(manager->hasOfflineCache());
    QVERIFY(!manager->isOffline());

    manager->lock();
    fixture.servers->set_offline(true);
    QSignalSpy offlineChanged(manager, &AccountManager::offlineChanged);
    QSignalSpy errors(manager, &AccountManager::error);

    // A wrong password fails as it does online
    manager->unlock("wrong");
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    QVERIFY(!manager->isOffline());

    manager->unlock(Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QVERIFY(manager->isOffline());
    QCOMPARE(offlineChanged.count(), 1);
    QVERIFY(manager->keyring().contains(setup.vaultId));
    QCOMPARE(manager->keyring().vault(setup.vaultId).name, QString("Mine"));
    QCOMPARE(errors.count(), 1);

    // Back online once GET /vaults answers
    fixture.servers->set_offline(false);
    manager->reconnect();
    QTRY_VERIFY(!manager->isOffline());
    QCOMPARE(offlineChanged.count(), 2);
    QVERIFY(manager->keyring().contains(setup.vaultId));
}

void TestAccountManager::testOfflineUnlockNeedsACache()
{
    Fixture fixture;
    auto setup = signInWithVault(fixture, this);
    auto* manager = setup.manager;
    QVERIFY(manager);
    manager->lock();
    AccountOfflineCache::clear(manager->accountId());
    QVERIFY(!manager->hasOfflineCache());

    fixture.servers->set_offline(true);
    QSignalSpy errors(manager, &AccountManager::error);
    manager->unlock(Password);
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    QVERIFY(!manager->isOffline());
    QVERIFY(errors.first().first().toString().contains(ServerId));
}

void TestAccountManager::testRefusedUnlockNeverUsesTheCache()
{
    Fixture fixture;
    auto setup = signInWithVault(fixture, this);
    auto* manager = setup.manager;
    QVERIFY(manager);
    manager->lock();
    QVERIFY(manager->hasOfflineCache());

    // The server answers, and says no: that's not offline
    fixture.servers->fail_next("auth", "GET", "/me", 403, 10);
    QSignalSpy errors(manager, &AccountManager::error);
    manager->unlock(Password);
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    QVERIFY(!manager->isOffline());
}

void TestAccountManager::testGoesOfflineAndDropsRevokedVaultsOnReconnect()
{
    Fixture fixture;
    auto setup = signInWithVault(fixture, this);
    auto* manager = setup.manager;
    QVERIFY(manager);

    // The network goes away while unlocked
    fixture.servers->set_offline(true);
    QSignalSpy errors(manager, &AccountManager::error);
    manager->reloadVaults();
    QTRY_VERIFY(manager->isOffline());
    QCOMPARE(errors.count(), 0);
    QVERIFY(manager->keyring().contains(setup.vaultId));

    // The vault went meanwhile (deleted on the other device)
    fixture.servers->set_offline(false);
    auto cancel = ov::new_cancel_token();
    ov::call([&] { return (*setup.other)->delete_vault(rs(setup.vaultId), *cancel); });
    manager->reconnect();
    QTRY_VERIFY(!manager->isOffline());
    QTRY_VERIFY(!manager->keyring().contains(setup.vaultId));
    // The vault list the core keeps for offline unlock dropped it too
    QDirIterator files(AccountOfflineCache::accountsRoot() + "/" + manager->accountId() + "/core/meta", QDir::Files);
    while (files.hasNext()) {
        QFile part(files.next());
        QVERIFY(part.open(QIODevice::ReadOnly));
        QVERIFY2(!part.readAll().contains(setup.vaultId.toUtf8()), qPrintable(part.fileName()));
    }
}

void TestAccountManager::testSessionEndWipesTheOfflineCache()
{
    Fixture fixture;
    auto setup = signInWithVault(fixture, this);
    auto* manager = setup.manager;
    QVERIFY(manager);
    const auto accountId = manager->accountId();
    QVERIFY(QFile::exists(AccountOfflineCache::path(accountId)));

    // Locking keeps it
    manager->lock();
    QVERIFY(QFile::exists(AccountOfflineCache::path(accountId)));

    manager->signOut();
    QVERIFY(!QFile::exists(AccountOfflineCache::path(accountId)));
}

void TestAccountManager::testOfflineUnlockCanBeTurnedOff()
{
    config()->set(Config::Account_AllowOfflineUnlock, false);
    Fixture fixture;
    auto setup = signInWithVault(fixture, this);
    auto* manager = setup.manager;
    QVERIFY(manager);
    QVERIFY(!QFile::exists(AccountOfflineCache::path(manager->accountId())));
    QVERIFY(!manager->hasOfflineCache());

    manager->lock();
    fixture.servers->set_offline(true);
    QSignalSpy errors(manager, &AccountManager::error);
    manager->unlock(Password);
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
    QCOMPARE(manager->state(), AccountManager::State::Locked);

    // Turning it off removes what an earlier session kept
    config()->set(Config::Account_AllowOfflineUnlock, true);
    AccountOfflineCache::saveVaults(manager->accountId(), {});
    QVERIFY(QFile::exists(AccountOfflineCache::path(manager->accountId())));
    AccountOfflineCache::clearAll();
    QVERIFY(!QFile::exists(AccountOfflineCache::path(manager->accountId())));
}
