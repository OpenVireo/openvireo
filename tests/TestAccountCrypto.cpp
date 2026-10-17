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

#include "TestAccountCrypto.h"

#include "config-keepassx.h"

#include "account/AccountCrypto.h"
#include "account/VaultKeyring.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/Argon2Kdf.h"
#include "format/KeePass2Writer.h"
#include "keys/CompositeKey.h"
#include "keys/FileKey.h"

#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

QTEST_GUILESS_MAIN(TestAccountCrypto)

using namespace AccountCrypto;

namespace
{
    // The shared contract every implementation (auth server, sync server, Qt, TS SDK) pins against
    const QString ContractDir = QStringLiteral(KEEPASSX_SOURCE_DIR "/docs/plans/accounts/");

    QByteArray hex(const QJsonValue& value)
    {
        return QByteArray::fromHex(value.toString().toLatin1());
    }

    SecureBytes secureHex(const QJsonValue& value)
    {
        return toSecure(hex(value));
    }

    QByteArray b64(const QJsonValue& value)
    {
        bool ok = false;
        auto data = fromBase64Url(value.toString(), &ok);
        return ok ? data : QByteArray("<invalid base64url>");
    }

    QString hexOf(const SecureBytes& data)
    {
        return toHex(fromSecure(data));
    }

    KdfParams kdfFrom(const QJsonObject& json)
    {
        bool ok = false;
        auto kdf = KdfParams::fromJson(json, &ok);
        return kdf;
    }
} // namespace

void TestAccountCrypto::initTestCase()
{
    QVERIFY(Crypto::init());
    QFile file(ContractDir + "test-vectors.json");
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
    m_vectors = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(m_vectors.value("spec").toString(), QString("kpx v1"));
}

void TestAccountCrypto::testBase64Url()
{
    const QByteArray data("\xfb\xff\xfe", 3);
    QCOMPARE(toBase64Url(data), QString("-__-"));
    bool ok = false;
    QCOMPARE(fromBase64Url("-__-", &ok), data);
    QVERIFY(ok);
    // Padded input is accepted too
    QCOMPARE(fromBase64Url("YQ==", &ok), QByteArray("a"));
    QVERIFY(ok);
    QCOMPARE(fromBase64Url("YQ", &ok), QByteArray("a"));
    QVERIFY(ok);
    fromBase64Url("not base64!", &ok);
    QVERIFY(!ok);
}

void TestAccountCrypto::testSecretKey()
{
    const auto vectors = m_vectors.value("secret_key").toArray();
    QVERIFY(!vectors.isEmpty());
    for (const auto& value : vectors) {
        const auto vector = value.toObject();
        const auto raw = secureHex(vector.value("raw_hex"));
        const auto display = vector.value("display").toString();
        QCOMPARE(formatSecretKey(raw), display);
        SecureBytes parsed;
        QVERIFY(parseSecretKey(display, &parsed));
        QCOMPARE(hexOf(parsed), vector.value("raw_hex").toString());
    }

    const auto generated = generateSecretKey();
    QCOMPARE(static_cast<int>(generated.size()), SecretKeySize);
    SecureBytes parsed;
    QVERIFY(parseSecretKey(formatSecretKey(generated), &parsed));
    QVERIFY(parsed == generated);
}

void TestAccountCrypto::testSecretKeyParsing()
{
    const auto expected = QString("1d70473b0c6e26718ef0f81d0312750f");
    SecureBytes parsed;
    // Whitespace, dashes and case don't matter; O, I and L read as 0, 1 and 1
    QVERIFY(parseSecretKey("  k1-3nr4ee-rcdrk-733qg-z0eg6-4kn1w ", &parsed));
    QCOMPARE(hexOf(parsed), expected);
    QVERIFY(parseSecretKey("K1 3NR4EE RCDRK 733QG ZOEG6 4KNIW", &parsed));
    QCOMPARE(hexOf(parsed), expected);
    QVERIFY(parseSecretKey("K13NR4EERCDRK733QGZ0EG64KNLW", &parsed));
    QCOMPARE(hexOf(parsed), expected);

    // Wrong prefix, length, alphabet or padding bits
    QVERIFY(!parseSecretKey("K2-3NR4EE-RCDRK-733QG-Z0EG6-4KN1W", &parsed));
    QVERIFY(!parseSecretKey("K1-3NR4EE-RCDRK-733QG-Z0EG6-4KN1", &parsed));
    QVERIFY(!parseSecretKey("K1-3NR4EE-RCDRK-733QG-Z0EG6-4KN1WW", &parsed));
    QVERIFY(!parseSecretKey("K1-3NR4EE-RCDRK-733QG-Z0EG6-4KN1U", &parsed));
    // The last character carries two padding bits that must be zero: X = 11101
    QVERIFY(!parseSecretKey("K1-3NR4EE-RCDRK-733QG-Z0EG6-4KN1X", &parsed));
    QVERIFY(!parseSecretKey("", &parsed));
}

void TestAccountCrypto::testAuk()
{
    const auto vectors = m_vectors.value("auk").toArray();
    QCOMPARE(vectors.size(), 3);
    for (const auto& value : vectors) {
        // The last vector uses the production settings (64 MiB), which takes a moment by design
        const auto vector = value.toObject();
        const auto password = vector.value("password").toString();
        const auto normalized = normalizePassword(password);
        QCOMPARE(hexOf(normalized), vector.value("password_nfkc_hex").toString());

        const auto kdf = kdfFrom(vector.value("kdf").toObject());
        QCOMPARE(kdf.memoryKiB, static_cast<quint32>(vector.value("kdf").toObject().value("m").toInt()));
        const auto stretched = stretchPassword(normalized, kdf);
        QCOMPARE(hexOf(stretched), vector.value("argon2id_hex").toString());

        const auto secretKey = secureHex(vector.value("secret_key_hex"));
        QCOMPARE(hexOf(deriveAuk(stretched, secretKey)), vector.value("auk_hex").toString());
        QCOMPARE(hexOf(deriveAuk(password, kdf, secretKey)), vector.value("auk_hex").toString());
    }

    // Clients refuse cheap settings, whatever the server sends
    auto kdf = kdfFrom(vectors.first().toObject().value("kdf").toObject());
    QVERIFY(!kdf.isAcceptable());
    kdf.memoryKiB = KdfParams::MinMemoryKiB;
    kdf.iterations = KdfParams::MinIterations;
    QVERIFY(kdf.isAcceptable());
    kdf.iterations = 1;
    QVERIFY(!kdf.isAcceptable());
    QVERIFY(KdfParams::generate().isAcceptable());
    // The JSON form round trips
    bool ok = false;
    const auto json = kdfFrom(vectors.last().toObject().value("kdf").toObject()).toJson();
    QCOMPARE(json, vectors.last().toObject().value("kdf").toObject());
    KdfParams::fromJson(json, &ok);
    QVERIFY(ok);
}

void TestAccountCrypto::testKeyset()
{
    const auto vector = m_vectors.value("keyset").toObject();
    const auto auk = secureHex(vector.value("auk_hex"));
    const auto keyset =
        keysetFromSecrets(secureHex(vector.value("enc_sk_hex")), secureHex(vector.value("sign_seed_hex")));
    const auto expected = vector.value("public").toObject();
    QCOMPARE(toBase64Url(keyset.publicKeys.encPk), expected.value("enc_pk").toString());
    QCOMPARE(toBase64Url(keyset.publicKeys.signPk), expected.value("sign_pk").toString());
    QCOMPARE(toBase64Url(keyset.publicKeys.sig), expected.value("sig").toString());
    QCOMPARE(fingerprint(keyset.publicKeys), expected.value("fingerprint").toString());
    QCOMPARE(vector.value("aad").toString().toLatin1(), KeysetAad);

    // wrap with the vector's nonce gives the vector's envelope
    const auto wrapped = wrapKeyset(auk, keyset, hex(vector.value("nonce_hex")));
    QCOMPARE(toBase64Url(wrapped), vector.value("wrapped").toString());

    // ...and unwrapping it gives the keys back, checked against the public part
    bool ok = false;
    const auto publicKeys = PublicKeys::fromJson(expected, &ok);
    QVERIFY(ok);
    Keyset unwrapped;
    QVERIFY(unwrapKeyset(auk, b64(vector.value("wrapped")), publicKeys, &unwrapped));
    QVERIFY(unwrapped.encSk == keyset.encSk);
    QVERIFY(unwrapped.signSeed == keyset.signSeed);

    // The wrong AUK, a changed envelope or someone else's public keys are refused
    Error error = Error::None;
    auto wrongAuk = auk;
    wrongAuk[0] ^= 1;
    QVERIFY(!unwrapKeyset(wrongAuk, wrapped, publicKeys, &unwrapped, &error));
    QCOMPARE(error, Error::DecryptionFailed);
    auto tampered = wrapped;
    tampered[tampered.size() - 1] = static_cast<char>(tampered.at(tampered.size() - 1) ^ 1);
    QVERIFY(!unwrapKeyset(auk, tampered, publicKeys, &unwrapped, &error));
    auto otherKeys = PublicKeys::fromJson(m_vectors.value("public_keys").toArray().first().toObject(), &ok);
    QVERIFY(!unwrapKeyset(auk, wrapped, otherKeys, &unwrapped, &error));
    QCOMPARE(error, Error::InvalidInput);

    // A fresh keyset round trips through its JSON form
    auto fresh = generateKeyset();
    QVERIFY(verifyPublicKeys(fresh.publicKeys));
    const auto json = wrappedKeysetJson(auk, fresh);
    Keyset back;
    QVERIFY(unwrapKeyset(auk, json, &back));
    QVERIFY(back.encSk == fresh.encSk);
    fresh.clear();
    QVERIFY(fresh.isNull());
}

void TestAccountCrypto::testPublicKeys()
{
    for (const auto& value : m_vectors.value("public_keys").toArray()) {
        const auto vector = value.toObject();
        const auto keyset =
            keysetFromSecrets(secureHex(vector.value("enc_sk_hex")), secureHex(vector.value("sign_seed_hex")));
        QCOMPARE(toBase64Url(keyset.publicKeys.encPk), vector.value("enc_pk").toString());
        QCOMPARE(toBase64Url(keyset.publicKeys.signPk), vector.value("sign_pk").toString());
        QCOMPARE(toBase64Url(keyset.publicKeys.sig), vector.value("sig").toString());
        QCOMPARE(fingerprint(keyset.publicKeys), vector.value("fingerprint").toString());

        bool ok = false;
        auto publicKeys = PublicKeys::fromJson(vector, &ok);
        QVERIFY(ok);
        QVERIFY(verifyPublicKeys(publicKeys));
        // A server swapping the encryption key is caught before anything is sealed to it
        publicKeys.encPk = b64(m_vectors.value("keyset").toObject().value("public").toObject().value("enc_pk"));
        QVERIFY(!verifyPublicKeys(publicKeys));
    }
}

void TestAccountCrypto::testSeal()
{
    const auto vectors = m_vectors.value("seal").toArray();
    QVERIFY(!vectors.isEmpty());
    for (const auto& value : vectors) {
        const auto vector = value.toObject();
        const auto recipientPk = b64(vector.value("recipient_enc_pk"));
        const auto esk = secureHex(vector.value("ephemeral_sk_hex"));
        QCOMPARE(toHex(x25519PublicKey(esk)), vector.value("ephemeral_pk_hex").toString());
        const auto aad = vector.value("aad").toString().toUtf8();
        const auto plaintext = secureHex(vector.value("plaintext_hex"));

        const auto sealed = seal(recipientPk, plaintext, aad, esk, hex(vector.value("nonce_hex")));
        QCOMPARE(toBase64Url(sealed), vector.value("sealed").toString());

        SecureBytes opened;
        QVERIFY(unseal(secureHex(vector.value("recipient_enc_sk_hex")), sealed, aad, &opened));
        QCOMPARE(hexOf(opened), vector.value("plaintext_hex").toString());
    }

    // The key each seal derives, from either side of the exchange
    for (const auto& value : vectors) {
        const auto vector = value.toObject();
        const auto recipientPk = b64(vector.value("recipient_enc_pk"));
        const auto epk = hex(vector.value("ephemeral_pk_hex"));
        const auto senderSide = sharedSecret(secureHex(vector.value("ephemeral_sk_hex")), recipientPk);
        const auto recipientSide = sharedSecret(secureHex(vector.value("recipient_enc_sk_hex")), epk);
        QVERIFY(!senderSide.empty());
        QVERIFY(senderSide == recipientSide);
        QCOMPARE(hexOf(sealKey(senderSide, epk, recipientPk)), vector.value("derived_key_hex").toString());
    }
}

void TestAccountCrypto::testSealNegative()
{
    for (const auto& value : m_vectors.value("seal_negative").toArray()) {
        const auto vector = value.toObject();
        const auto expect = vector.value("expect").toString();
        SecureBytes opened;
        Error error = Error::None;
        QVERIFY2(!unseal(secureHex(vector.value("recipient_enc_sk_hex")),
                         b64(vector.value("sealed")),
                         vector.value("aad").toString().toUtf8(),
                         &opened,
                         &error),
                 qPrintable(vector.value("case").toString()));
        if (expect == "unsupported version") {
            QCOMPARE(error, Error::UnsupportedVersion);
        } else {
            QCOMPARE(expect, QString("decryption failure"));
            QCOMPARE(error, Error::DecryptionFailed);
        }
    }
}

void TestAccountCrypto::testSealRoundTrip()
{
    const auto recipient = generateKeyset();
    const auto vaultKey = generateVaultKey();
    const auto aad = vaultKeyAad("3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b");
    const auto sealed = seal(recipient.publicKeys.encPk, vaultKey, aad);
    QCOMPARE(sealed.size(), 1 + 32 + 24 + 32 + 16);
    // Fresh randomness every time
    QVERIFY(seal(recipient.publicKeys.encPk, vaultKey, aad) != sealed);
    SecureBytes opened;
    QVERIFY(unseal(recipient.encSk, sealed, aad, &opened));
    QVERIFY(opened == vaultKey);
    QVERIFY(!unseal(recipient.encSk, sealed, vaultKeyAad("00000000-0000-0000-0000-000000000000"), &opened));
    // A low-order public key gives an all-zero shared secret, which is refused
    QVERIFY(seal(QByteArray(32, '\0'), vaultKey, aad).isEmpty());
}

void TestAccountCrypto::testVaultName()
{
    const auto vector = m_vectors.value("vault_name").toObject();
    const auto vaultKey = secureHex(vector.value("vault_key_hex"));
    const auto vaultId = vector.value("vault_id").toString();
    QCOMPARE(hexOf(vaultNameKey(vaultKey)), vector.value("name_key_hex").toString());
    QCOMPARE(QString::fromUtf8(vaultNameAad(vaultId)), vector.value("aad").toString());

    const auto encrypted =
        encryptVaultName(vaultKey, vaultId, vector.value("name").toString(), hex(vector.value("nonce_hex")));
    QCOMPARE(toBase64Url(encrypted), vector.value("encrypted_name").toString());
    QString name;
    QVERIFY(decryptVaultName(vaultKey, vaultId, b64(vector.value("encrypted_name")), &name));
    QCOMPARE(name, vector.value("name").toString());
    // Bound to its vault
    QVERIFY(!decryptVaultName(vaultKey, "00000000-0000-0000-0000-000000000000", encrypted, &name));
}

void TestAccountCrypto::testServiceAccount()
{
    const auto vector = m_vectors.value("service_account").toObject();
    const auto saId = vector.value("sa_id").toString();
    const auto payload = QJsonDocument::fromJson(vector.value("payload_json").toString().toUtf8()).object();

    // Building the token gives the vector's token, byte for byte
    ServiceAccountToken token;
    token.url = payload.value("url").toString();
    token.saId = saId;
    token.secret = toSecure(b64(payload.value("secret")));
    token.kek = secureHex(vector.value("kek_hex"));
    QCOMPARE(toBase64Url(token.kek), payload.value("kek").toString());
    QCOMPARE(buildServiceAccountToken(token), vector.value("token").toString());
    QCOMPARE(token.secretHash(), vector.value("secret_hash_hex").toString());
    QCOMPARE(token.secretPrefix(), vector.value("secret_prefix").toString());

    ServiceAccountToken parsed;
    QVERIFY(parseServiceAccountToken(vector.value("token").toString(), &parsed));
    QCOMPARE(parsed.version, 1);
    QCOMPARE(parsed.url, token.url);
    QCOMPARE(parsed.saId, saId);
    QVERIFY(parsed.secret == token.secret);
    QVERIFY(parsed.kek == token.kek);
    QVERIFY(!parseServiceAccountToken("kpx_sa_bm90IGpzb24", &parsed));
    QVERIFY(!parseServiceAccountToken(vector.value("token").toString().mid(7), &parsed));

    // The unwrap chain: kek -> master key -> keyset
    const auto master = secureHex(vector.value("master_key_hex"));
    const auto aad = saMasterAad(saId);
    QCOMPARE(QString::fromUtf8(aad), vector.value("wrapped_master_aad").toString());
    QCOMPARE(toBase64Url(wrap(token.kek, master, aad, hex(vector.value("wrapped_master_nonce_hex")))),
             vector.value("wrapped_master").toString());
    SecureBytes unwrappedMaster;
    QVERIFY(unwrap(parsed.kek, b64(vector.value("wrapped_master")), aad, &unwrappedMaster));
    QVERIFY(unwrappedMaster == master);

    const auto keyset = keysetFromSecrets(secureHex(vector.value("keyset_enc_sk_hex")),
                                          secureHex(vector.value("keyset_sign_seed_hex")));
    const auto expected = vector.value("public").toObject();
    QCOMPARE(toBase64Url(keyset.publicKeys.encPk), expected.value("enc_pk").toString());
    QCOMPARE(toBase64Url(keyset.publicKeys.signPk), expected.value("sign_pk").toString());
    QCOMPARE(toBase64Url(keyset.publicKeys.sig), expected.value("sig").toString());
    QCOMPARE(fingerprint(keyset.publicKeys), expected.value("fingerprint").toString());
    QCOMPARE(toBase64Url(wrapKeyset(master, keyset, hex(vector.value("wrapped_keyset_nonce_hex")))),
             vector.value("wrapped_keyset").toString());
    auto keysetJson = expected;
    keysetJson["wrapped"] = vector.value("wrapped_keyset");
    Keyset unwrapped;
    QVERIFY(unwrapKeyset(unwrappedMaster, keysetJson, &unwrapped));
    QVERIFY(unwrapped.encSk == keyset.encSk);

    // The master key sealed to an org admin (alice of the keyset vector)
    const auto sealed = vector.value("master_sealed_to_admin").toObject();
    const auto alice = m_vectors.value("keyset").toObject();
    QCOMPARE(toBase64Url(seal(b64(alice.value("public").toObject().value("enc_pk")),
                              master,
                              sealed.value("aad").toString().toUtf8(),
                              secureHex(sealed.value("ephemeral_sk_hex")),
                              hex(sealed.value("nonce_hex")))),
             sealed.value("sealed").toString());
    SecureBytes opened;
    QVERIFY(unseal(secureHex(alice.value("enc_sk_hex")), b64(sealed.value("sealed")), aad, &opened));
    QVERIFY(opened == master);
}

void TestAccountCrypto::testRefreshTokenHash()
{
    const auto vector = m_vectors.value("refresh_token").toObject();
    QCOMPARE(refreshTokenHash(vector.value("token").toString()), vector.value("stored_hash_hex").toString());
}

void TestAccountCrypto::testJwtClaims()
{
    const auto jwt = m_vectors.value("jwt").toObject();
    for (const auto& value : jwt.value("valid").toArray()) {
        const auto vector = value.toObject();
        const auto expected = vector.value("claims").toObject();
        JwtClaims claims;
        QVERIFY(parseJwtClaims(vector.value("token").toString(), &claims));
        QCOMPARE(claims.alg, QString("EdDSA"));
        QCOMPARE(claims.kid, QString("test-2026-10"));
        QCOMPARE(claims.iss, expected.value("iss").toString());
        QCOMPARE(claims.aud, QStringList({"kpx-api", "kpx-sync"}));
        QCOMPARE(claims.sub, expected.value("sub").toString());
        QCOMPARE(claims.dev, expected.value("dev").toString());
        QCOMPARE(claims.org, expected.value("org").toString());
        QCOMPARE(claims.iat, expected.value("iat").toInteger());
        QCOMPARE(claims.nbf, expected.value("nbf").toInteger());
        QCOMPARE(claims.exp, expected.value("exp").toInteger());
        QCOMPARE(claims.exp - claims.iat, 600);
        QCOMPARE(claims.jti, expected.value("jti").toString());
        const auto vlt = expected.value("vlt").toArray();
        QCOMPARE(claims.vaults.size(), vlt.size());
        for (int i = 0; i < vlt.size(); ++i) {
            QCOMPARE(claims.vaults.at(i).id, vlt.at(i).toObject().value("id").toString());
            QCOMPARE(claims.vaults.at(i).role, vlt.at(i).toObject().value("r").toString());
        }

        // The signature is the issuer's Ed25519 over header.payload, with the key in the JWKS
        const auto parts = vector.value("token").toString().split('.');
        const auto x = b64(jwt.value("jwks").toObject().value("keys").toArray().first().toObject().value("x"));
        QCOMPARE(toBase64Url(ed25519PublicKey(secureHex(jwt.value("signing_seed_hex")))), toBase64Url(x));
        QVERIFY(verify(x, (parts.at(0) + "." + parts.at(1)).toLatin1(), b64(parts.at(2))));
    }

    // Clients only read claims; the invalid tokens still parse (the servers reject them), except a
    // bad signature, which the JWKS key catches
    const auto x = b64(jwt.value("jwks").toObject().value("keys").toArray().first().toObject().value("x"));
    for (const auto& value : jwt.value("invalid").toArray()) {
        const auto vector = value.toObject();
        JwtClaims claims;
        QVERIFY(parseJwtClaims(vector.value("token").toString(), &claims));
        const auto parts = vector.value("token").toString().split('.');
        const auto signedPart = (parts.at(0) + "." + parts.at(1)).toLatin1();
        const auto validSignature = verify(x, signedPart, b64(parts.at(2)));
        const auto caseName = vector.value("case").toString();
        if (caseName == "bad signature" || caseName == "alg none") {
            QVERIFY2(!validSignature, qPrintable(caseName));
        } else if (caseName == "expired") {
            QVERIFY(claims.exp + 60 < jwt.value("now").toInteger());
        } else if (caseName == "not yet valid") {
            QVERIFY(claims.nbf - 60 > jwt.value("now").toInteger());
        } else if (caseName == "wrong audience") {
            QVERIFY(!claims.aud.contains("kpx-sync"));
        } else if (caseName == "wrong issuer") {
            QVERIFY(claims.iss != "https://accounts.keepassxc.example");
        } else if (caseName.startsWith("unknown kid")) {
            QCOMPARE(claims.kid, QString("unknown-kid"));
        }
        if (caseName == "alg none") {
            QCOMPARE(claims.alg, QString("none"));
        }
    }
    JwtClaims claims;
    QVERIFY(!parseJwtClaims("not.a.jwt", &claims));
    QVERIFY(!parseJwtClaims("onlyone", &claims));
}

void TestAccountCrypto::testVaultFixture()
{
    const auto vector = m_vectors.value("vault_kdbx").toObject();
    QFile file(ContractDir + vector.value("file").toString());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto data = file.readAll();

    // The vault key is a raw 32-byte key file component, read from memory, with no password
    const auto vaultKey = secureHex(vector.value("vault_key_hex"));
    auto keyData = fromSecure(vaultKey);
    QBuffer keyBuffer(&keyData);
    QVERIFY(keyBuffer.open(QIODevice::ReadOnly));
    auto fileKey = QSharedPointer<FileKey>::create();
    QVERIFY(fileKey->load(&keyBuffer));
    QCOMPARE(fileKey->type(), FileKey::FixedBinary);
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(fileKey);

    QString error;
    auto db = VaultKeyring::readVault(data, key, &error);
    QVERIFY2(db, qPrintable(error));
    // The same through the keyring's helper
    QVERIFY(VaultKeyring::readVault(data, VaultKeyring::compositeKeyFor(vaultKey)));

    const auto kdf = db->kdf().dynamicCast<Argon2Kdf>();
    QVERIFY(kdf);
    QCOMPARE(kdf->type(), Argon2Kdf::Type::Argon2id);
    QCOMPARE(kdf->memory(), static_cast<quint64>(vector.value("kdf").toObject().value("m_kib").toInt()));
    QCOMPARE(kdf->rounds(), vector.value("kdf").toObject().value("t").toInt());
    QCOMPARE(kdf->parallelism(), static_cast<quint32>(vector.value("kdf").toObject().value("p").toInt()));

    const auto expect = vector.value("expect").toObject();
    QCOMPARE(db->rootGroup()->name(), expect.value("root_group").toString());
    const auto expectedEntry = expect.value("entry").toObject();
    const auto entries = db->rootGroup()->entriesRecursive();
    Entry* found = nullptr;
    for (auto* entry : entries) {
        if (entry->title() == expectedEntry.value("Title").toString()) {
            found = entry;
        }
    }
    QVERIFY(found);
    QCOMPARE(found->username(), expectedEntry.value("UserName").toString());
    QCOMPARE(found->password(), expectedEntry.value("Password").toString());
    QCOMPARE(found->url(), expectedEntry.value("URL").toString());

    // Another key doesn't open it
    auto wrongKey = vaultKey;
    wrongKey[0] ^= 1;
    QVERIFY(!VaultKeyring::readVault(data, VaultKeyring::compositeKeyFor(wrongKey)));
}

void TestAccountCrypto::testVaultKeyRoundTrip()
{
    // A vault created here opens with its key, with the spec's KDF settings
    const auto vaultKey = generateVaultKey();
    auto db = VaultKeyring::createVaultDatabase("Personal", vaultKey);
    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle("Mail");
    entry->setGroup(db->rootGroup());

    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    KeePass2Writer writer;
    QVERIFY2(writer.writeDatabase(&buffer, db.data()), qPrintable(writer.errorString()));
    auto opened = VaultKeyring::readVault(buffer.data(), VaultKeyring::compositeKeyFor(vaultKey));
    QVERIFY(opened);
    QCOMPARE(opened->rootGroup()->name(), QString("Personal"));
    QCOMPARE(opened->rootGroup()->entries().first()->title(), QString("Mail"));
    const auto kdf = opened->kdf().dynamicCast<Argon2Kdf>();
    QVERIFY(kdf);
    QCOMPARE(kdf->memory(), VaultKeyring::VaultKdfMemoryKiB);
    QCOMPARE(kdf->rounds(), static_cast<int>(VaultKeyring::VaultKdfIterations));
}
