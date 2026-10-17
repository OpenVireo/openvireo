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

#include "TestAccountManager.h"

#include "config-keepassx.h"

#include "account/AccountManager.h"
#include "account/OpaqueClient.h"
#include "account/VaultKeyring.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "format/KeePass2Writer.h"
#include "gui/remote/SyncEventStream.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/remote/SyncTokenStore.h"

#ifdef KPXC_FEATURE_ACCOUNTS
#include "account/OpaqueCore.h"
#endif

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>

QTEST_GUILESS_MAIN(TestAccountManager)

using namespace AccountCrypto;

namespace
{
    const QString Issuer = QStringLiteral("https://accounts.keepassxc.example");
    const QString ServerId = QStringLiteral("accounts.keepassxc.example");
    const QString Email = QStringLiteral("Alice@Example.com");
    const QString Password = QStringLiteral("correct horse battery staple");
    const QString ContractDir = QStringLiteral(KEEPASSX_SOURCE_DIR "/docs/plans/accounts/");

    AccountManager::ServerConfig serverConfig()
    {
        AccountManager::ServerConfig config;
        config.issuer = Issuer;
        config.api = Issuer + "/api/v1";
        config.sync = "https://sync.keepassxc.example/api/v1";
        config.opaqueServerId = ServerId;
        return config;
    }

    // The cheapest settings clients accept, to keep the tests quick
    KdfParams testKdf()
    {
        auto kdf = KdfParams::generate();
        kdf.memoryKiB = KdfParams::MinMemoryKiB;
        kdf.iterations = KdfParams::MinIterations;
        return kdf;
    }

    QJsonObject loadVectors()
    {
        QFile file(ContractDir + "test-vectors.json");
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    }

    /*
     * Stands in for the OPAQUE server. With libopaque it runs the real server half, so sign-in goes
     * through the actual protocol; without it the client and server below just compare a hash.
     */
    struct OpaqueServer
    {
        struct Login
        {
            QByteArray authU0;
            QString email;
        };

        static QByteArray registrationResponse(const QByteArray& request, QByteArray* sec)
        {
#ifdef KPXC_FEATURE_ACCOUNTS
            QByteArray pub(OPAQUE_REGISTER_PUBLIC_LEN, '\0');
            sec->resize(OPAQUE_REGISTER_SECRET_LEN);
            if (opaque_CreateRegistrationResponse(reinterpret_cast<const uint8_t*>(request.constData()),
                                                  nullptr,
                                                  reinterpret_cast<uint8_t*>(sec->data()),
                                                  reinterpret_cast<uint8_t*>(pub.data()))
                != 0) {
                return {};
            }
            return pub;
#else
            *sec = request;
            return "pub";
#endif
        }

        static QByteArray storeRecord(const QByteArray& sec, const QByteArray& record)
        {
#ifdef KPXC_FEATURE_ACCOUNTS
            QByteArray rec(OPAQUE_USER_RECORD_LEN, '\0');
            opaque_StoreUserRecord(reinterpret_cast<const uint8_t*>(sec.constData()),
                                   reinterpret_cast<const uint8_t*>(record.constData()),
                                   reinterpret_cast<uint8_t*>(rec.data()));
            return rec;
#else
            Q_UNUSED(sec)
            return record;
#endif
        }

        static QByteArray
        credentialResponse(const QByteArray& ke1, const QByteArray& rec, const QString& email, QByteArray* authU0)
        {
#ifdef KPXC_FEATURE_ACCOUNTS
            QByteArray user = OpaqueClient::userId(email);
            QByteArray server = ServerId.toUtf8();
            Opaque_Ids ids{static_cast<uint16_t>(user.size()),
                           reinterpret_cast<uint8_t*>(user.data()),
                           static_cast<uint16_t>(server.size()),
                           reinterpret_cast<uint8_t*>(server.data())};
            QByteArray ke2(OPAQUE_SERVER_SESSION_LEN, '\0');
            QByteArray sk(OPAQUE_SHARED_SECRETBYTES, '\0');
            authU0->resize(crypto_auth_hmacsha512_BYTES);
            if (opaque_CreateCredentialResponse(reinterpret_cast<const uint8_t*>(ke1.constData()),
                                                reinterpret_cast<const uint8_t*>(rec.constData()),
                                                &ids,
                                                reinterpret_cast<const uint8_t*>(OpaqueClient::Context.constData()),
                                                static_cast<uint16_t>(OpaqueClient::Context.size()),
                                                reinterpret_cast<uint8_t*>(ke2.data()),
                                                reinterpret_cast<uint8_t*>(sk.data()),
                                                reinterpret_cast<uint8_t*>(authU0->data()))
                != 0) {
                return {};
            }
            return ke2;
#else
            Q_UNUSED(email)
            *authU0 = "auth";
            return ke1 == rec ? "ok" : "bad";
#endif
        }

        static bool userAuth(const QByteArray& authU0, const QByteArray& authU)
        {
#ifdef KPXC_FEATURE_ACCOUNTS
            return authU.size() == authU0.size()
                   && opaque_UserAuth(reinterpret_cast<const uint8_t*>(authU0.constData()),
                                      reinterpret_cast<const uint8_t*>(authU.constData()))
                          == 0;
#else
            return authU == authU0;
#endif
        }
    };

#ifndef KPXC_FEATURE_ACCOUNTS
    // Without libopaque: the record is the password's hash, and login compares hashes
    class FakeOpaqueClient : public OpaqueClient
    {
    public:
        bool createRegistrationRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* request) override
        {
            *sec = password;
            *request = "request";
            return true;
        }
        bool finalizeRegistration(const SecureBytes& sec, const QByteArray&, const Ids&, QByteArray* record) override
        {
            *record = sha256(fromSecure(sec));
            return true;
        }
        bool createCredentialRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* ke1) override
        {
            *sec = password;
            *ke1 = sha256(fromSecure(password));
            return true;
        }
        bool recoverCredentials(const QByteArray& ke2,
                                const SecureBytes&,
                                const Ids&,
                                QByteArray* authU,
                                SecureBytes*) override
        {
            *authU = "auth";
            return ke2 == "ok";
        }
    };
#endif

    std::unique_ptr<OpaqueClient> makeOpaque()
    {
#ifdef KPXC_FEATURE_ACCOUNTS
        return std::make_unique<OpaqueClient>();
#else
        return std::make_unique<FakeOpaqueClient>();
#endif
    }

    // What the fake auth server knows; every client made with it talks to the same server
    struct AuthServer
    {
        struct User
        {
            QString id;
            QString email;
            QByteArray record;
            QJsonObject kdf;
            QJsonObject keyset;
            bool mfa = false;
        };
        // One device session: its refresh tokens rotate, and showing a rotated one revokes it
        struct Family
        {
            QString userId;
            QString current;
            QSet<QString> rotated;
            bool revoked = false;
        };

        QHash<QString, User> users;
        QHash<QString, QPair<QString, QByteArray>> registrations;
        QHash<QString, OpaqueServer::Login> logins;
        QHash<QString, QString> mfaTokens;
        QHash<QString, Family> families;
        QHash<QString, QString> refreshFamily;
        QHash<QString, QString> accessTokens;
        int accessExpiresIn = 600;
        int counter = 0;
        QJsonArray vaults;
        QStringList requests;
        QStringList bearers;

        // The service account
        QString saId;
        QString saSecret;
        QString wrappedMaster;
        QJsonObject saKeyset;

        QString next(const QString& prefix)
        {
            return prefix + QString::number(++counter);
        }

        QJsonObject newSession(const User& user)
        {
            const auto family = next("family-");
            const auto refresh = next("kpx_rt_");
            families.insert(family, {user.id, refresh, {}, false});
            refreshFamily.insert(refresh, family);
            const auto access = next("at-");
            accessTokens.insert(access, "user:" + user.id);
            return {{"access_token", access},
                    {"refresh_token", refresh},
                    {"expires_in", accessExpiresIn},
                    {"device_id", "d4e5f6a7-b8c9-4d0e-9f1a-2b3c4d5e6f70"},
                    {"user", QJsonObject{{"id", user.id}, {"email", user.email}}},
                    {"kdf", user.kdf},
                    {"keyset", user.keyset}};
        }
    };

    class FakeAuthClient : public AuthClient
    {
    public:
        explicit FakeAuthClient(QSharedPointer<AuthServer> server)
            : AuthClient(Issuer + "/api/v1")
            , m_server(std::move(server))
        {
        }

    protected:
        void send(const QByteArray& verb,
                  const QUrl& url,
                  const Headers& headers,
                  const QByteArray& body,
                  int,
                  Callback callback) override
        {
            auto& server = *m_server;
            const auto path = url.path().mid(QString("/api/v1/").size());
            server.requests << QString::fromLatin1(verb) + " " + path;
            const auto json = QJsonDocument::fromJson(body).object();
            QString bearer;
            for (const auto& header : headers) {
                if (header.first == "Authorization") {
                    bearer = QString::fromLatin1(header.second).mid(QString("Bearer ").size());
                    server.bearers << bearer;
                }
            }

            Response response;
            const auto reply = [&](int status, const QJsonObject& object = {}) {
                response.status = status;
                response.body = QJsonDocument(object).toJson(QJsonDocument::Compact);
            };
            const auto error = [&](int status, const QString& code) {
                reply(status, {{"error", code}, {"code", code}});
            };
            const auto b64 = [&json](const QString& name) { return fromBase64Url(json.value(name).toString()); };

            if (path == "auth/register/start") {
                QByteArray sec;
                const auto pub = OpaqueServer::registrationResponse(b64("request"), &sec);
                const auto session = server.next("reg-");
                server.registrations.insert(session, {json.value("email").toString(), sec});
                reply(200,
                      {{"session_id", session},
                       {"response", toBase64Url(pub)},
                       {"user_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}});
            } else if (path == "auth/register/finish") {
                const auto registration = server.registrations.take(json.value("session_id").toString());
                AuthServer::User user;
                user.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                user.email = registration.first;
                user.record = OpaqueServer::storeRecord(registration.second, b64("record"));
                user.kdf = json.value("kdf").toObject();
                user.keyset = json.value("keyset").toObject();
                server.users.insert(user.email.toLower(), user);
                reply(201, server.newSession(user));
            } else if (path == "auth/login/start") {
                const auto email = json.value("email").toString().toLower();
                const auto user = server.users.value(email);
                OpaqueServer::Login login;
                login.email = email;
                auto ke2 = OpaqueServer::credentialResponse(b64("ke1"), user.record, email, &login.authU0);
                if (ke2.isEmpty()) {
                    // An unknown email still gets an answer, which fails on the client
                    ke2 = QByteArray(400, 'x');
                }
                const auto session = server.next("login-");
                server.logins.insert(session, login);
                reply(200, {{"session_id", session}, {"ke2", toBase64Url(ke2)}});
            } else if (path == "auth/login/finish") {
                const auto login = server.logins.take(json.value("session_id").toString());
                if (!server.users.contains(login.email) || !OpaqueServer::userAuth(login.authU0, b64("auth_u"))) {
                    error(401, "unauthorized");
                } else if (server.users.value(login.email).mfa) {
                    const auto token = server.next("mfa-");
                    server.mfaTokens.insert(token, login.email);
                    reply(200, {{"mfa_required", true}, {"mfa_token", token}, {"methods", QJsonArray{"totp"}}});
                } else {
                    reply(200, server.newSession(server.users.value(login.email)));
                }
            } else if (path == "auth/login/mfa") {
                const auto token = json.value("mfa_token").toString();
                if (!server.mfaTokens.contains(token) || json.value("totp").toString() != "123456") {
                    error(401, "invalid_mfa");
                } else {
                    reply(200, server.newSession(server.users.value(server.mfaTokens.take(token))));
                }
            } else if (path == "auth/refresh") {
                const auto token = json.value("refresh_token").toString();
                const auto familyId = server.refreshFamily.value(token);
                auto& family = server.families[familyId];
                if (familyId.isEmpty() || family.revoked) {
                    error(401, "unauthorized");
                } else if (family.rotated.contains(token)) {
                    // Reuse of a rotated token: revoke the whole device session
                    family.revoked = true;
                    error(401, "token_reused");
                } else {
                    family.rotated.insert(token);
                    family.current = server.next("kpx_rt_");
                    server.refreshFamily.insert(family.current, familyId);
                    const auto access = server.next("at-");
                    server.accessTokens.insert(access, "user:" + family.userId);
                    reply(200,
                          {{"access_token", access},
                           {"refresh_token", family.current},
                           {"expires_in", server.accessExpiresIn}});
                }
            } else if (path == "auth/service-account") {
                if (json.value("sa_id").toString() != server.saId
                    || json.value("secret").toString() != server.saSecret) {
                    error(401, "unauthorized");
                } else {
                    const auto access = server.next("at-");
                    server.accessTokens.insert(access, "sa:" + server.saId);
                    reply(200,
                          {{"access_token", access},
                           {"expires_in", server.accessExpiresIn},
                           {"wrapped_master", server.wrappedMaster},
                           {"keyset", server.saKeyset}});
                }
            } else if (!server.accessTokens.contains(bearer)) {
                error(401, "unauthorized");
            } else if (path == "auth/logout") {
                reply(204);
            } else if (path == "me") {
                const auto principal = server.accessTokens.value(bearer);
                for (const auto& user : server.users) {
                    if ("user:" + user.id == principal) {
                        reply(200,
                              {{"id", user.id},
                               {"email", user.email},
                               {"kdf", user.kdf},
                               {"keyset", user.keyset},
                               {"mfa", QJsonObject{{"totp", user.mfa}}}});
                    }
                }
            } else if (path == "vaults" && verb == "GET") {
                reply(200, {{"vaults", server.vaults}});
            } else if (path == "vaults" && verb == "POST") {
                auto vault = json;
                QJsonArray keys;
                for (const auto& grant : json.value("grants").toArray()) {
                    keys << QJsonObject{{"key_version", grant.toObject().value("key_version")},
                                        {"sealed_key", grant.toObject().value("sealed_key")}};
                }
                vault["keys"] = keys;
                vault["role"] = "manage";
                vault["current_key_version"] = 1;
                server.vaults << vault;
                reply(201, vault);
            } else {
                error(404, "not_found");
            }
            deliverLater(response, std::move(callback));
        }

    private:
        QSharedPointer<AuthServer> m_server;
    };

    struct Fixture
    {
        QSharedPointer<AuthServer> server = QSharedPointer<AuthServer>::create();

        AccountManager* manager(QObject* parent)
        {
            auto* manager = new AccountManager(new FakeAuthClient(server), makeOpaque(), parent);
            manager->setServer(serverConfig());
            manager->setNewAccountKdf(testKdf());
            manager->setEventStreamEnabled(false);
            return manager;
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

    // Register Alice and sign her out again, leaving her Secret Key on this device
    QString registerAlice(Fixture& fixture, QObject* parent, bool mfa = false)
    {
        auto* manager = fixture.manager(parent);
        QSignalSpy registered(manager, &AccountManager::registered);
        manager->registerAccount(Email, Password);
        if (!waitForState(manager, AccountManager::State::Unlocked) || registered.count() != 1) {
            return {};
        }
        manager->signOut();
        fixture.server->users[Email.toLower()].mfa = mfa;
        delete manager;
        return registered.first().first().toString();
    }

    QByteArray vaultBlob(const SecureBytes& vaultKey, const QString& name, const QString& entryTitle)
    {
        auto db = VaultKeyring::createVaultDatabase(name, vaultKey);
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(entryTitle);
        entry->setGroup(db->rootGroup());
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        KeePass2Writer writer;
        writer.writeDatabase(&buffer, db.data());
        return buffer.data();
    }

    QJsonObject vaultJson(const QString& id,
                          const SecureBytes& nameKey,
                          const QString& name,
                          const QList<QPair<int, QByteArray>>& keys,
                          int current,
                          int pending = -1)
    {
        QJsonArray sealedKeys;
        for (const auto& key : keys) {
            sealedKeys << QJsonObject{{"key_version", key.first}, {"sealed_key", toBase64Url(key.second)}};
        }
        return {{"id", id},
                {"org_id", QJsonValue::Null},
                {"owner_id", QJsonValue::Null},
                {"encrypted_name", toBase64Url(encryptVaultName(nameKey, id, name))},
                {"role", "manage"},
                {"current_key_version", current},
                {"pending_key_version", pending < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(pending)},
                {"rotation_required", false},
                {"keys", sealedKeys}};
    }
} // namespace

void TestAccountManager::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestAccountManager::init()
{
    // Never touch the real Keychain
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
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

    // The server got a keyset it can't open: the private keys are wrapped with the AUK
    const auto& user = fixture.server->users.value(Email.toLower());
    QCOMPARE(user.keyset.value("enc_pk").toString(), toBase64Url(manager->keyset().publicKeys.encPk));
    QVERIFY(!user.keyset.value("wrapped").toString().isEmpty());
    QVERIFY(verifyPublicKeys(manager->keyset().publicKeys));
    QCOMPARE(manager->principal(), "user:" + manager->accountId());
    QVERIFY(!manager->accessTokenNow().isEmpty());
    // The password never went to the server
    QVERIFY(!fixture.server->requests.isEmpty());
    QTRY_VERIFY(vaultsChanged.count() > 0);

    const auto encPk = manager->keyset().publicKeys.encPk;
    manager->signOut();
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY(manager->keyset().isNull());
    QVERIFY(manager->accessTokenNow().isEmpty());
    QVERIFY(fixture.server->requests.contains("POST auth/logout"));

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

void TestAccountManager::testWrongPasswordOrSecretKey()
{
    Fixture fixture;
    const auto secretKey = registerAlice(fixture, this);
    QVERIFY(!secretKey.isEmpty());

    auto* manager = fixture.manager(this);
    QSignalSpy errors(manager, &AccountManager::error);
    QSignalSpy signedOut(manager, &AccountManager::signedOut);
    manager->signIn(Email, "wrong password");
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 20000);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY(errors.first().first().toString().contains("password is wrong"));
    // OPAQUE fails on this device; login/finish is never even sent
    QVERIFY(!fixture.server->requests.contains("POST auth/login/finish"));

    // Another Secret Key: the server accepts the password, but the keyset doesn't unwrap
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    SecureBytes other = generateSecretKey();
    fixture.server->requests.clear();
    manager->signIn(Email, Password, formatSecretKey(other));
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 20000);
    QCOMPARE(manager->state(), AccountManager::State::SignedOut);
    QVERIFY(errors.last().first().toString().contains("Secret Key"));
    QVERIFY(manager->keyset().isNull());
    // The session it opened is closed again, and the wrong key isn't remembered
    QVERIFY(fixture.server->requests.contains("POST auth/logout"));
    QVERIFY(AccountManager::storedSecretKey(ServerId, Email).isEmpty());
    QCOMPARE(signedOut.count(), 0);

    // A malformed Secret Key doesn't reach the server
    fixture.server->requests.clear();
    manager->signIn(Email, Password, "K1-not-a-key");
    QCOMPARE(errors.count(), 3);
    QVERIFY(fixture.server->requests.isEmpty());
}

void TestAccountManager::testRefreshRotatesTokens()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));

    const auto session = [&] {
        return QJsonDocument::fromJson(SyncTokenStore::load("kpx-account-session:" + manager->accountId()).toUtf8())
            .object();
    };
    const auto firstAccess = manager->accessTokenNow();
    const auto firstRefresh = session().value("refresh_token").toString();
    QVERIFY(firstRefresh.startsWith("kpx_rt_"));

    QSignalSpy changed(manager, &AccountManager::accessTokenChanged);
    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 1);
    QVERIFY(manager->accessTokenNow() != firstAccess);
    // The keychain holds the new refresh token, the old one is spent
    const auto secondRefresh = session().value("refresh_token").toString();
    QVERIFY(secondRefresh != firstRefresh);
    QVERIFY(fixture.server->families.value(fixture.server->refreshFamily.value(firstRefresh))
                .rotated.contains(firstRefresh));

    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 2);
    QVERIFY(session().value("refresh_token").toString() != secondRefresh);
    QCOMPARE(manager->state(), AccountManager::State::Unlocked);
}

void TestAccountManager::testRefreshesExpiringTokens()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QTRY_COMPARE(fixture.server->requests.count("GET vaults"), 2);

    // A token that is about to expire as soon as it is issued
    fixture.server->accessExpiresIn = AccountManager::MinTokenLifetimeSec - 5;
    QSignalSpy changed(manager, &AccountManager::accessTokenChanged);
    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 1);
    fixture.server->requests.clear();
    const auto expiring = manager->accessTokenNow();

    // Asking for a token refreshes first, and everyone waiting gets the new one
    fixture.server->accessExpiresIn = 600;
    QStringList tokens;
    manager->accessToken([&tokens](const QString& token) { tokens << token; });
    manager->accessToken([&tokens](const QString& token) { tokens << token; });
    QTRY_COMPARE(tokens.size(), 2);
    QCOMPARE(fixture.server->requests.count("POST auth/refresh"), 1);
    QVERIFY(tokens.first() != expiring);
    QCOMPARE(tokens.first(), tokens.last());
    QCOMPARE(tokens.first(), manager->accessTokenNow());

    // A fresh token is handed out as it is
    manager->accessToken([&tokens](const QString& token) { tokens << token; });
    QCOMPARE(tokens.size(), 3);
    QCOMPARE(fixture.server->requests.count("POST auth/refresh"), 1);

    // Before it expires, the timer refreshes on its own
    fixture.server->accessExpiresIn = AccountManager::RefreshMarginSec + 1;
    manager->refreshAccessToken();
    QTRY_COMPARE(fixture.server->requests.count("POST auth/refresh"), 2);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.server->requests.count("POST auth/refresh"), 3, 5000);
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
    const auto stolen = SyncTokenStore::load("kpx-account-session:" + accountId);
    QSignalSpy changed(manager, &AccountManager::accessTokenChanged);
    manager->refreshAccessToken();
    QTRY_COMPARE(changed.count(), 1);

    // The copy's refresh token was rotated already: using it revokes the session...
    auto* thief = fixture.manager(this);
    SyncTokenStore::save("kpx-account-session:" + accountId, stolen);
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
    QVERIFY(SyncTokenStore::load("kpx-account-session:" + accountId).isEmpty());
    // The Secret Key stays: the device is still trusted for the next sign-in
    QVERIFY(!AccountManager::storedSecretKey(ServerId, Email).isEmpty());
}

void TestAccountManager::testServiceAccountExchange()
{
    const auto vectors = loadVectors().value("service_account").toObject();
    QVERIFY(!vectors.isEmpty());
    Fixture fixture;
    fixture.server->saId = vectors.value("sa_id").toString();
    const auto payload = QJsonDocument::fromJson(vectors.value("payload_json").toString().toUtf8()).object();
    fixture.server->saSecret = payload.value("secret").toString();
    fixture.server->wrappedMaster = vectors.value("wrapped_master").toString();
    auto keyset = vectors.value("public").toObject();
    keyset["wrapped"] = vectors.value("wrapped_keyset");
    keyset.remove("fingerprint");
    fixture.server->saKeyset = keyset;

    auto* manager = fixture.manager(this);
    QSignalSpy signedIn(manager, &AccountManager::signedIn);
    manager->signInServiceAccount(vectors.value("token").toString());
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    QCOMPARE(signedIn.count(), 1);
    QVERIFY(manager->isServiceAccount());
    QCOMPARE(manager->principal(), "sa:" + fixture.server->saId);
    // kek -> master key -> keyset
    QCOMPARE(toHex(fromSecure(manager->keyset().encSk)), vectors.value("keyset_enc_sk_hex").toString());
    QCOMPARE(toHex(fromSecure(manager->keyset().signSeed)), vectors.value("keyset_sign_seed_hex").toString());
    // Only the secret went to the server, never the kek
    QVERIFY(!fixture.server->requests.contains("POST auth/refresh"));

    // No refresh token: an expiring token is exchanged again
    const auto first = manager->accessTokenNow();
    fixture.server->accessExpiresIn = 600;
    manager->refreshAccessToken();
    QTRY_VERIFY(manager->accessTokenNow() != first);
    QCOMPARE(fixture.server->requests.count("POST auth/service-account"), 2);
    QCOMPARE(manager->state(), AccountManager::State::Unlocked);

    // A rejected secret
    Fixture other;
    other.server->saId = "someone-else";
    auto* rejected = other.manager(this);
    QSignalSpy errors(rejected, &AccountManager::error);
    rejected->signInServiceAccount(vectors.value("token").toString());
    QTRY_COMPARE(errors.count(), 1);
    QCOMPARE(rejected->state(), AccountManager::State::SignedOut);
    QVERIFY(rejected->keyset().isNull());
}

void TestAccountManager::testKeyringOpensNewestKeyWithFallback()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    const auto encPk = manager->keyset().publicKeys.encPk;

    // "rotating": version 2 is sealed and pending, but the blob is still under version 1
    const auto rotating = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto oldKey = generateVaultKey();
    const auto newKey = generateVaultKey();
    const auto rotatingBlob = vaultBlob(oldKey, "Team", "Old key entry");
    // "rotated": the blob is already re-keyed with version 2
    const auto rotated = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto rotatedOld = generateVaultKey();
    const auto rotatedNew = generateVaultKey();
    const auto rotatedBlob = vaultBlob(rotatedNew, "Infra", "New key entry");
    // Sealed for another vault: the AAD keeps it from opening here
    const auto foreign = QUuid::createUuid().toString(QUuid::WithoutBraces);

    fixture.server->vaults = {
        vaultJson(rotating,
                  oldKey,
                  "Team",
                  {{1, seal(encPk, oldKey, vaultKeyAad(rotating))}, {2, seal(encPk, newKey, vaultKeyAad(rotating))}},
                  1,
                  2),
        vaultJson(
            rotated,
            rotatedNew,
            "Infra",
            {{2, seal(encPk, rotatedNew, vaultKeyAad(rotated))}, {1, seal(encPk, rotatedOld, vaultKeyAad(rotated))}},
            2),
        vaultJson(foreign, oldKey, "Moved", {{1, seal(encPk, oldKey, vaultKeyAad(rotating))}}, 1),
    };
    QSignalSpy vaultsChanged(manager, &AccountManager::vaultsChanged);
    manager->reloadVaults();
    QTRY_COMPARE(vaultsChanged.count(), 1);

    const auto& keyring = manager->keyring();
    QCOMPARE(keyring.vaults().size(), 3);
    QCOMPARE(keyring.keyVersions(rotating), QList<int>({2, 1}));
    QCOMPARE(keyring.vault(rotating).pendingKeyVersion, 2);
    QCOMPARE(keyring.vault(rotating).name, QString("Team"));
    QCOMPARE(keyring.vault(rotated).name, QString("Infra"));
    QVERIFY(keyring.keyVersions(foreign).isEmpty());

    // Newest first, then the older key
    int version = 0;
    QString error;
    auto db = keyring.openVault(rotating, rotatingBlob, &version, &error);
    QVERIFY2(db, qPrintable(error));
    QCOMPARE(version, 1);
    QCOMPARE(db->rootGroup()->entries().first()->title(), QString("Old key entry"));

    db = keyring.openVault(rotated, rotatedBlob, &version, &error);
    QVERIFY2(db, qPrintable(error));
    QCOMPARE(version, 2);
    QCOMPARE(db->rootGroup()->name(), QString("Infra"));

    QVERIFY(!keyring.openVault(foreign, rotatingBlob, &version, &error));
    QVERIFY(!error.isEmpty());

    // What a database synced with this vault uses
    const auto params = manager->vaultRemoteParams(rotated);
    QCOMPARE(params.type, RemoteParams::Type::AccountVault);
    QCOMPARE(params.vaultId, rotated);
    QCOMPARE(params.accountId, manager->accountId());
    QCOMPARE(params.serverUrl, serverConfig().sync);
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
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    // Registering and signing in each listed the vaults once
    QTRY_COMPARE(fixture.server->requests.count("GET vaults"), 2);
    fixture.server->requests.clear();

    // Someone shares a vault: the event comes before the new token and vault list
    const auto vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto key = generateVaultKey();
    fixture.server->vaults = {vaultJson(
        vaultId, key, "Shared", {{1, seal(manager->keyset().publicKeys.encPk, key, vaultKeyAad(vaultId))}}, 1)};
    const auto before = manager->accessTokenNow();

    SyncEventStream::Event event;
    event.type = SyncEventStream::Event::Type::Access;
    event.vault = vaultId;
    event.change = "granted";
    manager->handleAccessEvent(event);
    QTRY_VERIFY(manager->keyring().contains(vaultId));
    QVERIFY(manager->accessTokenNow() != before);
    QCOMPARE(fixture.server->requests.count("POST auth/refresh"), 1);
    QCOMPARE(fixture.server->requests.count("GET vaults"), 1);
    // The refresh came first, so the vault list was read with the new token
    QCOMPARE(fixture.server->bearers.last(), manager->accessTokenNow());
}

void TestAccountManager::testLockClearsKeys()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    manager->signIn(Email, Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    const auto vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto key = generateVaultKey();
    fixture.server->vaults = {
        vaultJson(vaultId, key, "Mine", {{1, seal(manager->keyset().publicKeys.encPk, key, vaultKeyAad(vaultId))}}, 1)};
    manager->reloadVaults();
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

    auto* manager = fixture.manager(this);
    QVERIFY(manager->restoreSession(accountId));
    QCOMPARE(manager->state(), AccountManager::State::Locked);
    QCOMPARE(manager->email(), Email);
    QCOMPARE(manager->server().sync, serverConfig().sync);
    manager->unlock(Password);
    QVERIFY(waitForState(manager, AccountManager::State::Unlocked));
    // The access token came from the stored refresh token
    QCOMPARE(fixture.server->requests.count("POST auth/refresh"), 1);
    QVERIFY(!manager->restoreSession("unknown-account"));
}

void TestAccountManager::testAccountVaultUsesTokenProvider()
{
    Fixture fixture;
    QVERIFY(!registerAlice(fixture, this).isEmpty());
    auto* manager = fixture.manager(this);
    const auto vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    RemoteParams params;
    params.type = RemoteParams::Type::AccountVault;
    params.serverUrl = serverConfig().sync;
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
    QCOMPARE(client.urls.first(), "GET https://sync.keepassxc.example/api/v1/vaults/" + vaultId);
    QCOMPARE(client.urls.last(), QString("GET https://sync.keepassxc.example/api/v1/vaults"));
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
