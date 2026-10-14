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

#include "TestAutofill.h"

#include "autofill/AutofillCredentials.h"
#include "autofill/AutofillHost.h"
#include "autofill/AutofillPlatform.h"
#include "autofill/AutofillProtocol.h"
#include "browser/BrowserEntryConfig.h"
#include "browser/BrowserPasskeys.h"
#include "browser/BrowserService.h"
#include "config-keepassx.h"
#include "core/Clock.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Global.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/Tools.h"
#include "core/Totp.h"
#include "crypto/Crypto.h"

#include <QCborMap>
#include <QCborValue>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <botan/data_src.h>
#include <botan/pkcs8.h>
#include <botan/pubkey.h>

QTEST_GUILESS_MAIN(TestAutofill)

using AutofillProtocol::FrameReader;

namespace
{
    Entry* addEntry(Group* group, const QString& title, const QString& url, const QString& password = "secret")
    {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setGroup(group);
        entry->setTitle(title);
        entry->setUsername(title.toLower() + "@example.com");
        entry->setUrl(url);
        entry->setPassword(password);
        return entry;
    }

    QList<AutofillIdentity> identitiesOfType(const QList<AutofillIdentity>& identities, AutofillIdentity::Type type)
    {
        QList<AutofillIdentity> result;
        for (const auto& identity : identities) {
            if (identity.type == type) {
                result << identity;
            }
        }
        return result;
    }

    QStringList services(const QList<AutofillIdentity>& identities)
    {
        QStringList result;
        for (const auto& identity : identities) {
            result << identity.service;
        }
        result.sort();
        return result;
    }

    bool verifySignature(const QByteArray& privateKeyPem, const QByteArray& message, const QByteArray& signature)
    {
        Botan::DataSource_Memory source(reinterpret_cast<const uint8_t*>(privateKeyPem.constData()),
                                        privateKeyPem.size());
        const auto key = Botan::PKCS8::load_key(source);
#ifdef WITH_BOTAN3
        Botan::PK_Verifier verifier(*key, "EMSA1(SHA-256)", Botan::Signature_Format::DerSequence);
#else
        Botan::PK_Verifier verifier(*key, "EMSA1(SHA-256)", Botan::DER_SEQUENCE);
#endif
        return verifier.verify_message(reinterpret_cast<const uint8_t*>(message.constData()),
                                       message.size(),
                                       reinterpret_cast<const uint8_t*>(signature.constData()),
                                       signature.size());
    }

    // Reads one frame while letting the event loop run, which the host in the same process needs to answer
    QByteArray readFrame(QLocalSocket& socket)
    {
        QByteArray data;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 2000) {
            QCoreApplication::processEvents();
            socket.waitForReadyRead(10);
            data += socket.readAll();
            if (data.size() >= 4
                && static_cast<quint32>(data.size()) >= 4 + qFromBigEndian<quint32>(data.constData())) {
                break;
            }
        }
        return data;
    }
} // namespace

void TestAutofill::initTestCase()
{
    QVERIFY(Crypto::init());
    qRegisterMetaType<QLocalSocket*>();
}

void TestAutofill::testFrameRoundTrip()
{
    const QJsonObject message({{"type", "getCredential"}, {"recordId", "a:b"}});
    const auto frame = AutofillProtocol::encodeFrame(message);
    QCOMPARE(qFromBigEndian<quint32>(frame.constData()), static_cast<quint32>(frame.size() - 4));

    FrameReader reader;
    reader.append(frame);
    QCOMPARE(reader.next(), message);
    QVERIFY(reader.next().isEmpty());
    QVERIFY(!reader.hasError());
}

void TestAutofill::testFrameReaderPartialAndCoalesced()
{
    const QJsonObject first({{"type", "status"}});
    const QJsonObject second({{"type", "listCredentials"}, {"kind", "password"}});
    const auto stream = AutofillProtocol::encodeFrame(first) + AutofillProtocol::encodeFrame(second);

    // Byte by byte, as a slow socket might deliver it
    FrameReader reader;
    QList<QJsonObject> messages;
    for (const char byte : stream) {
        reader.append(QByteArray(1, byte));
        for (auto message = reader.next(); !message.isEmpty(); message = reader.next()) {
            messages << message;
        }
    }
    QCOMPARE(messages, QList<QJsonObject>({first, second}));

    // Both at once
    FrameReader coalesced;
    coalesced.append(stream);
    QCOMPARE(coalesced.next(), first);
    QCOMPARE(coalesced.next(), second);
    QVERIFY(!coalesced.hasError());
}

void TestAutofill::testFrameReaderRejectsBadFrames()
{
    QByteArray tooLarge(4, '\0');
    qToBigEndian<quint32>(AutofillProtocol::MaxFrameSize + 1, tooLarge.data());
    FrameReader large;
    large.append(tooLarge);
    QVERIFY(large.next().isEmpty());
    QVERIFY(large.hasError());

    const QByteArray body("[1, 2]");
    QByteArray notAnObject(4, '\0');
    qToBigEndian<quint32>(body.size(), notAnObject.data());
    FrameReader array;
    array.append(notAnObject + body);
    QVERIFY(array.next().isEmpty());
    QVERIFY(array.hasError());

    FrameReader empty;
    empty.append(QByteArray(4, '\0'));
    QVERIFY(empty.next().isEmpty());
    QVERIFY(empty.hasError());
}

void TestAutofill::testRecordId()
{
    Database db;
    auto* entry = addEntry(db.rootGroup(), "Example", "https://example.com");

    const auto recordId = AutofillCredentials::recordId(&db, entry);
    QUuid dbUuid;
    QUuid entryUuid;
    QVERIFY(AutofillCredentials::parseRecordId(recordId, &dbUuid, &entryUuid));
    QCOMPARE(dbUuid, db.rootGroup()->uuid());
    QCOMPARE(entryUuid, entry->uuid());

    QVERIFY(!AutofillCredentials::parseRecordId("", nullptr, nullptr));
    QVERIFY(!AutofillCredentials::parseRecordId("nothex:nothex", nullptr, nullptr));
    QVERIFY(!AutofillCredentials::parseRecordId(recordId + ":extra", nullptr, nullptr));
}

void TestAutofill::testNormalizedHost_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("host");

    QTest::newRow("https") << "https://login.example.com/path?q=1" << "login.example.com";
    QTest::newRow("www") << "https://www.Example.com" << "example.com";
    QTest::newRow("bare host") << "example.com" << "example.com";
    QTest::newRow("bare host with path") << "example.com/login" << "example.com";
    QTest::newRow("port") << "http://example.com:8080/" << "example.com";
    QTest::newRow("app scheme") << "androidapp://com.example" << "";
    QTest::newRow("command") << "cmd://ssh example.com" << "";
    QTest::newRow("empty") << "" << "";
}

void TestAutofill::testNormalizedHost()
{
    QFETCH(QString, url);
    QFETCH(QString, host);
    QCOMPARE(AutofillCredentials::normalizedHost(url), host);
}

void TestAutofill::testHostMatches()
{
    QVERIFY(AutofillCredentials::hostMatches("example.com", "example.com"));
    QVERIFY(AutofillCredentials::hostMatches("example.com", "login.example.com"));
    QVERIFY(!AutofillCredentials::hostMatches("login.example.com", "example.com"));
    QVERIFY(!AutofillCredentials::hostMatches("example.com", "badexample.com"));
    QVERIFY(!AutofillCredentials::hostMatches("example.com", "example.com.evil.net"));
    QVERIFY(!AutofillCredentials::hostMatches("", "example.com"));
}

void TestAutofill::testIdentitiesForDatabase()
{
    Database db;
    auto* root = db.rootGroup();

    auto* web = addEntry(root, "Web", "https://www.example.com/login");
    web->attributes()->set(EntryAttributes::AdditionalUrlAttribute, "https://accounts.example.org");

    auto* otp = addEntry(root, "Code", "github.com", "");
    otp->setTotp(Totp::createSettings("JBSWY3DPEHPK3PXP"));

    auto* passkey = addEntry(root, "Passkey", "", "");
    passkey->attributes()->set(EntryAttributes::KPEX_PASSKEY_RELYING_PARTY, "webauthn.io");
    passkey->attributes()->set(EntryAttributes::KPEX_PASSKEY_USERNAME, "passkey-user");
    passkey->attributes()->set(EntryAttributes::KPEX_PASSKEY_CREDENTIAL_ID, "AQIDBA");
    passkey->attributes()->set(EntryAttributes::KPEX_PASSKEY_USER_HANDLE, "BQYHCA");
    passkey->attributes()->set(EntryAttributes::KPEX_PASSKEY_PRIVATE_KEY_PEM, "-----BEGIN PRIVATE KEY-----", true);

    const auto identities = AutofillCredentials::identitiesForDatabase(&db);

    const auto passwords = identitiesOfType(identities, AutofillIdentity::Type::Password);
    QCOMPARE(services(passwords), QStringList({"accounts.example.org", "example.com"}));
    for (const auto& identity : passwords) {
        QCOMPARE(identity.user, QString("web@example.com"));
        QCOMPARE(identity.recordId, AutofillCredentials::recordId(&db, web));
    }

    const auto codes = identitiesOfType(identities, AutofillIdentity::Type::OneTimeCode);
    QCOMPARE(services(codes), QStringList({"github.com"}));
    QCOMPARE(codes.first().recordId, AutofillCredentials::recordId(&db, otp));

    const auto passkeys = identitiesOfType(identities, AutofillIdentity::Type::Passkey);
    QCOMPARE(passkeys.size(), 1);
    QCOMPARE(passkeys.first().service, QString("webauthn.io"));
    QCOMPARE(passkeys.first().user, QString("passkey-user"));
    QCOMPARE(passkeys.first().credentialId, QByteArray("\x01\x02\x03\x04"));
    QCOMPARE(passkeys.first().userHandle, QByteArray("\x05\x06\x07\x08"));

    // An entry without a password or code gives the store nothing for its URL
    QCOMPARE(identities.size(), 4);
}

void TestAutofill::testIdentitiesSkipExcludedEntries()
{
    Database db;
    db.metadata()->setRecycleBinEnabled(true);
    auto* root = db.rootGroup();

    addEntry(root, "Kept", "https://kept.example");

    auto* expired = addEntry(root, "Expired", "https://expired.example");
    expired->setExpires(true);
    expired->setExpiryTime(Clock::currentDateTimeUtc().addDays(-1));

    auto* hidden = addEntry(root, "Hidden", "https://hidden.example");
    hidden->customData()->set(BrowserService::OPTION_HIDE_ENTRY, TRUE_STR);

    auto* hiddenGroup = new Group();
    hiddenGroup->setUuid(QUuid::createUuid());
    hiddenGroup->setParent(root);
    hiddenGroup->setCustomDataTriState(BrowserService::OPTION_HIDE_ENTRY, Group::Enable);
    addEntry(hiddenGroup, "InHiddenGroup", "https://group.example");

    auto* recycled = addEntry(root, "Recycled", "https://recycled.example");
    db.recycleEntry(recycled);

    auto* denied = addEntry(root, "Denied", "https://denied.example");
    denied->attributes()->set(EntryAttributes::AdditionalUrlAttribute, "https://allowed.example");
    BrowserEntryConfig config;
    config.deny("denied.example");
    config.save(denied);

    const auto identities = AutofillCredentials::identitiesForDatabase(&db);
    QCOMPARE(services(identities), QStringList({"allowed.example", "kept.example"}));
}

void TestAutofill::testMatchingEntries()
{
    auto db = QSharedPointer<Database>::create();
    auto* root = db->rootGroup();
    auto* example = addEntry(root, "Example", "https://example.com");
    auto* login = addEntry(root, "Login", "https://login.example.com");
    auto* other = addEntry(root, "Other", "https://other.example");
    const QList<QSharedPointer<Database>> databases({db});

    auto entries = AutofillCredentials::matchingEntries(databases, {"https://login.example.com/signin"});
    QCOMPARE(entries.size(), 2);
    QVERIFY(entries.contains(example));
    QVERIFY(entries.contains(login));

    entries = AutofillCredentials::matchingEntries(databases, {"example.com"});
    QCOMPARE(entries, QList<Entry*>({example}));

    // With nothing to match on, the user picks from every entry
    entries = AutofillCredentials::matchingEntries(databases, {});
    QCOMPARE(entries.size(), 3);
    QVERIFY(entries.contains(other));
}

void TestAutofill::testFindEntry()
{
    auto db = QSharedPointer<Database>::create();
    auto* entry = addEntry(db->rootGroup(), "Example", "https://example.com");
    const QList<QSharedPointer<Database>> databases({db});

    Database* found = nullptr;
    QCOMPARE(AutofillCredentials::findEntry(databases, AutofillCredentials::recordId(db.data(), entry), &found), entry);
    QCOMPARE(found, db.data());

    // A record of another database, even with the same entry uuid, is not found
    auto otherDb = QSharedPointer<Database>::create();
    const auto foreignRecord = Tools::uuidToHex(otherDb->rootGroup()->uuid()) + ":" + entry->uuidToHex();
    QVERIFY(!AutofillCredentials::findEntry(databases, foreignRecord));

    // Hidden entries can't be fetched by record id either
    entry->customData()->set(BrowserService::OPTION_HIDE_ENTRY, TRUE_STR);
    QVERIFY(!AutofillCredentials::findEntry(databases, AutofillCredentials::recordId(db.data(), entry)));
}

void TestAutofill::testHostRefusesUntrustedPeer()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("autofill.sock");

    // This test binary is not the signed extension, so the real code signature check refuses it
    AutofillHost host;
    host.setPeerValidator(&AutofillPlatform::isTrustedPeer);
    QVERIFY(host.start(path));
    QSignalSpy requests(&host, &AutofillHost::requestReceived);

    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected(2000));
    client.write(AutofillProtocol::encodeFrame({{"type", "status"}}));
    client.flush();

    QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
    QCOMPARE(requests.count(), 0);
}

void TestAutofill::testHostRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("autofill.sock");

    AutofillHost host;
    // Without a validator the host refuses to listen at all
    QVERIFY(!host.start(path));

    int validatedDescriptor = -1;
    host.setPeerValidator([&](int descriptor, QString*) {
        validatedDescriptor = descriptor;
        return true;
    });
    QVERIFY(host.start(path));
    connect(&host, &AutofillHost::requestReceived, &host, [&](QLocalSocket* socket, const QJsonObject& request) {
        host.sendResponse(socket, AutofillProtocol::success({{"echo", request.value("type")}}));
    });

    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected(2000));
    client.write(AutofillProtocol::encodeFrame({{"type", "status"}}));
    client.flush();

    FrameReader reader;
    reader.append(readFrame(client));
    const auto response = reader.next();
    QVERIFY(validatedDescriptor >= 0);
    QCOMPARE(response.value("ok").toBool(), true);
    QCOMPARE(response.value("echo").toString(), QString("status"));
}

void TestAutofill::testPasskeyAssertionWithClientDataHash()
{
    const auto registration = browserPasskeys()->buildRegistrationWithClientDataHash("webauthn.io", {-7});
    QVERIFY(!registration.privateKeyPem.isEmpty());

    const auto clientDataHash = QCryptographicHash::hash("client data", QCryptographicHash::Sha256);
    const auto assertion = browserPasskeys()->buildAssertionWithClientDataHash(
        "webauthn.io", clientDataHash, QString::fromUtf8(registration.privateKeyPem));
    QVERIFY(!assertion.signature.isEmpty());

    // Authenticator data starts with the relying party's hash and has the user present and verified flags
    const auto authData = assertion.authenticatorData;
    QCOMPARE(authData.left(32), QCryptographicHash::hash("webauthn.io", QCryptographicHash::Sha256));
    QVERIFY(authData.at(32) & 0x01);
    QVERIFY(authData.at(32) & 0x04);

    // The signature covers the authenticator data and the hash we were given, not a hash of our own
    QVERIFY(verifySignature(registration.privateKeyPem, authData + clientDataHash, assertion.signature));
    QVERIFY(!verifySignature(registration.privateKeyPem, authData + QByteArray(32, '\0'), assertion.signature));

    // A hash of the wrong size is refused
    QVERIFY(
        browserPasskeys()
            ->buildAssertionWithClientDataHash("webauthn.io", "short", QString::fromUtf8(registration.privateKeyPem))
            .signature.isEmpty());
}

void TestAutofill::testPasskeyRegistrationWithClientDataHash()
{
    // Algorithms we don't support are skipped for the first one we do
    const auto registration = browserPasskeys()->buildRegistrationWithClientDataHash("webauthn.io", {-35, -8});
    QVERIFY(!registration.credentialId.isEmpty());
    QVERIFY(registration.privateKeyPem.contains("PRIVATE KEY"));

    const auto attestation = QCborValue::fromCbor(registration.attestationObject).toMap();
    QCOMPARE(attestation.value(QStringLiteral("fmt")).toString(), QString("none"));
    const auto authData = attestation.value(QStringLiteral("authData")).toByteArray();
    QCOMPARE(authData.left(32), QCryptographicHash::hash("webauthn.io", QCryptographicHash::Sha256));
    // Attested credential data is present
    QVERIFY(authData.at(32) & 0x40);

    const auto credentialId =
        QByteArray::fromBase64(registration.credentialId.toLatin1(), QByteArray::Base64UrlEncoding);
    QCOMPARE(authData.mid(55, credentialId.size()), credentialId);

    QVERIFY(browserPasskeys()->buildRegistrationWithClientDataHash("", {-7}).attestationObject.isEmpty());
    QVERIFY(browserPasskeys()->buildRegistrationWithClientDataHash("webauthn.io", {-35}).attestationObject.isEmpty());
}
