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

#ifndef KEEPASSXC_ACCOUNTCRYPTO_H
#define KEEPASSXC_ACCOUNTCRYPTO_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <botan/secmem.h>

/**
 * The crypto of OpenVireo accounts (ov v1), as specified in docs/plans/accounts/SPEC.md and pinned by
 * docs/plans/accounts/test-vectors.json. Every secret (Secret Key, AUK, private keys, vault keys) is kept
 * in a SecureBytes, which wipes itself when it is freed.
 *
 * Functions that encrypt take an optional nonce (and ephemeral key) so that the test vectors can be
 * reproduced; leave them empty to get fresh random ones, which is the only correct use outside tests.
 */
namespace AccountCrypto
{
    using SecureBytes = Botan::secure_vector<uint8_t>;

    enum class Error
    {
        None,
        // The envelope starts with a version byte other than 0x01
        UnsupportedVersion,
        // Wrong key or AAD, or the data was changed
        DecryptionFailed,
        InvalidInput
    };

    constexpr int KeySize = 32;
    constexpr int SecretKeySize = 16;
    constexpr int NonceSize = 24;
    constexpr char EnvelopeVersion = 0x01;

    // AAD and info strings (SPEC §4-§6)
    extern const QByteArray KeysetAad;
    QByteArray vaultKeyAad(const QString& vaultId);
    QByteArray saMasterAad(const QString& saId);
    QByteArray vaultNameAad(const QString& vaultId);

    // Base64url without padding; decoding also accepts padding (SPEC §1)
    QString toBase64Url(const QByteArray& data);
    QString toBase64Url(const SecureBytes& data);
    QByteArray fromBase64Url(const QString& text, bool* ok = nullptr);
    SecureBytes secureFromBase64Url(const QString& text, bool* ok = nullptr);

    SecureBytes toSecure(const QByteArray& data);
    QByteArray fromSecure(const SecureBytes& data);
    SecureBytes randomSecure(int size);
    QByteArray sha256(const QByteArray& data);
    QString toHex(const QByteArray& data);

    // HKDF-SHA256 (RFC 5869); an empty salt means HashLen zero bytes
    SecureBytes hkdf(const SecureBytes& ikm, const QByteArray& salt, const QByteArray& info, int length = KeySize);

    // §2 Secret Key
    SecureBytes generateSecretKey();
    // K1-XXXXXX-XXXXX-XXXXX-XXXXX-XXXXX
    QString formatSecretKey(const SecureBytes& secretKey);
    bool parseSecretKey(const QString& text, SecureBytes* secretKey);

    // §3 Account Unlock Key
    struct KdfParams
    {
        static constexpr quint32 DefaultMemoryKiB = 65536;
        static constexpr quint32 DefaultIterations = 3;
        static constexpr quint32 MinMemoryKiB = 19456;
        static constexpr quint32 MinIterations = 2;

        QByteArray salt;
        quint32 memoryKiB = DefaultMemoryKiB;
        quint32 iterations = DefaultIterations;
        quint32 parallelism = 1;

        // A fresh random salt with the production defaults
        static KdfParams generate();
        QJsonObject toJson() const;
        static KdfParams fromJson(const QJsonObject& json, bool* ok = nullptr);
        // Clients refuse parameters below the minimum, which a malicious server could send to speed up guessing
        bool isAcceptable() const;
    };

    // UTF-8 of the NFKC normalized password, not trimmed
    SecureBytes normalizePassword(const QString& password);
    // Argon2id(pw, salt, m, t, p), version 0x13, 32 bytes
    SecureBytes stretchPassword(const SecureBytes& normalizedPassword, const KdfParams& kdf);
    SecureBytes deriveAuk(const QString& password, const KdfParams& kdf, const SecureBytes& secretKey);
    SecureBytes deriveAuk(const SecureBytes& stretchedPassword, const SecureBytes& secretKey);

    // §4 wrap(key, plaintext, aad) = 0x01 || nonce(24) || XChaCha20-Poly1305(key, nonce, plaintext, aad)
    QByteArray
    wrap(const SecureBytes& key, const SecureBytes& plaintext, const QByteArray& aad, const QByteArray& nonce = {});
    bool unwrap(const SecureBytes& key,
                const QByteArray& wrapped,
                const QByteArray& aad,
                SecureBytes* plaintext,
                Error* error = nullptr);

    // §5 Keysets
    struct PublicKeys
    {
        QByteArray encPk;
        QByteArray signPk;
        QByteArray sig;

        bool isNull() const
        {
            return encPk.isEmpty();
        }
        QJsonObject toJson() const;
        static PublicKeys fromJson(const QJsonObject& json, bool* ok = nullptr);
    };

    struct Keyset
    {
        SecureBytes encSk;
        SecureBytes signSeed;
        PublicKeys publicKeys;

        bool isNull() const
        {
            return encSk.empty();
        }
        void clear();
    };

    Keyset generateKeyset();
    // Derives the public keys and their self-signature from the private halves
    Keyset keysetFromSecrets(const SecureBytes& encSk, const SecureBytes& signSeed);
    QByteArray x25519PublicKey(const SecureBytes& secretKey);
    QByteArray ed25519PublicKey(const SecureBytes& seed);
    QByteArray sign(const SecureBytes& signSeed, const QByteArray& message);
    bool verify(const QByteArray& signPk, const QByteArray& message, const QByteArray& signature);
    // sig = Ed25519.sign(sign_seed, "ov-pubkeys-v1" || enc_pk || sign_pk)
    QByteArray signPublicKeys(const SecureBytes& signSeed, const QByteArray& encPk, const QByteArray& signPk);
    bool verifyPublicKeys(const PublicKeys& publicKeys);
    // The first 20 bytes of SHA-256("ov-fp-v1" || enc_pk || sign_pk), hex in groups of 4
    QString fingerprint(const PublicKeys& publicKeys);

    // wrap(key, enc_sk || sign_seed, "ov-keyset-v1"); {enc_pk, sign_pk, sig, wrapped}
    QByteArray wrapKeyset(const SecureBytes& key, const Keyset& keyset, const QByteArray& nonce = {});
    QJsonObject wrappedKeysetJson(const SecureBytes& key, const Keyset& keyset);
    // Unwraps and checks that the private keys belong to the given public keys and that those are self-signed
    bool unwrapKeyset(const SecureBytes& key,
                      const QByteArray& wrapped,
                      const PublicKeys& publicKeys,
                      Keyset* keyset,
                      Error* error = nullptr);
    bool unwrapKeyset(const SecureBytes& key, const QJsonObject& wrappedKeyset, Keyset* keyset, Error* error = nullptr);

    // §6 seal = 0x01 || epk(32) || nonce(24) || XChaCha20-Poly1305(HKDF(X25519(esk, pk), epk || pk, "ov-seal-v1"),
    // ...)
    QByteArray seal(const QByteArray& recipientEncPk,
                    const SecureBytes& plaintext,
                    const QByteArray& aad,
                    const SecureBytes& ephemeralSk = {},
                    const QByteArray& nonce = {});
    bool unseal(const SecureBytes& recipientEncSk,
                const QByteArray& sealed,
                const QByteArray& aad,
                SecureBytes* plaintext,
                Error* error = nullptr);
    // X25519(sk, pk); empty for an all-zero result (a low-order public key)
    SecureBytes sharedSecret(const SecureBytes& secretKey, const QByteArray& publicKey);
    // The symmetric key a seal uses, for the test vectors
    SecureBytes sealKey(const SecureBytes& sharedSecret, const QByteArray& ephemeralPk, const QByteArray& recipientPk);

    // §7 Vaults
    SecureBytes generateVaultKey();
    SecureBytes vaultNameKey(const SecureBytes& vaultKey);
    QByteArray encryptVaultName(const SecureBytes& vaultKey,
                                const QString& vaultId,
                                const QString& name,
                                const QByteArray& nonce = {});
    bool
    decryptVaultName(const SecureBytes& vaultKey, const QString& vaultId, const QByteArray& encrypted, QString* name);

    // §9 Tokens
    // SHA-256 hex of the token's UTF-8, which is what the server stores
    QString refreshTokenHash(const QString& refreshToken);

    struct ServiceAccountToken
    {
        int version = 1;
        QString url;
        QString saId;
        SecureBytes secret;
        SecureBytes kek;

        // What the server stores: SHA-256 hex of the 32 secret bytes, and the first 8 characters of its base64url
        QString secretHash() const;
        QString secretPrefix() const;
    };
    QString buildServiceAccountToken(const ServiceAccountToken& token);
    bool parseServiceAccountToken(const QString& text, ServiceAccountToken* token);

    // The claims of an access token, read without checking the signature: clients only use them to know
    // when to refresh and which vaults the token covers. The servers verify tokens.
    struct JwtClaims
    {
        struct VaultClaim
        {
            QString id;
            QString role;
        };
        QString iss;
        QStringList aud;
        QString sub;
        QString dev;
        QString org;
        QList<VaultClaim> vaults;
        qint64 iat = 0;
        qint64 nbf = 0;
        qint64 exp = 0;
        QString jti;
        QString alg;
        QString kid;
    };
    bool parseJwtClaims(const QString& token, JwtClaims* claims);
} // namespace AccountCrypto

#endif // KEEPASSXC_ACCOUNTCRYPTO_H
