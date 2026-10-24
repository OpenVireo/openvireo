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

#include "OpaqueClient.h"

#include "config-keepassx.h"

#ifdef KPXC_FEATURE_ACCOUNTS
#include "OpaqueCore.h"
#endif

const QByteArray OpaqueClient::Context = QByteArrayLiteral("ov-opaque-v1");

namespace
{
#ifdef KPXC_FEATURE_ACCOUNTS
    bool initSodium()
    {
        static const bool ready = sodium_init() >= 0;
        return ready;
    }

    // libopaque takes the ids as mutable pointers but doesn't change them
    struct OpaqueIds
    {
        explicit OpaqueIds(const OpaqueClient::Ids& ids)
            : user(ids.user)
            , server(ids.server)
        {
            value.idU = reinterpret_cast<uint8_t*>(user.data());
            value.idU_len = static_cast<uint16_t>(user.size());
            value.idS = reinterpret_cast<uint8_t*>(server.data());
            value.idS_len = static_cast<uint16_t>(server.size());
        }

        QByteArray user;
        QByteArray server;
        Opaque_Ids value{};
    };

    const uint8_t* bytes(const QByteArray& data)
    {
        return reinterpret_cast<const uint8_t*>(data.constData());
    }

    QByteArray toByteArray(const uint8_t* data, size_t size)
    {
        return QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(size));
    }
#endif
} // namespace

bool OpaqueClient::isAvailable()
{
#ifdef KPXC_FEATURE_ACCOUNTS
    return initSodium();
#else
    return false;
#endif
}

QByteArray OpaqueClient::userId(const QString& email)
{
    return email.trimmed().toLower().toUtf8();
}

bool OpaqueClient::createRegistrationRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* request)
{
#ifdef KPXC_FEATURE_ACCOUNTS
    if (!isAvailable() || password.size() > 0xffff) {
        return false;
    }
    SecureBytes secret(OPAQUE_REGISTER_USER_SEC_LEN + password.size());
    uint8_t blinded[crypto_core_ristretto255_BYTES];
    if (opaque_CreateRegistrationRequest(
            password.data(), static_cast<uint16_t>(password.size()), secret.data(), blinded)
        != 0) {
        return false;
    }
    *sec = secret;
    *request = toByteArray(blinded, sizeof(blinded));
    return true;
#else
    Q_UNUSED(password)
    Q_UNUSED(sec)
    Q_UNUSED(request)
    return false;
#endif
}

bool OpaqueClient::finalizeRegistration(const SecureBytes& sec,
                                        const QByteArray& response,
                                        const Ids& ids,
                                        QByteArray* record)
{
#ifdef KPXC_FEATURE_ACCOUNTS
    if (!isAvailable() || response.size() != OPAQUE_REGISTER_PUBLIC_LEN || sec.size() < OPAQUE_REGISTER_USER_SEC_LEN) {
        return false;
    }
    OpaqueIds opaqueIds(ids);
    uint8_t result[OPAQUE_REGISTRATION_RECORD_LEN];
    SecureBytes exportKey(crypto_hash_sha512_BYTES);
    if (opaque_FinalizeRequest(sec.data(), bytes(response), &opaqueIds.value, result, exportKey.data()) != 0) {
        return false;
    }
    *record = toByteArray(result, sizeof(result));
    return true;
#else
    Q_UNUSED(sec)
    Q_UNUSED(response)
    Q_UNUSED(ids)
    Q_UNUSED(record)
    return false;
#endif
}

bool OpaqueClient::createCredentialRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* ke1)
{
#ifdef KPXC_FEATURE_ACCOUNTS
    if (!isAvailable() || password.size() > 0xffff) {
        return false;
    }
    SecureBytes secret(OPAQUE_USER_SESSION_SECRET_LEN + password.size());
    uint8_t request[OPAQUE_USER_SESSION_PUBLIC_LEN];
    if (opaque_CreateCredentialRequest(password.data(), static_cast<uint16_t>(password.size()), secret.data(), request)
        != 0) {
        return false;
    }
    *sec = secret;
    *ke1 = toByteArray(request, sizeof(request));
    return true;
#else
    Q_UNUSED(password)
    Q_UNUSED(sec)
    Q_UNUSED(ke1)
    return false;
#endif
}

bool OpaqueClient::recoverCredentials(const QByteArray& ke2,
                                      const SecureBytes& sec,
                                      const Ids& ids,
                                      QByteArray* authU,
                                      SecureBytes* sessionKey)
{
#ifdef KPXC_FEATURE_ACCOUNTS
    if (!isAvailable() || ke2.size() != OPAQUE_SERVER_SESSION_LEN || sec.size() < OPAQUE_USER_SESSION_SECRET_LEN) {
        return false;
    }
    OpaqueIds opaqueIds(ids);
    SecureBytes sk(OPAQUE_SHARED_SECRETBYTES);
    SecureBytes exportKey(crypto_hash_sha512_BYTES);
    uint8_t auth[crypto_auth_hmacsha512_BYTES];
    if (opaque_RecoverCredentials(bytes(ke2),
                                  sec.data(),
                                  bytes(Context),
                                  static_cast<uint16_t>(Context.size()),
                                  &opaqueIds.value,
                                  sk.data(),
                                  auth,
                                  exportKey.data())
        != 0) {
        return false;
    }
    *authU = toByteArray(auth, sizeof(auth));
    if (sessionKey) {
        *sessionKey = sk;
    }
    return true;
#else
    Q_UNUSED(ke2)
    Q_UNUSED(sec)
    Q_UNUSED(ids)
    Q_UNUSED(authU)
    Q_UNUSED(sessionKey)
    return false;
#endif
}
