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

#include "AccountCrypto.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <botan/mem_ops.h>

#include <ovcore.h>
#include <ovdesktop.h>


/*
 * The crypto itself runs in openvireo-core (ov-crypto, through <ovcore.h> and <ovdesktop.h>), the same code
 * the web, iOS and CLI clients use. This file converts between Qt types and the core's byte buffers.
 */

namespace AccountCrypto
{
    const QByteArray KeysetAad = QByteArrayLiteral("ov-keyset-v1");

    namespace
    {
        const QByteArray PublicKeysContext = QByteArrayLiteral("ov-pubkeys-v1");
        const QByteArray SealInfo = QByteArrayLiteral("ov-seal-v1");
        const QByteArray AukInfo = QByteArrayLiteral("ov-auk-v1");

        rust::Slice<const uint8_t> slice(const QByteArray& data)
        {
            return {reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size())};
        }

        rust::Slice<const uint8_t> slice(const SecureBytes& data)
        {
            return {data.data(), data.size()};
        }

        QByteArray toByteArray(const rust::Vec<uint8_t>& data)
        {
            return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
        }

        // Moves a secret out of the core's buffer and wipes the buffer
        SecureBytes takeSecure(rust::Vec<uint8_t>&& data)
        {
            SecureBytes secure(data.begin(), data.end());
            ov::wipe(data);
            return secure;
        }

        // The wire code at the start of a core error, "decryption_failed: ..." (rust-core-tasks.md §2.4)
        Error errorOf(const rust::Error& e)
        {
            const auto code = QByteArray(e.what()).split(':').value(0);
            if (code == "unsupported_version") {
                return Error::UnsupportedVersion;
            }
            if (code == "decryption_failed" || code == "low_order_point") {
                return Error::DecryptionFailed;
            }
            return Error::InvalidInput;
        }

        bool fail(Error* error, Error reason)
        {
            if (error) {
                *error = reason;
            }
            return false;
        }

        bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
        {
            return a.size() == b.size()
                   && Botan::constant_time_compare(reinterpret_cast<const uint8_t*>(a.constData()),
                                                   reinterpret_cast<const uint8_t*>(b.constData()),
                                                   static_cast<size_t>(a.size()));
        }

        QJsonObject decodeJsonPart(const QString& part, bool* ok)
        {
            bool decoded = false;
            const auto data = fromBase64Url(part, &decoded);
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(data, &error);
            *ok = decoded && error.error == QJsonParseError::NoError && doc.isObject();
            return doc.object();
        }
    } // namespace

    QByteArray vaultKeyAad(const QString& vaultId)
    {
        return "ov-vault-key|" + vaultId.toUtf8();
    }

    QByteArray saMasterAad(const QString& saId)
    {
        return "ov-sa-master-v1|" + saId.toUtf8();
    }

    QByteArray vaultNameAad(const QString& vaultId)
    {
        return "ov-vault-name|" + vaultId.toUtf8();
    }

    QString toBase64Url(const QByteArray& data)
    {
        return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    QString toBase64Url(const SecureBytes& data)
    {
        auto bytes = fromSecure(data);
        auto text = toBase64Url(bytes);
        Botan::secure_scrub_memory(bytes.data(), static_cast<size_t>(bytes.size()));
        return text;
    }

    QByteArray fromBase64Url(const QString& text, bool* ok)
    {
        auto result = QByteArray::fromBase64Encoding(
            text.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
        if (ok) {
            *ok = static_cast<bool>(result);
        }
        return result ? *result : QByteArray();
    }

    SecureBytes secureFromBase64Url(const QString& text, bool* ok)
    {
        auto data = fromBase64Url(text, ok);
        auto secure = toSecure(data);
        Botan::secure_scrub_memory(data.data(), static_cast<size_t>(data.size()));
        return secure;
    }

    SecureBytes toSecure(const QByteArray& data)
    {
        return SecureBytes(data.begin(), data.end());
    }

    QByteArray fromSecure(const SecureBytes& data)
    {
        return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    }

    SecureBytes randomSecure(int size)
    {
        return takeSecure(ov::random_bytes(static_cast<size_t>(size)));
    }

    QByteArray sha256(const QByteArray& data)
    {
        return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    }

    QString toHex(const QByteArray& data)
    {
        return QString::fromLatin1(data.toHex());
    }

    SecureBytes hkdf(const SecureBytes& ikm, const QByteArray& salt, const QByteArray& info, int length)
    {
        try {
            return takeSecure(ov::hkdf_sha256(slice(ikm), slice(salt), slice(info), static_cast<size_t>(length)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    SecureBytes generateSecretKey()
    {
        try {
            return takeSecure(ovd::generate_secret_key());
        } catch (const rust::Error&) {
            return {};
        }
    }

    QString formatSecretKey(const SecureBytes& secretKey)
    {
        try {
            auto text = ovd::format_secret_key(slice(secretKey));
            auto result = QString::fromUtf8(text.data(), static_cast<int>(text.size()));
            Botan::secure_scrub_memory(const_cast<char*>(text.data()), text.size());
            return result;
        } catch (const rust::Error&) {
            return {};
        }
    }

    bool parseSecretKey(const QString& text, SecureBytes* secretKey)
    {
        auto utf8 = text.toUtf8();
        try {
            *secretKey = takeSecure(ovd::parse_secret_key(rust::Str(utf8.constData(), static_cast<size_t>(utf8.size()))));
            Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
            return true;
        } catch (const rust::Error&) {
            Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
            return false;
        }
    }

    KdfParams KdfParams::generate()
    {
        KdfParams kdf;
        kdf.salt = fromSecure(randomSecure(16));
        return kdf;
    }

    QJsonObject KdfParams::toJson() const
    {
        return QJsonObject{{"alg", "argon2id"},
                           {"salt", toBase64Url(salt)},
                           {"m", static_cast<qint64>(memoryKiB)},
                           {"t", static_cast<qint64>(iterations)},
                           {"p", static_cast<qint64>(parallelism)}};
    }

    KdfParams KdfParams::fromJson(const QJsonObject& json, bool* ok)
    {
        KdfParams kdf;
        bool saltOk = false;
        kdf.salt = fromBase64Url(json.value("salt").toString(), &saltOk);
        kdf.memoryKiB = static_cast<quint32>(json.value("m").toInteger());
        kdf.iterations = static_cast<quint32>(json.value("t").toInteger());
        kdf.parallelism = static_cast<quint32>(json.value("p").toInteger());
        if (ok) {
            *ok = json.value("alg").toString() == "argon2id" && saltOk && !kdf.salt.isEmpty() && kdf.memoryKiB > 0
                  && kdf.iterations > 0 && kdf.parallelism > 0;
        }
        return kdf;
    }

    bool KdfParams::isAcceptable() const
    {
        return memoryKiB >= MinMemoryKiB && iterations >= MinIterations && parallelism >= 1 && !salt.isEmpty();
    }

    SecureBytes normalizePassword(const QString& password)
    {
        auto utf8 = password.toUtf8();
        try {
            auto normalized = takeSecure(ovd::nfkc_utf8(slice(utf8)));
            Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
            return normalized;
        } catch (const rust::Error&) {
            Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
            return {};
        }
    }

    SecureBytes stretchPassword(const SecureBytes& normalizedPassword, const KdfParams& kdf)
    {
        try {
            return takeSecure(ov::argon2id(
                slice(normalizedPassword), slice(kdf.salt), kdf.memoryKiB, kdf.iterations, kdf.parallelism));
        } catch (const rust::Error&) {
            return {};
        }
    }

    SecureBytes deriveAuk(const SecureBytes& stretchedPassword, const SecureBytes& secretKey)
    {
        return hkdf(stretchedPassword, fromSecure(secretKey), AukInfo);
    }

    SecureBytes deriveAuk(const QString& password, const KdfParams& kdf, const SecureBytes& secretKey)
    {
        return deriveAuk(stretchPassword(normalizePassword(password), kdf), secretKey);
    }

    QByteArray
    wrap(const SecureBytes& key, const SecureBytes& plaintext, const QByteArray& aad, const QByteArray& nonce)
    {
        try {
            return toByteArray(ovd::wrap(slice(key), slice(plaintext), slice(aad), slice(nonce)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    bool unwrap(const SecureBytes& key,
                const QByteArray& wrapped,
                const QByteArray& aad,
                SecureBytes* plaintext,
                Error* error)
    {
        try {
            *plaintext = takeSecure(ovd::unwrap(slice(key), slice(wrapped), slice(aad)));
        } catch (const rust::Error& e) {
            return fail(error, wrapped.isEmpty() ? Error::InvalidInput : errorOf(e));
        }
        if (error) {
            *error = Error::None;
        }
        return true;
    }

    QJsonObject PublicKeys::toJson() const
    {
        return QJsonObject{{"enc_pk", toBase64Url(encPk)}, {"sign_pk", toBase64Url(signPk)}, {"sig", toBase64Url(sig)}};
    }

    PublicKeys PublicKeys::fromJson(const QJsonObject& json, bool* ok)
    {
        PublicKeys keys;
        bool encOk = false, signOk = false, sigOk = false;
        keys.encPk = fromBase64Url(json.value("enc_pk").toString(), &encOk);
        keys.signPk = fromBase64Url(json.value("sign_pk").toString(), &signOk);
        keys.sig = fromBase64Url(json.value("sig").toString(), &sigOk);
        if (ok) {
            *ok = encOk && signOk && sigOk && keys.encPk.size() == KeySize && keys.signPk.size() == KeySize
                  && keys.sig.size() == 64;
        }
        return keys;
    }

    void Keyset::clear()
    {
        // secure_vector wipes its memory when it is freed
        SecureBytes().swap(encSk);
        SecureBytes().swap(signSeed);
        publicKeys = {};
    }

    QByteArray x25519PublicKey(const SecureBytes& secretKey)
    {
        try {
            return toByteArray(ovd::x25519_public_key(slice(secretKey)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    QByteArray ed25519PublicKey(const SecureBytes& seed)
    {
        try {
            return toByteArray(ovd::ed25519_public_key(slice(seed)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    QByteArray sign(const SecureBytes& signSeed, const QByteArray& message)
    {
        try {
            return toByteArray(ovd::ed25519_sign(slice(signSeed), slice(message)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    bool verify(const QByteArray& signPk, const QByteArray& message, const QByteArray& signature)
    {
        return ovd::ed25519_verify(slice(signPk), slice(message), slice(signature));
    }

    QByteArray signPublicKeys(const SecureBytes& signSeed, const QByteArray& encPk, const QByteArray& signPk)
    {
        return sign(signSeed, PublicKeysContext + encPk + signPk);
    }

    bool verifyPublicKeys(const PublicKeys& publicKeys)
    {
        return ovd::verify_public_keys(slice(publicKeys.encPk), slice(publicKeys.signPk), slice(publicKeys.sig));
    }

    QString fingerprint(const PublicKeys& publicKeys)
    {
        const auto text = ovd::fingerprint(slice(publicKeys.encPk), slice(publicKeys.signPk));
        return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
    }

    Keyset keysetFromSecrets(const SecureBytes& encSk, const SecureBytes& signSeed)
    {
        Keyset keyset;
        try {
            const auto publicKeys = ovd::keyset_from_secrets(slice(encSk), slice(signSeed));
            keyset.encSk = encSk;
            keyset.signSeed = signSeed;
            keyset.publicKeys.encPk = toByteArray(publicKeys.enc_pk);
            keyset.publicKeys.signPk = toByteArray(publicKeys.sign_pk);
            keyset.publicKeys.sig = toByteArray(publicKeys.sig);
        } catch (const rust::Error&) {
        }
        return keyset;
    }

    Keyset generateKeyset()
    {
        return keysetFromSecrets(randomSecure(KeySize), randomSecure(KeySize));
    }

    QByteArray wrapKeyset(const SecureBytes& key, const Keyset& keyset, const QByteArray& nonce)
    {
        SecureBytes secrets(keyset.encSk);
        secrets.insert(secrets.end(), keyset.signSeed.begin(), keyset.signSeed.end());
        return wrap(key, secrets, KeysetAad, nonce);
    }

    QJsonObject wrappedKeysetJson(const SecureBytes& key, const Keyset& keyset)
    {
        auto json = keyset.publicKeys.toJson();
        json["wrapped"] = toBase64Url(wrapKeyset(key, keyset));
        return json;
    }

    bool unwrapKeyset(const SecureBytes& key,
                      const QByteArray& wrapped,
                      const PublicKeys& publicKeys,
                      Keyset* keyset,
                      Error* error)
    {
        SecureBytes secrets;
        if (!unwrap(key, wrapped, KeysetAad, &secrets, error)) {
            return false;
        }
        if (secrets.size() != 2 * KeySize) {
            if (error) {
                *error = Error::InvalidInput;
            }
            return false;
        }
        auto result = keysetFromSecrets(SecureBytes(secrets.begin(), secrets.begin() + KeySize),
                                        SecureBytes(secrets.begin() + KeySize, secrets.end()));
        // The server hands out the public keys too; they must be the ones these private keys make
        if (!constantTimeEquals(result.publicKeys.encPk, publicKeys.encPk)
            || !constantTimeEquals(result.publicKeys.signPk, publicKeys.signPk) || !verifyPublicKeys(publicKeys)) {
            if (error) {
                *error = Error::InvalidInput;
            }
            return false;
        }
        // Keep the server's signature: Ed25519 is deterministic, but it is the one others verify
        result.publicKeys.sig = publicKeys.sig;
        *keyset = result;
        return true;
    }

    bool unwrapKeyset(const SecureBytes& key, const QJsonObject& wrappedKeyset, Keyset* keyset, Error* error)
    {
        bool publicOk = false, wrappedOk = false;
        const auto publicKeys = PublicKeys::fromJson(wrappedKeyset, &publicOk);
        const auto wrapped = fromBase64Url(wrappedKeyset.value("wrapped").toString(), &wrappedOk);
        if (!publicOk || !wrappedOk) {
            if (error) {
                *error = Error::InvalidInput;
            }
            return false;
        }
        return unwrapKeyset(key, wrapped, publicKeys, keyset, error);
    }

    SecureBytes sharedSecret(const SecureBytes& secretKey, const QByteArray& publicKey)
    {
        try {
            return takeSecure(ovd::x25519_shared(slice(secretKey), slice(publicKey)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    SecureBytes sealKey(const SecureBytes& sharedSecret, const QByteArray& ephemeralPk, const QByteArray& recipientPk)
    {
        return hkdf(sharedSecret, ephemeralPk + recipientPk, SealInfo);
    }

    QByteArray seal(const QByteArray& recipientEncPk,
                    const SecureBytes& plaintext,
                    const QByteArray& aad,
                    const SecureBytes& ephemeralSk,
                    const QByteArray& nonce)
    {
        try {
            return toByteArray(
                ovd::seal(slice(recipientEncPk), slice(plaintext), slice(aad), slice(ephemeralSk), slice(nonce)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    bool unseal(const SecureBytes& recipientEncSk,
                const QByteArray& sealed,
                const QByteArray& aad,
                SecureBytes* plaintext,
                Error* error)
    {
        try {
            *plaintext = takeSecure(ovd::unseal(slice(recipientEncSk), slice(sealed), slice(aad)));
        } catch (const rust::Error& e) {
            return fail(error, sealed.isEmpty() ? Error::InvalidInput : errorOf(e));
        }
        if (error) {
            *error = Error::None;
        }
        return true;
    }

    SecureBytes generateVaultKey()
    {
        return randomSecure(KeySize);
    }

    SecureBytes vaultNameKey(const SecureBytes& vaultKey)
    {
        try {
            return takeSecure(ovd::vault_name_key(slice(vaultKey)));
        } catch (const rust::Error&) {
            return {};
        }
    }

    QByteArray
    encryptVaultName(const SecureBytes& vaultKey, const QString& vaultId, const QString& name, const QByteArray& nonce)
    {
        return wrap(vaultNameKey(vaultKey), toSecure(name.toUtf8()), vaultNameAad(vaultId), nonce);
    }

    bool
    decryptVaultName(const SecureBytes& vaultKey, const QString& vaultId, const QByteArray& encrypted, QString* name)
    {
        SecureBytes plaintext;
        if (!unwrap(vaultNameKey(vaultKey), encrypted, vaultNameAad(vaultId), &plaintext)) {
            return false;
        }
        *name = QString::fromUtf8(fromSecure(plaintext));
        return true;
    }

    QByteArray shareAad(const QString& shareId)
    {
        return "ov-share-v1|" + shareId.toUtf8();
    }

    QByteArray shareRefAad(const QString& shareId)
    {
        return "ov-share-ref|" + shareId.toUtf8();
    }

    SecureBytes padShare(const SecureBytes& data)
    {
        const auto size = (data.size() / SharePad + 1) * SharePad;
        SecureBytes padded(size, 0);
        std::copy(data.begin(), data.end(), padded.begin());
        padded[data.size()] = 0x80;
        return padded;
    }

    bool unpadShare(const SecureBytes& padded, SecureBytes* data)
    {
        auto end = padded.size();
        while (end > 0 && padded[end - 1] == 0) {
            --end;
        }
        if (end == 0 || padded[end - 1] != 0x80) {
            return false;
        }
        data->assign(padded.begin(), padded.begin() + static_cast<std::ptrdiff_t>(end - 1));
        return true;
    }

    SecureBytes sharePasscodeKey(const QString& passcode, const KdfParams& kdf)
    {
        return stretchPassword(normalizePassword(passcode), kdf);
    }

    SecureBytes shareContentKey(const SecureBytes& linkKey, const SecureBytes& passcodeKey)
    {
        return hkdf(linkKey, fromSecure(passcodeKey), "ov-share-v1");
    }

    SecureBytes shareAuth(const SecureBytes& passcodeKey)
    {
        return hkdf(passcodeKey, {}, "ov-share-auth-v1");
    }

    QByteArray encryptSharePayload(const SecureBytes& linkKey,
                                   const QString& shareId,
                                   const QByteArray& payloadJson,
                                   const SecureBytes& passcodeKey,
                                   const QByteArray& nonce)
    {
        auto ct = wrap(shareContentKey(linkKey, passcodeKey), padShare(toSecure(payloadJson)), shareAad(shareId), nonce);
        return ct.size() > ShareMaxCiphertext ? QByteArray() : ct;
    }

    bool decryptSharePayload(const SecureBytes& linkKey,
                             const QString& shareId,
                             const QByteArray& ciphertext,
                             QByteArray* payloadJson,
                             const SecureBytes& passcodeKey)
    {
        SecureBytes padded;
        SecureBytes plain;
        if (!unwrap(shareContentKey(linkKey, passcodeKey), ciphertext, shareAad(shareId), &padded)
            || !unpadShare(padded, &plain)) {
            return false;
        }
        *payloadJson = fromSecure(plain);
        return true;
    }

    SecureBytes shareRefKey(const SecureBytes& encSk)
    {
        return hkdf(encSk, {}, "ov-share-ref-v1");
    }

    QByteArray
    encryptShareRef(const SecureBytes& encSk, const QString& shareId, const QByteArray& refJson, const QByteArray& nonce)
    {
        return wrap(shareRefKey(encSk), toSecure(refJson), shareRefAad(shareId), nonce);
    }

    bool decryptShareRef(const SecureBytes& encSk, const QString& shareId, const QByteArray& ref, QByteArray* refJson)
    {
        SecureBytes plain;
        if (!unwrap(shareRefKey(encSk), ref, shareRefAad(shareId), &plain)) {
            return false;
        }
        *refJson = fromSecure(plain);
        return true;
    }

    QString shareLink(const QString& base, const QString& shareId, const SecureBytes& linkKey)
    {
        auto trimmed = base;
        while (trimmed.endsWith('/')) {
            trimmed.chop(1);
        }
        return trimmed + "/" + shareId + "#" + toBase64Url(linkKey);
    }

    QString refreshTokenHash(const QString& refreshToken)
    {
        const auto utf8 = refreshToken.toUtf8();
        const auto hash = ovd::refresh_token_hash(rust::Str(utf8.constData(), static_cast<size_t>(utf8.size())));
        return QString::fromUtf8(hash.data(), static_cast<int>(hash.size()));
    }

    QString ServiceAccountToken::secretHash() const
    {
        try {
            const auto hash = ovd::service_account_secret_hash(slice(secret));
            return QString::fromUtf8(hash.data(), static_cast<int>(hash.size()));
        } catch (const rust::Error&) {
            return {};
        }
    }

    QString ServiceAccountToken::secretPrefix() const
    {
        try {
            const auto prefix = ovd::service_account_secret_prefix(slice(secret));
            return QString::fromUtf8(prefix.data(), static_cast<int>(prefix.size()));
        } catch (const rust::Error&) {
            return {};
        }
    }

    QString buildServiceAccountToken(const ServiceAccountToken& token)
    {
        const auto url = token.url.toUtf8();
        const auto saId = token.saId.toUtf8();
        try {
            auto text = ovd::build_service_account_token(rust::Str(url.constData(), static_cast<size_t>(url.size())),
                                                         rust::Str(saId.constData(), static_cast<size_t>(saId.size())),
                                                         slice(token.secret),
                                                         slice(token.kek));
            auto result = QString::fromUtf8(text.data(), static_cast<int>(text.size()));
            Botan::secure_scrub_memory(const_cast<char*>(text.data()), text.size());
            return result;
        } catch (const rust::Error&) {
            return {};
        }
    }

    bool parseServiceAccountToken(const QString& text, ServiceAccountToken* token)
    {
        auto utf8 = text.toUtf8();
        try {
            auto parsed = ovd::parse_service_account_token(rust::Str(utf8.constData(), static_cast<size_t>(utf8.size())));
            Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
            ServiceAccountToken result;
            result.version = 1;
            result.url = QString::fromUtf8(parsed.url.data(), static_cast<int>(parsed.url.size()));
            result.saId = QString::fromUtf8(parsed.sa_id.data(), static_cast<int>(parsed.sa_id.size()));
            result.secret = takeSecure(std::move(parsed.secret));
            result.kek = takeSecure(std::move(parsed.kek));
            *token = result;
            return true;
        } catch (const rust::Error&) {
            Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
            return false;
        }
    }

    bool parseJwtClaims(const QString& token, JwtClaims* claims)
    {
        const auto parts = token.trimmed().split('.');
        if (parts.size() != 3) {
            return false;
        }
        bool headerOk = false, payloadOk = false;
        const auto header = decodeJsonPart(parts.at(0), &headerOk);
        const auto payload = decodeJsonPart(parts.at(1), &payloadOk);
        if (!headerOk || !payloadOk) {
            return false;
        }

        JwtClaims result;
        result.alg = header.value("alg").toString();
        result.kid = header.value("kid").toString();
        result.iss = payload.value("iss").toString();
        const auto aud = payload.value("aud");
        if (aud.isArray()) {
            for (const auto& value : aud.toArray()) {
                result.aud << value.toString();
            }
        } else if (aud.isString()) {
            result.aud << aud.toString();
        }
        result.sub = payload.value("sub").toString();
        result.dev = payload.value("dev").toString();
        result.org = payload.value("org").toString();
        for (const auto& value : payload.value("vlt").toArray()) {
            const auto vault = value.toObject();
            result.vaults.append({vault.value("id").toString(), vault.value("r").toString()});
        }
        result.iat = payload.value("iat").toInteger();
        result.nbf = payload.value("nbf").toInteger();
        result.exp = payload.value("exp").toInteger();
        result.jti = payload.value("jti").toString();
        *claims = result;
        return true;
    }
} // namespace AccountCrypto
