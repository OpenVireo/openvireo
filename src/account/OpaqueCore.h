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

#ifndef KEEPASSXC_OPAQUECORE_H
#define KEEPASSXC_OPAQUECORE_H

/*
 * The parts of libopaque's opaque.h (libopaque 6e9ac92) that KeePassXC uses. opaque.h itself pulls in
 * liboprf's toprf.h, whose variable length array parameters aren't valid C++, so the declarations are
 * repeated here. Only include this when KPXC_FEATURE_ACCOUNTS is on.
 */

#include <cstdint>
#include <sodium.h>

#define OPAQUE_SHARED_SECRETBYTES 64
#define OPAQUE_ENVELOPE_NONCEBYTES 32
#define OPAQUE_NONCE_BYTES 32

#define OPAQUE_REGISTRATION_RECORD_LEN                                                                                 \
    (crypto_scalarmult_BYTES + crypto_hash_sha512_BYTES + OPAQUE_ENVELOPE_NONCEBYTES + crypto_auth_hmacsha512_BYTES)
#define OPAQUE_USER_RECORD_LEN                                                                                         \
    (crypto_core_ristretto255_SCALARBYTES + crypto_scalarmult_SCALARBYTES + OPAQUE_REGISTRATION_RECORD_LEN)
#define OPAQUE_USER_SESSION_PUBLIC_LEN (crypto_core_ristretto255_BYTES + crypto_scalarmult_BYTES + OPAQUE_NONCE_BYTES)
#define OPAQUE_USER_SESSION_SECRET_LEN                                                                                 \
    (crypto_core_ristretto255_SCALARBYTES + crypto_scalarmult_SCALARBYTES + OPAQUE_NONCE_BYTES                         \
     + crypto_core_ristretto255_BYTES + OPAQUE_USER_SESSION_PUBLIC_LEN + sizeof(uint16_t))
#define OPAQUE_SERVER_SESSION_LEN                                                                                      \
    (crypto_core_ristretto255_BYTES + 32 + crypto_scalarmult_BYTES + OPAQUE_NONCE_BYTES + crypto_scalarmult_BYTES      \
     + crypto_auth_hmacsha512_BYTES + OPAQUE_ENVELOPE_NONCEBYTES + crypto_auth_hmacsha512_BYTES)
#define OPAQUE_REGISTER_USER_SEC_LEN (crypto_core_ristretto255_SCALARBYTES + sizeof(uint16_t))
#define OPAQUE_REGISTER_PUBLIC_LEN (crypto_core_ristretto255_BYTES + crypto_scalarmult_BYTES)
#define OPAQUE_REGISTER_SECRET_LEN (crypto_scalarmult_SCALARBYTES + crypto_core_ristretto255_SCALARBYTES)

extern "C" {
typedef struct
{
    uint16_t idU_len;
    uint8_t* idU;
    uint16_t idS_len;
    uint8_t* idS;
} Opaque_Ids;

// Client
int opaque_CreateRegistrationRequest(const uint8_t* pwdU, const uint16_t pwdU_len, uint8_t* sec, uint8_t* request);
int opaque_FinalizeRequest(const uint8_t* sec,
                           const uint8_t* pub,
                           const Opaque_Ids* ids,
                           uint8_t* reg_rec,
                           uint8_t* export_key);
int opaque_CreateCredentialRequest(const uint8_t* pwdU, const uint16_t pwdU_len, uint8_t* sec, uint8_t* ke1);
int opaque_RecoverCredentials(const uint8_t* resp,
                              const uint8_t* sec,
                              const uint8_t* ctx,
                              const uint16_t ctx_len,
                              const Opaque_Ids* ids,
                              uint8_t* sk,
                              uint8_t* authU,
                              uint8_t* export_key);

// Server, used by the tests to stand in for the auth server
int opaque_CreateRegistrationResponse(const uint8_t* request, const uint8_t* skS, uint8_t* sec, uint8_t* pub);
void opaque_StoreUserRecord(const uint8_t* sec, const uint8_t* recU, uint8_t* rec);
int opaque_CreateCredentialResponse(const uint8_t* ke1,
                                    const uint8_t* rec,
                                    const Opaque_Ids* ids,
                                    const uint8_t* ctx,
                                    const uint16_t ctx_len,
                                    uint8_t* ke2,
                                    uint8_t* sk,
                                    uint8_t* authU);
int opaque_UserAuth(const uint8_t* authU0, const uint8_t* authU);
}

#endif // KEEPASSXC_OPAQUECORE_H
