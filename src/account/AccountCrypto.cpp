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

#include "AccountCrypto.h"

#include "config-keepassx.h"
#include "crypto/Random.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <botan/aead.h>
#include <botan/ed25519.h>
#include <botan/mac.h>
#include <botan/mem_ops.h>
#include <botan/pubkey.h>
#include <botan/pwdhash.h>
#include <botan/x25519.h>

namespace AccountCrypto
{
    const QByteArray KeysetAad = QByteArrayLiteral("ov-keyset-v1");

    namespace
    {
        const QByteArray PublicKeysContext = QByteArrayLiteral("ov-pubkeys-v1");
        const QByteArray FingerprintContext = QByteArrayLiteral("ov-fp-v1");
        const QByteArray SealInfo = QByteArrayLiteral("ov-seal-v1");
        const QByteArray AukInfo = QByteArrayLiteral("ov-auk-v1");
        const QByteArray VaultNameInfo = QByteArrayLiteral("ov-vault-name-v1");
        const QString SecretKeyPrefix = QStringLiteral("K1");
        const char CrockfordAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        const QString ServiceAccountPrefix = QStringLiteral("ov_sa_");

        const uint8_t* bytes(const QByteArray& data)
        {
            return reinterpret_cast<const uint8_t*>(data.constData());
        }

        QByteArray toByteArray(const std::vector<uint8_t>& data)
        {
            return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
        }

        QByteArray randomBytes(int size)
        {
            return randomGen()->randomArray(size);
        }

        // XChaCha20-Poly1305 (the 24-byte nonce selects the X variant in Botan's ChaCha20Poly1305)
        bool
        aead(bool encrypt, const SecureBytes& key, const QByteArray& nonce, const QByteArray& aad, SecureBytes& buffer)
        {
            try {
#ifdef WITH_BOTAN3
                const auto direction = encrypt ? Botan::Cipher_Dir::Encryption : Botan::Cipher_Dir::Decryption;
#else
                const auto direction = encrypt ? Botan::ENCRYPTION : Botan::DECRYPTION;
#endif
                auto cipher = Botan::AEAD_Mode::create_or_throw("ChaCha20Poly1305", direction);
                cipher->set_key(key.data(), key.size());
                cipher->set_associated_data(bytes(aad), static_cast<size_t>(aad.size()));
                cipher->start(bytes(nonce), static_cast<size_t>(nonce.size()));
                cipher->finish(buffer);
                return true;
            } catch (std::exception&) {
                return false;
            }
        }

        // The X25519 shared secret, empty when it is all zeros (a low-order public key)
        SecureBytes x25519(const SecureBytes& secretKey, const QByteArray& publicKey)
        {
            if (secretKey.size() != KeySize || publicKey.size() != KeySize) {
                return {};
            }
            try {
                Botan::X25519_PrivateKey key(secretKey);
                Botan::PK_Key_Agreement agreement(key, *randomGen()->getRng(), "Raw");
                auto shared =
                    agreement.derive_key(0, bytes(publicKey), static_cast<size_t>(publicKey.size())).bits_of();
                SecureBytes zeros(shared.size(), 0);
                if (shared.size() != KeySize
                    || Botan::constant_time_compare(shared.data(), zeros.data(), shared.size())) {
                    return {};
                }
                return shared;
            } catch (std::exception&) {
                return {};
            }
        }

        bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
        {
            return a.size() == b.size()
                   && Botan::constant_time_compare(bytes(a), bytes(b), static_cast<size_t>(a.size()));
        }

        QString jsonString(const QString& value)
        {
            // A one-element array is the shortest way to get Qt to escape a string as JSON
            auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
            return QString::fromUtf8(json.mid(1, json.size() - 2));
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
        return randomGen()->getRng()->random_vec(static_cast<size_t>(size));
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
        auto hmac = Botan::MessageAuthenticationCode::create_or_throw("HMAC(SHA-256)");
        const auto hashLength = static_cast<int>(hmac->output_length());
        // Extract
        const QByteArray actualSalt = salt.isEmpty() ? QByteArray(hashLength, '\0') : salt;
        hmac->set_key(bytes(actualSalt), static_cast<size_t>(actualSalt.size()));
        hmac->update(ikm.data(), ikm.size());
        const auto prk = hmac->final();
        // Expand
        hmac->set_key(prk.data(), prk.size());
        SecureBytes okm;
        SecureBytes block;
        for (uint8_t counter = 1; static_cast<int>(okm.size()) < length; ++counter) {
            hmac->update(block.data(), block.size());
            hmac->update(bytes(info), static_cast<size_t>(info.size()));
            hmac->update(counter);
            block = hmac->final();
            okm.insert(okm.end(), block.begin(), block.end());
        }
        okm.resize(static_cast<size_t>(length));
        return okm;
    }

    SecureBytes generateSecretKey()
    {
        return randomSecure(SecretKeySize);
    }

    QString formatSecretKey(const SecureBytes& secretKey)
    {
        if (secretKey.size() != SecretKeySize) {
            return {};
        }
        // 128 bits big-endian, padded with two zero bits to 26 characters of 5 bits
        QString digits;
        quint32 buffer = 0;
        int bits = 0;
        for (auto byte : secretKey) {
            buffer = (buffer << 8) | byte;
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                digits += QLatin1Char(CrockfordAlphabet[(buffer >> bits) & 0x1f]);
            }
        }
        if (bits > 0) {
            digits += QLatin1Char(CrockfordAlphabet[(buffer << (5 - bits)) & 0x1f]);
        }
        return QStringLiteral("%1-%2-%3-%4-%5-%6")
            .arg(SecretKeyPrefix,
                 digits.mid(0, 6),
                 digits.mid(6, 5),
                 digits.mid(11, 5),
                 digits.mid(16, 5),
                 digits.mid(21, 5));
    }

    bool parseSecretKey(const QString& text, SecureBytes* secretKey)
    {
        QString normalized;
        for (auto c : text.toUpper()) {
            if (c.isSpace() || c == '-') {
                continue;
            }
            if (c == 'O') {
                c = '0';
            } else if (c == 'I' || c == 'L') {
                c = '1';
            }
            normalized += c;
        }
        if (!normalized.startsWith(SecretKeyPrefix) || normalized.size() != SecretKeyPrefix.size() + 26) {
            return false;
        }

        SecureBytes result;
        quint32 buffer = 0;
        int bits = 0;
        for (const auto c : normalized.mid(SecretKeyPrefix.size())) {
            const char* found = c.unicode() < 128 ? std::strchr(CrockfordAlphabet, c.toLatin1()) : nullptr;
            if (!found || c.toLatin1() == '\0') {
                return false;
            }
            buffer = (buffer << 5) | static_cast<quint32>(found - CrockfordAlphabet);
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                result.push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
            }
        }
        // Two bits are left over, the padding, which must be zero
        if (bits != 2 || (buffer & 0x3) != 0 || result.size() != SecretKeySize) {
            return false;
        }
        *secretKey = result;
        return true;
    }

    KdfParams KdfParams::generate()
    {
        KdfParams kdf;
        kdf.salt = randomBytes(16);
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
        auto utf8 = password.normalized(QString::NormalizationForm_KC).toUtf8();
        auto secure = toSecure(utf8);
        Botan::secure_scrub_memory(utf8.data(), static_cast<size_t>(utf8.size()));
        return secure;
    }

    SecureBytes stretchPassword(const SecureBytes& normalizedPassword, const KdfParams& kdf)
    {
        auto family = Botan::PasswordHashFamily::create_or_throw("Argon2id");
        auto argon2 = family->from_params(kdf.memoryKiB, kdf.iterations, kdf.parallelism);
        SecureBytes out(KeySize);
        argon2->derive_key(out.data(),
                           out.size(),
                           reinterpret_cast<const char*>(normalizedPassword.data()),
                           normalizedPassword.size(),
                           bytes(kdf.salt),
                           static_cast<size_t>(kdf.salt.size()));
        return out;
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
        const auto actualNonce = nonce.isEmpty() ? randomBytes(NonceSize) : nonce;
        if (key.size() != KeySize || actualNonce.size() != NonceSize) {
            return {};
        }
        SecureBytes buffer(plaintext);
        if (!aead(true, key, actualNonce, aad, buffer)) {
            return {};
        }
        return QByteArray(1, EnvelopeVersion) + actualNonce + fromSecure(buffer);
    }

    bool unwrap(const SecureBytes& key,
                const QByteArray& wrapped,
                const QByteArray& aad,
                SecureBytes* plaintext,
                Error* error)
    {
        const auto fail = [error](Error reason) {
            if (error) {
                *error = reason;
            }
            return false;
        };
        if (wrapped.isEmpty()) {
            return fail(Error::InvalidInput);
        }
        if (wrapped.at(0) != EnvelopeVersion) {
            return fail(Error::UnsupportedVersion);
        }
        if (key.size() != KeySize || wrapped.size() < 1 + NonceSize + 16) {
            return fail(Error::InvalidInput);
        }
        auto buffer = toSecure(wrapped.mid(1 + NonceSize));
        if (!aead(false, key, wrapped.mid(1, NonceSize), aad, buffer)) {
            return fail(Error::DecryptionFailed);
        }
        *plaintext = buffer;
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
            Botan::X25519_PrivateKey key(secretKey);
            return toByteArray(key.public_value());
        } catch (std::exception&) {
            return {};
        }
    }

    namespace
    {
        std::unique_ptr<Botan::Ed25519_PrivateKey> ed25519Key(const SecureBytes& seed)
        {
#ifdef WITH_BOTAN3
            return std::make_unique<Botan::Ed25519_PrivateKey>(Botan::Ed25519_PrivateKey::from_seed(seed));
#else
            return std::make_unique<Botan::Ed25519_PrivateKey>(seed);
#endif
        }
    } // namespace

    QByteArray ed25519PublicKey(const SecureBytes& seed)
    {
        try {
            return toByteArray(ed25519Key(seed)->get_public_key());
        } catch (std::exception&) {
            return {};
        }
    }

    QByteArray sign(const SecureBytes& signSeed, const QByteArray& message)
    {
        try {
            auto key = ed25519Key(signSeed);
            Botan::PK_Signer signer(*key, *randomGen()->getRng(), "Pure");
            return toByteArray(
                signer.sign_message(bytes(message), static_cast<size_t>(message.size()), *randomGen()->getRng()));
        } catch (std::exception&) {
            return {};
        }
    }

    bool verify(const QByteArray& signPk, const QByteArray& message, const QByteArray& signature)
    {
        if (signPk.size() != KeySize || signature.size() != 64) {
            return false;
        }
        try {
            Botan::Ed25519_PublicKey key(std::vector<uint8_t>(signPk.begin(), signPk.end()));
            Botan::PK_Verifier verifier(key, "Pure");
            return verifier.verify_message(bytes(message),
                                           static_cast<size_t>(message.size()),
                                           bytes(signature),
                                           static_cast<size_t>(signature.size()));
        } catch (std::exception&) {
            return false;
        }
    }

    QByteArray signPublicKeys(const SecureBytes& signSeed, const QByteArray& encPk, const QByteArray& signPk)
    {
        return sign(signSeed, PublicKeysContext + encPk + signPk);
    }

    bool verifyPublicKeys(const PublicKeys& publicKeys)
    {
        return publicKeys.encPk.size() == KeySize
               && verify(publicKeys.signPk, PublicKeysContext + publicKeys.encPk + publicKeys.signPk, publicKeys.sig);
    }

    QString fingerprint(const PublicKeys& publicKeys)
    {
        const auto hex = toHex(sha256(FingerprintContext + publicKeys.encPk + publicKeys.signPk).left(20));
        QStringList groups;
        for (int i = 0; i < hex.size(); i += 4) {
            groups << hex.mid(i, 4);
        }
        return groups.join(' ');
    }

    Keyset keysetFromSecrets(const SecureBytes& encSk, const SecureBytes& signSeed)
    {
        Keyset keyset;
        if (encSk.size() != KeySize || signSeed.size() != KeySize) {
            return keyset;
        }
        keyset.encSk = encSk;
        keyset.signSeed = signSeed;
        keyset.publicKeys.encPk = x25519PublicKey(encSk);
        keyset.publicKeys.signPk = ed25519PublicKey(signSeed);
        keyset.publicKeys.sig = signPublicKeys(signSeed, keyset.publicKeys.encPk, keyset.publicKeys.signPk);
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
        return x25519(secretKey, publicKey);
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
        const auto esk = ephemeralSk.empty() ? randomSecure(KeySize) : ephemeralSk;
        const auto epk = x25519PublicKey(esk);
        const auto shared = x25519(esk, recipientEncPk);
        if (shared.empty() || epk.size() != KeySize) {
            return {};
        }
        const auto key = sealKey(shared, epk, recipientEncPk);
        const auto wrapped = wrap(key, plaintext, aad, nonce);
        if (wrapped.isEmpty()) {
            return {};
        }
        // 0x01 || epk || nonce || ciphertext
        return QByteArray(1, EnvelopeVersion) + epk + wrapped.mid(1);
    }

    bool unseal(const SecureBytes& recipientEncSk,
                const QByteArray& sealed,
                const QByteArray& aad,
                SecureBytes* plaintext,
                Error* error)
    {
        const auto fail = [error](Error reason) {
            if (error) {
                *error = reason;
            }
            return false;
        };
        if (sealed.isEmpty()) {
            return fail(Error::InvalidInput);
        }
        if (sealed.at(0) != EnvelopeVersion) {
            return fail(Error::UnsupportedVersion);
        }
        if (sealed.size() < 1 + KeySize + NonceSize + 16) {
            return fail(Error::InvalidInput);
        }
        const auto epk = sealed.mid(1, KeySize);
        const auto recipientPk = x25519PublicKey(recipientEncSk);
        const auto shared = x25519(recipientEncSk, epk);
        if (shared.empty() || recipientPk.isEmpty()) {
            return fail(Error::DecryptionFailed);
        }
        const auto key = sealKey(shared, epk, recipientPk);
        return unwrap(key, QByteArray(1, EnvelopeVersion) + sealed.mid(1 + KeySize), aad, plaintext, error);
    }

    SecureBytes generateVaultKey()
    {
        return randomSecure(KeySize);
    }

    SecureBytes vaultNameKey(const SecureBytes& vaultKey)
    {
        return hkdf(vaultKey, {}, VaultNameInfo);
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

    QString refreshTokenHash(const QString& refreshToken)
    {
        return toHex(sha256(refreshToken.toUtf8()));
    }

    QString ServiceAccountToken::secretHash() const
    {
        return toHex(sha256(fromSecure(secret)));
    }

    QString ServiceAccountToken::secretPrefix() const
    {
        return toBase64Url(secret).left(8);
    }

    QString buildServiceAccountToken(const ServiceAccountToken& token)
    {
        // Written by hand to keep the key order of the spec; QJsonDocument would sort the keys
        const auto json = QStringLiteral(R"({"v":%1,"url":%2,"sa_id":%3,"secret":%4,"kek":%5})")
                              .arg(token.version)
                              .arg(jsonString(token.url),
                                   jsonString(token.saId),
                                   jsonString(toBase64Url(token.secret)),
                                   jsonString(toBase64Url(token.kek)));
        return ServiceAccountPrefix + toBase64Url(json.toUtf8());
    }

    bool parseServiceAccountToken(const QString& text, ServiceAccountToken* token)
    {
        const auto trimmed = text.trimmed();
        if (!trimmed.startsWith(ServiceAccountPrefix)) {
            return false;
        }
        bool ok = false;
        const auto json = decodeJsonPart(trimmed.mid(ServiceAccountPrefix.size()), &ok);
        if (!ok || json.value("v").toInt() != 1) {
            return false;
        }
        ServiceAccountToken result;
        result.version = 1;
        result.url = json.value("url").toString();
        result.saId = json.value("sa_id").toString();
        bool secretOk = false, kekOk = false;
        result.secret = secureFromBase64Url(json.value("secret").toString(), &secretOk);
        result.kek = secureFromBase64Url(json.value("kek").toString(), &kekOk);
        if (result.url.isEmpty() || result.saId.isEmpty() || !secretOk || !kekOk || result.secret.size() != KeySize
            || result.kek.size() != KeySize) {
            return false;
        }
        *token = result;
        return true;
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
