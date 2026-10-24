# OpenVireo accounts: crypto and protocol spec (ov v1)

This is the shared contract for the Flask auth server (`openvireo-auth`), the sync server
(`openvireo-sync-server`), the Qt desktop (`openvireo`), and the TS SDK (`openvireo-sdk`). The plan
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
- **Transport:** v1 clients accept `http://` as well as `https://` issuer, API and sync URLs (decided
  2026-10-17, for local and self-hosted setups); hosted deployments use https. Requiring https is
  left for a later version.
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
AUK        = HKDF(ikm=stretched, salt=SecretKey(16 bytes), info="ov-auk-v1", L=32)
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
| User keyset | AUK | `enc_sk(32) ‖ sign_seed(32)` | `ov-keyset-v1` |
| SA keyset | SA master key | `enc_sk(32) ‖ sign_seed(32)` | `ov-keyset-v1` |
| SA master key, per secret | `kek` from the SA token | master key (32) | `ov-sa-master-v1|<sa uuid>` |
| Vault name | `HKDF(vault_key, salt="", info="ov-vault-name-v1")` | UTF-8 name | `ov-vault-name|<vault uuid>` |

## 5. Keysets and public keys

- Each principal has an **X25519** encryption keypair and an **Ed25519** signing keypair (the
  32-byte seed is stored). The public part is
  `{"enc_pk","sign_pk","sig"}`, where `sig = Ed25519.sign(sign_seed, "ov-pubkeys-v1" ‖ enc_pk ‖ sign_pk)`.
  Every client checks `sig` before sealing anything to a public key.
- **Fingerprint** (shown in share dialogs, Phase 6 verification): the first 20 bytes of
  `SHA-256("ov-fp-v1" ‖ enc_pk ‖ sign_pk)` as lowercase hex in groups of 4 separated by spaces.
- Clients keep the decrypted keyset only in memory while the account is unlocked.

## 6. Seal (public-key envelope, "ov-seal-v1")

```
esk, epk  = fresh X25519 keypair
shared    = X25519(esk, recipient_enc_pk)       # reject an all-zero result
key       = HKDF(ikm=shared, salt=epk ‖ recipient_enc_pk, info="ov-seal-v1", L=32)
sealed    = 0x01 || epk(32) || nonce(24) || XChaCha20-Poly1305(key, nonce, plaintext, aad)
```

| Use | Plaintext | AAD |
|---|---|---|
| Vault key for a member | vault key (32) | `ov-vault-key|<vault uuid>` |
| SA master key for an org owner/admin | SA master key (32) | `ov-sa-master-v1|<sa uuid>` |

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
pending version during a rotation. A new vault starts at key version **1**: every grant in
`POST /api/v1/vaults` uses it, and the caller must be one of the grantees. An **org vault** must also
have a `manage` grant for every confirmed owner and admin of the org at creation, or the server
answers 400 naming who is missing; regular members are added afterwards as the admin chooses
(§12). Invited or not-yet-confirmed owners and admins get it when they are confirmed. Rotating is required when a member is removed and optional at
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
  `libopaque/python` (Flask, which needs `pysodium`), and a JS/WASM **release build** of that
  commit made by `opaque-spike/build-libopaque-wasm.sh` (`DEFINES=` empty, emsdk 1.40.1;
  SHA-256 `5135049e647894325acc6ec2d832853afe6591ad770bd2d02029a400d0a98495`). Two builds must
  **never** be used:
  - upstream's `js/dist/libopaque.js` at 6e9ac92, which is built with `-DTRACE -DNORANDOM`: its
    randomness is the fixed sequence 00 01 02 …, so every login sends the same client nonce and key
    share, and it prints passwords and keys to the console;
  - the npm package `libopaque@0.0.0-alpha.2`, which is incompatible (it fails at `finalizeRequest`).

  Clients refuse to run a build whose login request (KE1 bytes 32–63) carries the nonce
  00 01 … 1f. `opaque-spike/` holds the build script and a cross-implementation check.
- Passwords go into OPAQUE as their real NFKC UTF-8 bytes. The stock libopaque.js wrapper passes
  the UTF-16 length and truncates non-ASCII passwords, so JS clients call the raw exports.
- Ids: `idU` = the account email, lowercased and trimmed. `idS` = the issuer host
  (e.g. `accounts.openvireo.com`). Context = `ov-opaque-v1`.
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
- **MFA:** when TOTP is on, `login/finish` returns `mfa_token` (5 minutes). A wrong code returns 401
  `invalid_mfa` and keeps the token, so the user can retry; the 5th wrong code invalidates it. The
  first correct code consumes it.
- The OPAQUE `export_key` is not used in v1.

## 9. Tokens

### Access tokens (JWT)

- Header `{"alg":"EdDSA","typ":"JWT","kid":…}`, signed with Ed25519. JWKS:
  `{"kty":"OKP","crv":"Ed25519","x","kid","use":"sig","alg":"EdDSA"}` at `/.well-known/jwks.json`.
- Claims:

| Claim | Value |
|---|---|
| `iss` | issuer URL, the same as in `/.well-known/openvireo-configuration` |
| `aud` | `["ov-api","ov-sync"]` |
| `sub` | `user:<uuid>` or `sa:<uuid>` |
| `dev` | device uuid (users only) |
| `org` | owning org uuid (SAs only) |
| `vlt` | `[{"id": vault uuid, "r": "read"|"write"|"manage"}]`, every vault the principal can open |
| `iat`, `nbf`, `exp` | `exp = iat + 600` |
| `jti` | uuid |

- Verifiers accept only `alg` `EdDSA`, check `iss` and `aud` (the sync server needs `ov-sync`),
  and allow 60 seconds of clock skew on `exp` and `nbf`. If they see an unknown `kid`, they refetch
  the JWKS at most once every 30 seconds.
- Roles: `read` < `write` < `manage`. `write` can PUT a blob. `manage` can create or delete the
  blob and change access. Org owners and admins get `manage` on every org vault.

### Refresh tokens

- `ov_rt_` + base64url(32 random bytes). The server stores `SHA-256(utf8(token))` hex. A refresh
  token is bound to one device, lives 30 days without use, and is replaced by every refresh.
- **Reuse detection:** presenting a token that was already rotated revokes the whole family (the
  device's session), and the user has to log in again.

### Service account tokens

```
ov_sa_ + base64url(UTF-8 JSON {"v":1,"url":<issuer>,"sa_id":<uuid>,"secret":<b64url 32>,"kek":<b64url 32>})
```

- A secret's `expires_at` lives on the server only (the token doesn't carry it), so an owner or
  admin can change it later with `PATCH /service-accounts/{sa}/secrets/{id}` (a past time expires
  it at once, `null` removes the expiry) without issuing a new token. An SA's own `expires_at`
  applies on top of each secret's.
- The server stores `SHA-256(secret)` hex, `prefix` = the first 8 characters of the base64url
  secret (for display), and the per-secret `wrapped_master = wrap(kek, sa_master_key, "ov-sa-master-v1|<sa>")`.
- The SA keyset is stored once, wrapped by the SA **master key**. The master key is sealed to every
  org owner and admin, so any of them can issue new secrets without having the old ones.
- Exchange: `POST /api/v1/auth/service-account {sa_id, secret}` returns
  `{access_token, expires_in, wrapped_master, keyset:{public…, wrapped}}`. The client unwraps the
  master key with `kek`, then the keyset with the master key. No refresh token: SAs exchange again
  when the access token expires.

## 10. Discovery

`GET /.well-known/openvireo-configuration` on the auth server:

```json
{"issuer":"https://accounts.openvireo.com","api":"https://accounts.openvireo.com/api/v1",
 "sync":"https://sync.openvireo.com/api/v1","jwks_uri":"https://accounts.openvireo.com/.well-known/jwks.json",
 "opaque":{"lib":"libopaque","version":"1.0.1-6e9ac92","context":"ov-opaque-v1","ids":"accounts.openvireo.com"},
 "kdf_min":{"m":19456,"t":2},"web":"https://app.openvireo.com"}
```

`web` is the web app's URL (§16). It is optional; clients that don't find it hide "Manage account".

## 11. Events

The sync server's `GET /api/v1/events` is the only event stream. With a JWT:

- `event: vault`, `data: {"id","etag","revision"}` or `{"id","deleted":true}`. Sent for every vault
  in the token's `vlt` that changes.
- `event: access`, `data: {"vault": uuid|null, "change": "granted"|"revoked"|"rotating"|"rotated"|"org"}`.
  The auth server sends these through the sync server (below). On any `access` event the client
  refreshes its JWT (picking up new `vlt` claims), calls `GET /api/v1/vaults` again, and
  reconnects the stream.
- The sync server ends a JWT stream when its token's `exp` passes (exactly `exp`, without the
  skew allowance), and the client reconnects with a fresh token. Browsers must read the stream with
  `fetch`, because `EventSource` can't send an `Authorization` header.
- Clients send only CORS-allowed request headers on the stream (`Authorization`, plus the
  safelisted `Accept`), never `Cache-Control`: a browser would need a preflight the sync server
  refuses. Browsers ask `fetch` for `cache: "no-store"` instead.
- Legacy device-token streams keep sending `event: database` unchanged.
- History: `GET {sync}/vaults/{id}/history` returns `{"versions":[BlobMeta…]}` and
  `GET {sync}/vaults/{id}/history/{revision}` returns that version's bytes (role `read`), as for
  legacy databases.

**Auth → sync notification:** `POST {sync}/internal/notify`, where `{sync}` is the discovery value
and already ends in `/api/v1` (so the full path is `/api/v1/internal/notify`; never add `/api/v1`
twice). It carries
`Authorization: Bearer <SYNC_INTERNAL_TOKEN>` (a shared secret; the route is disabled when the
variable is unset) and body `{"subjects":["user:…","sa:…"],"event":"access","data":{…}}`, returning
`204`. The sync server sends the event to every open stream of those subjects. The call is best
effort: the auth server sends it after the commit and logs failures.

## 12. Invitations

An org invitation token is `ov_inv_` + base64url(32 random bytes); the server stores only its
SHA-256. It reaches the invitee only by email, as a link to `OPENVIREO_INVITATION_URL#token=<token>` (in
the fragment, so it never reaches a server log), and is accepted with `POST /api/v1/invitations/accept`.

### Confirming members

Confirming an accepted member (`POST /orgs/{org}/members/{user}/confirm`, owner/admin) sends
`grants` as `VaultGrant`s (each names its `vault_id`) sealed to the member at each vault's current
key version, after the client checks the member's key signature (§5):

- an **owner or admin** gets every org vault the confirming client manages, with role `manage`,
  plus `sa_master_keys` for every org service account whose master key that client holds;
- a **member** gets only the vaults the admin picks, with the role picked for each, and no SA master
  keys. An empty list is valid: the member then sees no vault until one is shared.

Promoting a confirmed member to owner/admin (`PATCH …/members/{user}`) carries the same
`sa_master_keys` and is followed by `manage` grants on the same vaults; demoting to member drops
their SA master keys on the server. Removing a member flags the org vaults they could open
(`rotation_required`), and a client with `manage` rotates each one (§7).

## 13. Modes

| `SYNC_AUTH_MODE` | Accepted tokens | Routes |
|---|---|---|
| `standalone` (default; `legacy` is accepted as its old name) | device tokens | `databases/*`, `devices/pairing*`, `events` (legacy) |
| `jwt` | JWTs | `vaults/*`, `events`, `internal/notify` |
| `both` | either; a token with two `.` separators is a JWT | all of the above |

`vaults/*` and `internal/notify` return 404 in `standalone` mode. In `both` mode, a JWT on a legacy route or a
device token on a vault route gets 403 `forbidden`. Creating a vault blob needs `If-None-Match: *`
and updating it needs `If-Match`; a vault PUT with neither gets 428, even when the blob doesn't
exist (unlike legacy databases, which can be created without a header). The sync server stores
vault blobs under `<data>/vaults/<uuid>.kdbx` on disk and the `_vaults/` prefix in S3; the
exact layout is up to the server. Legacy namespaces (`databases/{id}`
under a user) and vault blobs (`vaults/{id}`) never share storage keys.

## 14. Secret references

`ov://<vault>/[<group>/…]<entry>/<field>` names one field; without `/<field>` it names a whole entry
(an *item*, for `openvireo item` and `openvireo connect`).

- Segments are split on `/` and then percent-decoded (UTF-8), so a `/` inside a name is `%2F`. An
  empty segment is an error (`invalid_reference`).
- `<vault>` is a vault id, or else a vault name that must match exactly one vault the principal can
  open (two vaults with that name → error; use the id).
- Group segments are matched by exact name, one level at a time from the vault's root group.
- `<entry>` is an entry UUID (32 hex digits, with or without dashes, or KDBX base64), or else a title
  that must match exactly one entry under that group, recursively, outside the Recycle Bin.
- `<field>` is a field name: an exact match first, then the aliases `title`, `username`/`user`,
  `password`, `url`, `notes` (any case) for the standard fields, then a case-insensitive match.
- In templates (`openvireo inject`) a bare reference runs up to whitespace, a quote, a backtick or `}`;
  `{{ ov://… }}` (any spacing inside the braces) delimits one followed by other text.

## 15. Service-account tools

**`openvireo run --env-file`** reads a `.env` file: `KEY=value` lines, an optional `export ` prefix, `#`
comments, `"double quotes"` (escapes `\n`, `\t`, `\"`, `\\`; may span lines) and `'single quotes'`
(literal). A value that is a reference, or contains `{{ … }}` references, is resolved. Variables
already in the environment are resolved the same way. The child never receives
`OPENVIREO_SERVICE_ACCOUNT_TOKEN`, `OPENVIREO_PASSWORD` or `OPENVIREO_SECRET_KEY`.

**Masking:** every resolved value, and every line of 4 or more characters of a multi-line value, is
replaced by `<concealed by openvireo>` in the child's stdout and stderr, including when a value is split
across writes.

**`openvireo connect`** is a self-hosted REST server holding one SA token. It is not part of the auth or
sync API and lives in `openvireo-sdk/packages/connect`.

- Its bearer tokens are `ov_ct_` + base64url(32 random bytes). Only their SHA-256 (hex, of the UTF-8
  token) is configured, as `{name, sha256, vaults, access}`: `vaults` lists vault ids or names, or
  `"*"`, and `access` is `read` or `write`. They are unrelated to the SA token.
- Routes: `GET /health` and `GET /ready` (no auth); `GET /v1/vaults`, `GET /v1/vaults/{vault}`;
  `GET /v1/vaults/{vault}/items` (no values); `GET /v1/vaults/{vault}/items/{group/…/entry}` (with
  `?field=<name>` → `{"value"}`, or the raw value for `Accept: text/plain`); and
  `PUT /v1/vaults/{vault}/items/{group/…/entry}` with `{"fields":{…},"protected":{name:true}}`, which
  creates (201) or updates (200) the entry, keeping history (`?create=false` → update only). A PUT
  needs a `write` token and a `write` grant, and goes through the conditional PUT with merge and
  retry (§11, 412).
- `{vault}` and `{entry}` resolve as in §14. A vault outside a token's scope answers 404, as an unknown
  one does. Errors use the `{"error","code"}` format with the auth API's codes, plus
  `upstream_unauthorized`/`upstream_error` (502) when the auth or sync server refuses the SA.
  Responses carry `Cache-Control: no-store`.
- Service accounts download the whole vault, as every client does; connect reuses a downloaded copy
  for a configured time and then checks it with a conditional HEAD.

## 16. Web account and settings

The auth website (billing, locale, admin) has no password form and never sees a password or the
Secret Key. Everything that needs the AUK lives in a client: the web app's **Account** page
(`{web}/?view=account`) changes the master password (`/me/password/*`, keyset re-wrapped, same
public keys), changes the email (below), turns TOTP on and off (the enrolment secret is shown as a QR
code of its `otpauth_uri` and as text), lists and revokes devices, and downloads the Emergency Kit
again from the Secret Key stored on the device. Qt's "Manage account…" opens that page in the browser.

**Changing the email.** The email is OPAQUE's `idU`, so the OPAQUE record is registered again under
the new one. The AUK doesn't depend on the email, so the KDF, the keyset, the Secret Key and every
sealed key stay as they are.

1. `POST /me/email {email}` (user token issued in the last 5 minutes): 409 if another account has the
   email. The server mails an 8-digit code to the new address and keeps `{email, SHA-256(code)}` for
   30 minutes under the user (a new request replaces it). 3 requests per hour per user.
2. The client checks the master password by unwrapping the keyset with it (a wrong password must
   never become the new OPAQUE record), then `POST /me/email/start {email, code, request}`, where
   `request` is `CreateRegistrationRequest(password)` and `email` must match the pending one. A wrong
   code is 400 `invalid_code`; the 5th wrong code drops the pending change (410 `session_expired`).
   The answer is `{session_id, response}`, as for `/me/password/start`.
3. `FinalizeRequest` with `idU` = the new email → `POST /me/email/finish {session_id, record}` (204).
   The server stores the record and the email at once (409 if the email was taken meanwhile) and
   mails a notice to the old address. Sessions stay signed in.

Clients keep the Secret Key and device id on the device under the email (SDK `secret_key:<idU>`,
Qt `ov-account-secret-key:<idS>:<email>`). When `GET /me` returns an email that differs from the
stored session's, the client moves those entries to the new email and updates its session, so other
devices that are signed in follow the change without asking for the Secret Key again.

- **Sign-in and sign-up** on the auth website redirect to `{web}/?view=signin` or `?view=signup`,
  with `&return=<path>` when a page on the auth website asked for it. The web app signs in as usual
  (first sign-in on a device needs the Secret Key) and then hands the session over as below.
- **Handover:** `POST /api/v1/me/web-session` (user token) returns `{"code","expires_in":60}`, where
  `code` is `ov_ws_` + base64url(32 random bytes); the server keeps `SHA-256(utf8(code))` for 60
  seconds, bound to the user. The client then navigates the browser to
  `{issuer}/session/claim?code=<code>&next=<path>`. The auth website consumes the code once, sets
  its cookie session and redirects to `next` if it is a path on the auth website (`/settings`
  otherwise). An unknown, used or expired code goes to sign-in.
- Accounts made before this (a password hash and no OPAQUE record) can't sign in anywhere and are
  deleted; admins are account users whose role is set with `flask users role <email> admin`.


## 17. Names

The fork is published as **OpenVireo**. This is a clean break from the earlier `kpx v1` names:
nothing accepts the old prefixes, labels or variables, and nothing migrates. Every implementation
uses these names:

| Thing | Name |
|---|---|
| Protocol | `ov v1`; every crypto label (§3–§8) starts `ov-` |
| JWT audiences | `ov-api`, `ov-sync` |
| Discovery | `/.well-known/openvireo-configuration` |
| Tokens | `ov_rt_`, `ov_sa_`, `ov_inv_`, `ov_ct_`, `ov_ws_` |
| Secret references | `ov://` |
| Environment variables | `OPENVIREO_*` (e.g. `OPENVIREO_SERVICE_ACCOUNT_TOKEN`, `OPENVIREO_INVITATION_URL`); the sync server's `SYNC_*` keep their names |
| CLI command | `openvireo` (alias `ov`); masking text `<concealed by openvireo>` |
| Desktop | app `OpenVireo`, binaries `openvireo`, `openvireo-cli`, `openvireo-proxy`, app/bundle id `com.openvireo.OpenVireo`, URL scheme `openvireo://` |
| Browser extension | `OpenVireo Browser`, native-messaging host `com.openvireo.browser` |
| Keychain | sync token service `com.openvireo.SyncToken`; Secret Key `ov-account-secret-key:<idS>:<email>` |
| Repos and packages | `openvireo-*` repos; npm `@openvireo/*` |
| Hosts | `openvireo.com` (site), `app.` (web app), `accounts.` (issuer), `sync.` (sync server) |

These keep their old names: the KeePassXC copyright and licence notices, KDBX data keys
(`KeePassXC-Browser Settings`, `KeePassXC-Browser Passkeys`, `KPEX_PASSKEY_*`,
`KPXC_REMOTE_SYNC_SETTINGS`) so databases still open in other KeePass clients, internal Qt symbols
(`KPXC_FEATURE_*`, `KEEPASSXC_*`, namespaces, classes, file names) and links to upstream issues.
