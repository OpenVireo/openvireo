# KeePassXC accounts: crypto and protocol spec (kpx v1)

This is the shared contract for the Flask auth server (`keepassxc-auth`), the sync server
(`keepassxc-sync-server`), the Qt desktop (`keepassxc`), and the TS SDK (`kpnx-sdk`). The plan
that explains why is in `../accounts-and-sharing.md`. The HTTP API is in `openapi.yaml`.

Every implementation must pass `test-vectors.json` and open `vault-fixture.kdbx`. If you think the
spec is wrong, report it; don't change one implementation on its own.

## 1. Conventions

- **Binary in JSON and tokens** is base64url **without padding** (RFC 4648 §5). Decoders must also
  accept padded input. Fields whose names end in `_hex` in the test vectors are lowercase hex.
- **Strings** are UTF-8. AAD and info strings are ASCII and are used byte for byte as written here.
- **Ids** are lowercase hyphenated UUIDv4 strings. A principal is `user:<uuid>` or `sa:<uuid>`.
- **HKDF** is HKDF-SHA256 (RFC 5869). An empty salt means 32 zero bytes, as in the RFC.
- **AEAD** is XChaCha20-Poly1305 (24-byte nonce, 16-byte tag), the libsodium
  `crypto_aead_xchacha20poly1305_ietf` construction. Nonces are random for every encryption.
- **Constant time:** compare MACs, hashes and tokens with constant-time functions.
- **Zeroing:** clients clear private keys, the AUK and vault keys from memory when they lock
  (Qt: `Botan::secure_vector`/`SecureVector`; TS: `.fill(0)` once done, best effort).

## 2. Secret Key

- 16 random bytes, created on the client when the account is registered. The server never sees it.
- Display form: `K1-` + Crockford base32 of the 128 bits big-endian, padded with two zero bits to
  26 characters, grouped 6-5-5-5-5: `K1-XXXXXX-XXXXX-XXXXX-XXXXX-XXXXX`.
  Alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ`.
- Parsing: remove whitespace and `-`, uppercase, map `O→0`, `I→1`, `L→1`, require the `K1` prefix
  and 26 characters, and reject the input if the two padding bits are not zero.
- Stored on trusted devices in the OS keychain (Qt `SyncTokenStore`; web: IndexedDB, wrapped with a
  non-extractable WebCrypto AES-GCM key) and printed in the Emergency Kit.

## 3. Account Unlock Key (AUK)

```
pw         = UTF-8(NFKC(master password))          # not trimmed
stretched  = Argon2id(pw, kdf.salt, m=kdf.m KiB, t=kdf.t, p=kdf.p, version 0x13, len 32)
AUK        = HKDF(ikm=stretched, salt=SecretKey(16 bytes), info="kpx-auk-v1", L=32)
```

- `kdf` = `{"alg":"argon2id","salt":<16 bytes>,"m":65536,"t":3,"p":1}`. These are the production
  defaults. The client picks a random salt at registration, and the server stores the object as
  given and returns it after a successful OPAQUE login. Clients refuse `m < 19456` or `t < 2`.
- The OPAQUE password input (§8) is the same NFKC UTF-8 password. OPAQUE and the AUK are
  independent: a stolen OPAQUE record still needs the Secret Key to decrypt anything.

## 4. Wrap (symmetric envelope)

```
wrap(key, plaintext, aad) = 0x01 || nonce(24) || XChaCha20-Poly1305(key, nonce, plaintext, aad)
```

A first byte other than `0x01` is an "unsupported version" error.

| Use | Key | Plaintext | AAD |
|---|---|---|---|
| User keyset | AUK | `enc_sk(32) ‖ sign_seed(32)` | `kpx-keyset-v1` |
| SA keyset | SA master key | `enc_sk(32) ‖ sign_seed(32)` | `kpx-keyset-v1` |
| SA master key, per secret | `kek` from the SA token | master key (32) | `kpx-sa-master-v1|<sa uuid>` |
| Vault name | `HKDF(vault_key, salt="", info="kpx-vault-name-v1")` | UTF-8 name | `kpx-vault-name|<vault uuid>` |

## 5. Keysets and public keys

- Each principal has an **X25519** encryption keypair and an **Ed25519** signing keypair (the
  32-byte seed is stored). The public part is
  `{"enc_pk","sign_pk","sig"}`, where `sig = Ed25519.sign(sign_seed, "kpx-pubkeys-v1" ‖ enc_pk ‖ sign_pk)`.
  Every client checks `sig` before sealing anything to a public key.
- **Fingerprint** (shown in share dialogs, Phase 6 verification): the first 20 bytes of
  `SHA-256("kpx-fp-v1" ‖ enc_pk ‖ sign_pk)` as lowercase hex in groups of 4 separated by spaces.
- Clients keep the decrypted keyset only in memory while the account is unlocked.

## 6. Seal (public-key envelope, "kpx-seal-v1")

```
esk, epk  = fresh X25519 keypair
shared    = X25519(esk, recipient_enc_pk)       # reject an all-zero result
key       = HKDF(ikm=shared, salt=epk ‖ recipient_enc_pk, info="kpx-seal-v1", L=32)
sealed    = 0x01 || epk(32) || nonce(24) || XChaCha20-Poly1305(key, nonce, plaintext, aad)
```

| Use | Plaintext | AAD |
|---|---|---|
| Vault key for a member | vault key (32) | `kpx-vault-key|<vault uuid>` |
| SA master key for an org owner/admin | SA master key (32) | `kpx-sa-master-v1|<sa uuid>` |

The AAD binds a sealed key to its vault, so the server can't move one between vaults.

## 7. Vaults

- A vault is one KDBX 4 file on the sync server, stored at `vaults/<vault uuid>`.
- **Vault key:** 32 random bytes. The KDBX composite key is the vault key used as a **raw 32-byte
  key file component, with no password component**. Qt: `FileKey` loaded from an in-memory
  `QBuffer` of exactly those 32 bytes (don't show the CLI's "old key file format" warning for
  account vaults). kdbxweb: `new Credentials(null, vaultKey.slice())`. kdbxweb wipes the buffer
  it is given, so pass a copy.
- **KDF of vault files:** Argon2id, m = 1 MiB, t = 2, p = 1. The key has full entropy, so a costly
  KDF adds nothing. `vault-fixture.kdbx` uses these settings.
- **Vault names** are encrypted (§4) so that the auth server never learns them. The KDBX root group
  name is kept the same as the vault name.
- Clients read the sync server's ETag, merge and history behavior unchanged from `feature/auto-sync`.

### Key versions and rotation

`vault_access` rows carry a `key_version`. A vault has one `current_key_version`, plus at most one
pending version during a rotation. Rotating is required when a member is removed and optional at
any other time:

1. The client (needs `manage`) creates a new vault key and seals it to every remaining member.
   `POST /api/v1/vaults/{id}/keys` with `{key_version: current+1, grants:[…], remove:[principals]}`.
   The server removes the listed principals' access **immediately**, stores the new grants as
   pending, and notifies the affected principals (§11).
2. The client downloads the latest blob (GET with the ETag), re-keys it with the new vault key, and
   uploads it with a conditional PUT (`If-Match`). On 412 it merges and retries, as today.
3. `POST /api/v1/vaults/{id}/keys/{version}/activate`: the server makes the pending version current
   and deletes the older sealed keys.

`GET /api/v1/vaults` returns every sealed key the caller holds for the current version and the
pending one. Clients try the newest version first and fall back to the older one, so a crash
between steps 2 and 3 never locks anyone out. A pending version older than 24 hours can be
activated or abandoned by any `manage` member. Phase 6: the server rejects an access change that
removes a member without a new key version.

## 8. OPAQUE

- Library: **libopaque** (stef/libopaque) pinned at commit `6e9ac92` (v1.0.1-4) with liboprf
  `f925b87` and libsodium ≥ 1.0.19. It is used through the C core (Qt), the Python binding
  `libopaque/python` (Flask, which needs `pysodium`), and the JS/WASM build **from the repo's
  `js/dist/libopaque.js` at that commit**. The npm package `libopaque@0.0.0-alpha.2` is NOT
  compatible (it fails at `finalizeRequest`) and must not be used. `opaque-spike/` holds a
  reproducible cross-implementation check (WASM client against the C/Python server).
- Ids: `idU` = the account email, lowercased and trimmed. `idS` = the issuer host
  (e.g. `accounts.keepassxc.org`). Context = `kpx-opaque-v1`.
- The server uses a **per-user server keypair** (`CreateRegistrationResponse` with `skS = null`).
- Registration: `CreateRegistrationRequest` → `register/start` →
  `CreateRegistrationResponse`. The server keeps `sec` in Redis for 5 minutes under `session_id`.
  Then `FinalizeRequest` → `register/finish` (record) → `StoreUserRecord`.
- Login: `CreateCredentialRequest` (ke1) → `login/start` → `CreateCredentialResponse`. The server
  keeps `authU0` (`sec`) in Redis for 5 minutes. Then `RecoverCredentials` (ke2) → `login/finish`
  (authU) → `UserAuth`.
- **Unknown email:** `login/start` still answers, with a fake record derived from
  `HMAC(server secret, email)`, so the response looks the same and accounts can't be listed.
  `login/finish` then fails with the same 401 as a wrong password.
- The OPAQUE `export_key` is not used in v1.

## 9. Tokens

### Access tokens (JWT)

- Header `{"alg":"EdDSA","typ":"JWT","kid":…}`, signed with Ed25519. JWKS:
  `{"kty":"OKP","crv":"Ed25519","x","kid","use":"sig","alg":"EdDSA"}` at `/.well-known/jwks.json`.
- Claims:

| Claim | Value |
|---|---|
| `iss` | issuer URL, the same as in `/.well-known/kpx-configuration` |
| `aud` | `["kpx-api","kpx-sync"]` |
| `sub` | `user:<uuid>` or `sa:<uuid>` |
| `dev` | device uuid (users only) |
| `org` | owning org uuid (SAs only) |
| `vlt` | `[{"id": vault uuid, "r": "read"|"write"|"manage"}]`, every vault the principal can open |
| `iat`, `nbf`, `exp` | `exp = iat + 600` |
| `jti` | uuid |

- Verifiers accept only `alg` `EdDSA`, check `iss` and `aud` (the sync server needs `kpx-sync`),
  and allow 60 seconds of clock skew on `exp` and `nbf`. If they see an unknown `kid`, they refetch
  the JWKS at most once every 30 seconds.
- Roles: `read` < `write` < `manage`. `write` can PUT a blob. `manage` can create or delete the
  blob and change access. Org owners and admins get `manage` on every org vault.

### Refresh tokens

- `kpx_rt_` + base64url(32 random bytes). The server stores `SHA-256(utf8(token))` hex. A refresh
  token is bound to one device, lives 30 days without use, and is replaced by every refresh.
- **Reuse detection:** presenting a token that was already rotated revokes the whole family (the
  device's session), and the user has to log in again.

### Service account tokens

```
kpx_sa_ + base64url(UTF-8 JSON {"v":1,"url":<issuer>,"sa_id":<uuid>,"secret":<b64url 32>,"kek":<b64url 32>})
```

- The server stores `SHA-256(secret)` hex, `prefix` = the first 8 characters of the base64url
  secret (for display), and the per-secret `wrapped_master = wrap(kek, sa_master_key, "kpx-sa-master-v1|<sa>")`.
- The SA keyset is stored once, wrapped by the SA **master key**. The master key is sealed to every
  org owner and admin, so any of them can issue new secrets without having the old ones.
- Exchange: `POST /api/v1/auth/service-account {sa_id, secret}` returns
  `{access_token, expires_in, wrapped_master, keyset:{public…, wrapped}}`. The client unwraps the
  master key with `kek`, then the keyset with the master key. No refresh token: SAs exchange again
  when the access token expires.

## 10. Discovery

`GET /.well-known/kpx-configuration` on the auth server:

```json
{"issuer":"https://accounts.keepassxc.org","api":"https://accounts.keepassxc.org/api/v1",
 "sync":"https://sync.keepassxc.org/api/v1","jwks_uri":"https://accounts.keepassxc.org/.well-known/jwks.json",
 "opaque":{"lib":"libopaque","version":"1.0.1-6e9ac92","context":"kpx-opaque-v1","ids":"accounts.keepassxc.org"},
 "kdf_min":{"m":19456,"t":2}}
```

## 11. Events

The sync server's `GET /api/v1/events` is the only event stream. With a JWT:

- `event: vault`, `data: {"id","etag","revision"}` or `{"id","deleted":true}`. Sent for every vault
  in the token's `vlt` that changes.
- `event: access`, `data: {"vault": uuid|null, "change": "granted"|"revoked"|"rotating"|"rotated"|"org"}`.
  The auth server sends these through the sync server (below). On any `access` event the client
  refreshes its JWT (picking up new `vlt` claims), calls `GET /api/v1/vaults` again, and
  reconnects the stream.
- The sync server ends a JWT stream when its token's `exp` passes, and the client reconnects with a
  fresh token.
- Legacy device-token streams keep sending `event: database` unchanged.

**Auth → sync notification:** `POST {sync}/api/v1/internal/notify` with
`Authorization: Bearer <SYNC_INTERNAL_TOKEN>` (a shared secret; the route is disabled when the
variable is unset) and body `{"subjects":["user:…","sa:…"],"event":"access","data":{…}}`, returning
`204`. The sync server sends the event to every open stream of those subjects. The call is best
effort: the auth server sends it after the commit and logs failures.

## 12. Modes

| `SYNC_AUTH_MODE` | Accepted tokens | Routes |
|---|---|---|
| `legacy` (default) | device tokens | `databases/*`, `devices/pairing*`, `events` (legacy) |
| `jwt` | JWTs | `vaults/*`, `events`, `internal/notify` |
| `both` | either; a token with two `.` separators is a JWT | all of the above |

`vaults/*` and `internal/notify` return 404 in `legacy` mode. Legacy namespaces (`databases/{id}`
under a user) and vault blobs (`vaults/{id}`) never share storage keys.
