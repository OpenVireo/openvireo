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
- **Transport:** clients require `https://` for the issuer, every URL in `openvireo-configuration`
  (`api`, `sync`, `jwks_uri`, `web`) and self-hosted sync servers, and check each URL on its own, so an
  https issuer can't send clients to a plain-http sync server. Plain `http://` is allowed only for this
  computer: `localhost`, `*.localhost`, `127.0.0.0/8` and `[::1]`, for local development and the
  workspace stack. A refused URL fails before anything is sent (SDK code `insecure_url`). Server-to-server
  traffic (the sync server's JWKS fetch, `SYNC_INTERNAL_URL`) is the operator's choice. (Decided
  2026-11-02, replacing the v1 rule of 2026-10-17 that accepted any `http://` URL.)
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
  non-extractable WebCrypto AES-GCM key) and printed in the Emergency Kit. A stored Secret Key
  belongs to one account on one server, and §21 says when it is deleted.

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
| `vlt` | `[{"id": vault uuid, "r": "read"|"write"|"manage", "q"?: bytes}]`, every vault the principal can open; `q` is the vault's sync quota (§19) |
| `del` | RFC 3339 erase date, only on a pending account's token, which then has no `vlt` (§23) |
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
  device's session, code `token_reused`), and the user has to log in again.
- **Grace period:** the response to a refresh can be lost, leaving the device with the rotated
  token. For 60 seconds from its first rotation, a rotated token whose replacement nothing has used
  is accepted: the server revokes that replacement and issues a new one. Retries don't extend the 60
  seconds. A client doesn't cancel a refresh it has started, and retries one that got no response.

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
`share` is the base URL of the public share page (§22), optional; without it clients use `<web>/s`.

## 11. Events

The sync server's `GET /api/v1/events` is the only event stream. With a JWT:

- `event: vault`, `data: {"id","etag","revision"}` or `{"id","deleted":true}`. Sent for every vault
  in the token's `vlt` that changes.
- `event: access`, `data: {"vault": uuid|null, "change": "granted"|"revoked"|"rotating"|"rotated"|"org"|"deletion_scheduled"|"account_deleted"}`.
  `deletion_scheduled` carries `at` (the erase date) and `account_deleted` ends the account (§23).
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
(an *item*, for `ov item` and `ov connect`).

- Segments are split on `/` and then percent-decoded (UTF-8), so a `/` inside a name is `%2F`. An
  empty segment is an error (`invalid_reference`).
- `<vault>` is a vault id, or else a vault name that must match exactly one vault the principal can
  open (two vaults with that name → error; use the id).
- Group segments are matched by exact name, one level at a time from the vault's root group.
- `<entry>` is an entry UUID (32 hex digits, with or without dashes, or KDBX base64), or else a title
  that must match exactly one entry under that group, recursively, outside the Recycle Bin.
- `<field>` is a field name: an exact match first, then the aliases `title`, `username`/`user`,
  `password`, `url`, `notes` (any case) for the standard fields, then a case-insensitive match.
- In templates (`ov inject`) a bare reference runs up to whitespace, a quote, a backtick or `}`;
  `{{ ov://… }}` (any spacing inside the braces) delimits one followed by other text.

## 15. Service-account tools

**`ov run --env-file`** reads a `.env` file: `KEY=value` lines, an optional `export ` prefix, `#`
comments, `"double quotes"` (escapes `\n`, `\t`, `\"`, `\\`; may span lines) and `'single quotes'`
(literal). A value that is a reference, or contains `{{ … }}` references, is resolved. Variables
already in the environment are resolved the same way. The child never receives
`OPENVIREO_SERVICE_ACCOUNT_TOKEN`, `OPENVIREO_PASSWORD` or `OPENVIREO_SECRET_KEY`.

**Masking:** every resolved value, and every line of 4 or more characters of a multi-line value, is
replaced by `<concealed by openvireo>` in the child's stdout and stderr, including when a value is split
across writes.

**`ov connect`** is a self-hosted REST server holding one SA token. It is not part of the auth or
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
| Tokens | `ov_rt_`, `ov_sa_`, `ov_inv_`, `ov_ct_`, `ov_ws_`, `ov_rq_`, `ov_rc_` |
| Secret references | `ov://` |
| Environment variables | `OPENVIREO_*` (e.g. `OPENVIREO_SERVICE_ACCOUNT_TOKEN`, `OPENVIREO_INVITATION_URL`); the sync server's `SYNC_*` keep their names |
| CLI command | `ov` (not `openvireo`, which is the desktop app's binary on Linux); masking text `<concealed by openvireo>` |
| Desktop | app `OpenVireo`, binaries `openvireo`, `openvireo-kdbx`, `openvireo-proxy`, app/bundle id `com.openvireo.OpenVireo`, URL scheme `openvireo://` |
| Browser extension | `OpenVireo Browser`, native-messaging host `com.openvireo.browser` |
| Keychain | sync token service `com.openvireo.SyncToken`; Secret Key `ov-account-secret-key:<idS>:<email>` |
| Repos and packages | `openvireo-*` repos; npm `@openvireo/*` |
| Hosts | `openvireo.com` (site), `app.` (web app), `accounts.` (issuer), `sync.` (sync server) |

These keep their old names: the KeePassXC copyright and licence notices, KDBX data keys
(`KeePassXC-Browser Settings`, `KeePassXC-Browser Passkeys`, `KPEX_PASSKEY_*`,
`KPXC_REMOTE_SYNC_SETTINGS`) so databases still open in other KeePass clients, internal Qt symbols
(`KPXC_FEATURE_*`, `KEEPASSXC_*`, namespaces, classes, file names) and links to upstream issues.

## 18. Account recovery

A user who has lost the master password or the Secret Key can get the **account** back (email,
billing, org memberships) through a manual review on the hosted service. No server can decrypt
vault data, so a reset starts the account with new keys. The old keys and vaults are kept for
**30 days**, so someone who later finds their Emergency Kit and old password can restore their
vaults into the new keys. Self-hosted deployments may leave recovery off (`OPENVIREO_RECOVERY=0`
hides the form and answers 404 on every route below).

**Tokens.** Both are random 32 bytes, base64url, and the server stores only `SHA-256(utf8(token))`
hex, as with refresh tokens (§9):

| Token | Use | Lifetime |
|---|---|---|
| `ov_rq_` | confirms that the requester controls the email; sent as a link to `{issuer}/recover/confirm?token=` | 1 hour, single use |
| `ov_rc_` | the reset link `{web}/?view=recover#token=` (fragment, as in §12) | usable from `not_before` until 72 hours after approval, single use |

A billing-only approval mails a link to `{issuer}/recover/claim?token=<ov_ws_…>`. That is the same
`ov_ws_` code as in §16, but it lives 60 minutes instead of 60 seconds and signs the website in
without any key change. Nothing on the website needs keys, so billing, locale and invoices work.

### Request and review

1. **Form** (`{issuer}/recover`, anonymous): email, outcome wanted (`billing` or `reset`),
   optional evidence (name on the card, last 4 card digits, the date of the last invoice or of
   sign-up) and free text. The answer is always the same page ("if an account exists, we've
   emailed you"). Limits: 3 per hour per IP and 3 per day per email. An `ov_rq_` link is mailed
   only when an active account with keys has that email. A new request replaces the account's
   open one.
2. **Confirm:** opening the link moves the request from `unconfirmed` to `pending`. Only pending
   requests reach the admin queue.
3. **Review** (`/admin/recovery`, role admin): the admin compares the evidence with what the server
   holds, then **declines** (a reason that is mailed to the user), **approves billing only**, or
   **approves a reset**. Every decision needs a note and writes an audit event. An admin can't
   decide their own request. A request on an admin account needs two different admins to approve:
   the first approval only records itself.
4. **Reset approval:** the server sets `not_before = now + 24 h` (`OPENVIREO_RECOVERY_DELAY`,
   seconds; tests use 0) and mails the account's address the reset link together with a notice.
   The notice says when the link starts working, what will be archived, and has a **cancel** link
   (`{issuer}/recover/cancel?token=<ov_rc_…>`) that sets the request to `cancelled`. The reset
   link stays usable for 72 hours after approval.

Request states: `unconfirmed → pending → declined | approved_billing | approved_reset →
completed | cancelled | expired`, plus `awaiting_second_admin` between `pending` and the
approvals for admin accounts.

### Reset

The web app (`?view=recover`) removes the fragment from the history before calling anything, then:

1. `GET /auth/recover/{token}` shows the account email, `not_before`, `expires_at` and what will be
   archived: the number of personal vaults, grants on vaults owned by others, and orgs whose only
   confirmed owner/admin is this user. Before `not_before` the routes answer 409
   `recovery_not_ready` with `not_before`. A cancelled, used or expired token gets 410
   `recovery_cancelled` / `session_expired`.
2. The client creates a new Secret Key, KDF salt and keyset exactly as at registration (§2–§5),
   checks the user confirms the loss, then `POST /auth/recover/start {token, request}` (an OPAQUE
   registration request under the account email) → `{session_id, response}`.
3. `POST /auth/recover/finish {session_id, record, kdf, keyset, device}` → `Session` (signed in, as
   register/finish). The client shows the new Emergency Kit.

In one transaction, the server:

- moves the old `opaque_record`, `kdf`, `keyset` and the email (the old `idU`) into a **recovery
  archive** that expires 30 days later, then stores the new record, kdf and keyset;
- turns TOTP off;
- revokes every refresh token and deletes every device (other devices get 401 on their next
  refresh and fall back to signing in);
- **personal vaults** (no `org_id`, owned by the user) that nobody else has access to: marks them
  archived. They leave every listing and JWT `vlt` claim, and auth routes answer 404 for them. The
  sync server keeps the blobs, and with no grant no token reaches them. A personal vault the user
  shared stays live for the other grantees and is treated like the next point;
- moves every other `vault_access` row of the user (vaults shared with them, org vaults, their
  shared personal vaults) into the archive, together with its `key_version` and role. Nothing is rotated: the old private keys exist
  only wrapped with the old AUK;
- sets each `confirmed` org membership to `accepted` and drops the user's SA master keys, so an
  owner or admin confirms them again against the new keys (§12);
- keeps the user id, the email, the Stripe customer, subscription and invoices;
- sends `access` `revoked` for every affected vault (§11), records `account.recovered` and mails the
  address "your account was reset", with the archive's expiry date.

### Restore within 30 days

While an archive exists, `GET /me/recovery` returns `{archived: true, email, expires_at, vaults, locked}`
(`{archived: false}` otherwise), where `email` is the archived one, and the web Account page offers a restore. All restore routes need
a user token issued in the last 5 minutes (403 `forbidden` otherwise, as for `/me/password/*`). They allow 5 attempts per hour, and 10 failed `verify`
or `finish` calls lock the archive (403 `restore_locked`) until an admin unlocks it.

1. `POST /me/recovery/restore/start {ke1}`: OPAQUE login against the **archived** record, with the
   archived email as `idU` → `{session_id, ke2, email}`. The client finishes OPAQUE with that `email` as
   `idU`; it differs from the account's current email if the email changed after the reset.
2. `POST /me/recovery/restore/verify {session_id, auth_u}`: 401 for a wrong old password. Otherwise
   → `{kdf, keyset, grants}`, the archived kdf and wrapped keyset plus every archived grant whose
   vault still exists **and** is still at that key version, as
   `VaultGrant` with the caller as `principal`. Grants on vaults rotated since the reset
   are left out; an org admin grants those again. The session moves on to `finish` (5 minutes).
3. The client derives the old AUK from the old password and the old Secret Key (the Emergency Kit),
   unwraps the old keyset, unseals each grant (AAD `ov-vault-key|<vault>`) and seals the vault key
   to the account's **current** `enc_pk` (§6).
4. `POST /me/recovery/restore/finish {session_id, grants, proof}` with
   `proof = Ed25519.sign(old sign_seed, "ov-restore-v1" ‖ utf8(user id) ‖ new enc_pk ‖ new sign_pk)`.
   The user id is the 36-character uuid string, and "new" is the account's current public keys.
   The server checks the proof against the archived `sign_pk` (400 `invalid_proof` otherwise). That
   proves the person has the old Secret Key, which step 1 alone doesn't. `grants` must name only
   vaults returned in step 2, at the same key versions, with the caller as `principal`; anything
   else is 400. A grant whose vault was rotated or deleted between `verify` and `finish` is
   skipped, not an error. Each grant gets its
   archived role. The server then un-archives the personal vaults, deletes the archive, sends
   `access` `granted`, records `account.restored` and mails a notice. Org memberships stay
   `accepted`.

The new password and Secret Key stay. Restoring merges the old vaults into the new keys and never
swaps the old keys back.

**Expiry.** A daily job deletes archives past `expires_at`, along with their archived grants and
archived vaults (a shared personal vault that other grantees still use stays; its `owner_id`
stays the user). The auth server deletes those vault blobs on the sync server through
`DELETE {sync}/internal/vaults/{id}` (internal token, as `internal/notify`; 204 or 404). The same
call is used whenever the auth server deletes a vault without a client. Unconfirmed requests expire
after 1 hour, and reset approvals 72 hours after approval.

**Audit events:** `account.recovery.requested`, `.declined`, `.approved_billing`,
`.approved_reset`, `.cancelled`, `account.recovered`, `account.restored`,
`account.restore_failed`, `account.restore_locked`. Recovery covers only an address the user can
still read: changing the email of a locked-out account is out of scope for v1.


## 19. Plans and limits

The hosted service sells plans; a self-hosted server has none (`OPENVIREO_BILLING=0`, the default).
Plans pay for hosted storage, sharing and teams. Nothing local, nothing self-hosted and no security
feature (MFA, recovery, Emergency Kit, devices, key rotation, export, account deletion) is ever
limited. Scheduling an account deletion stops the plan renewing (§23).

**Plans.** A personal plan belongs to a user and covers the vaults they own (`owner_id`); an org plan
belongs to an org and covers its vaults. A user without a subscription is on `free`, an org on
`org_free`. `null` is unlimited.

| Plan | Billed | `personal_vaults` | `share_recipients` | `members` | `org_vaults` | `service_accounts` | `audit_days` | `vault_bytes` | `share_links` | `share_link_days` |
|---|---|---|---|---|---|---|---|---|---|---|
| `free` | — | 1 | 0 | | | | | 25 MiB | 5 | 7 |
| `premium` | user, $2/mo, $18/yr | null | 5 | | | | | 1 GiB | null | 30 |
| `org_free` | — | | | 2 | 2 | 1 | 7 | 25 MiB | null | 30 |
| `families` | org, $4/mo, $40/yr | | | 6 | null | 1 | 30 | 1 GiB | null | 30 |
| `teams` | org, $4/member/mo, $40/yr | | | null | null | 10 | 90 | 1 GiB | null | 30 |
| `business` | org, $6/member/mo, $60/yr | | | null | null | null | 365 | 5 GiB | null | 30 |

The prices are the auth server's settings (`BILLING_PLANS`); clients read limits, not prices.

**What each limit counts.**
- `personal_vaults`: live vaults the user owns. Checked by `POST /vaults` without `org_id`.
- `share_recipients`: other users with any grant on one personal vault, checked by `POST /vaults` and
  `POST /vaults/{vault}/access` against the vault owner's plan. Re-sealing for someone who already
  has access (rotation) is not a new recipient.
- `members`: members in any status plus open invitations to other addresses, checked by
  `POST /orgs/{org}/invitations` (re-inviting an address takes no new seat); confirmed members,
  checked by `POST /orgs/{org}/members/{user}/confirm`. Per-member plans (`teams`, `business`) have
  no cap: the org is billed for its confirmed members, updated as they are confirmed or removed.
- `org_vaults`: the org's live vaults, checked by `POST /vaults` with `org_id`.
- `service_accounts`: the org's SAs, checked by `POST /orgs/{org}/service-accounts`.
- `audit_days`: `GET /orgs/{org}/audit` returns only events this many days old or newer.
- `vault_bytes`: the largest database the sync server takes for the vault (below).
- `share_links`, `share_link_days`: share links that haven't ended, and their longest expiry (§22).
  Checked by `POST /shares`.

**Over a limit.** Routes answer **402** `plan_limit`:
`{"error", "code": "plan_limit", "limit": "<name>", "max": n, "current": n, "plan": "<plan>", "upgrade_url"}`.
`upgrade_url` is the auth site's pricing page, or the org's billing page for an org limit. Clients
show the message with an Upgrade link. Limits only stop *new* things: after a downgrade, or when a
subscription ends, everything over the limit keeps working (vaults open, sync, members stay), and
nothing is deleted.

**Reading limits.** `GET /me` has `plan: {id, limits, billing}` for the user and the same object on
each entry of `orgs`; `GET /orgs/{id}` and `POST /orgs` have it as `plan`. `id` is `null` and every
limit `null` when billing is off. Clients use it to grey out actions; the server's check is what
counts. `billing` (below) is `null` on a free plan and when billing is off.

**Sync quota.** Each `vlt` entry of the access token (§9) has `q`, the vault's `vault_bytes`, when it
is limited: `{"id", "r", "q": 26214400}`. The sync server answers a `PUT /vaults/{vault}` body
larger than `q` with **413** `quota_exceeded` (`{"error", "code": "quota_exceeded", "max": q}`),
before it stores anything. Without `q` only the server's own `SYNC_MAX_DB_BYTES` applies. A quota
change reaches the sync server with the next token (at most `exp`, 10 minutes).

**Billing.** A subject has at most one paid plan, billed by one of two sources:

- **`stripe`**, the website. Stripe Checkout starts a subscription and the Stripe Customer Portal
  changes or cancels it, both from the auth site (`/subscription/pricing`, `/subscription/orgs/{org}`
  for an org's owners). Stripe's webhook keeps the auth server's copy; a subscription is in force
  while Stripe says `active`, `trialing` or `past_due`.
- **`apple`**, In-App Purchase in the iOS app, for `premium` and `families` only (`teams` and
  `business` are per member and sold on the website). Product IDs `com.openvireo.premium.month`,
  `com.openvireo.premium.year` (subscription group Personal) and `com.openvireo.families.month`,
  `com.openvireo.families.year` (group Organization). The App Store sets the price shown in the app,
  which can differ from the website's. Apple's server notifications and the app keep the auth
  server's copy; a subscription is in force while Apple says it is active or in its billing grace
  period, and ends when Apple says it expired, was revoked or refunded, or is in billing retry after
  the grace period.

The plan, its limits and everything above are the same whichever source bills it.

`billing` in `plan`:

```json
{"source": "apple", "interval": "year", "status": "active",
 "renews_at": "2027-11-08T12:00:00Z", "ends_at": null, "manage": "app_store"}
```

- `source`: `stripe` or `apple`. `interval`: `month` or `year`. `status`: `active`, `trialing` or
  `past_due` (Apple's billing grace period is `past_due`).
- `renews_at` when it will renew, else `null`; `ends_at` when it is set not to renew, else `null`.
- `manage`: `website` (the auth site's pricing or org billing page) or `app_store` (the subscription
  belongs to an Apple ID: manage it on that device, or at `https://apps.apple.com/account/subscriptions`).
  Clients follow `manage`, not `source`. No client other than iOS sells In-App Purchase, and none
  can change or cancel an Apple subscription.

**In-App Purchase.** The app buys with `appAccountToken` set to the subject's UUID (the user's for
Premium, the org's for Families) and then reports the transaction.

- `GET /billing/apple/eligibility` (for the user) or `?org=<uuid>` (for an org) →
  `{"eligible": bool, "reason": null | code}`. Not eligible: `billed_by_stripe` (a Stripe plan is in
  force), `billed_by_apple` (an Apple plan is in force; the app offers Manage instead), `not_owner`
  (the caller isn't an owner of the org), `per_seat_plan` (the org is on `teams` or `business`),
  `billing_off`. Clients ask before showing purchase buttons.
- `POST /billing/apple/transactions` `{"signed_transaction": "<JWS from StoreKit>"}`. The server
  verifies Apple's signature, that `appAccountToken` is the caller or an org the caller owns, and that
  the product is a plan for that subject, then records it and answers **200** with the subject's
  `plan`. Errors: 400 `apple_verification_failed`; 403 `forbidden` (token is someone else); 409
  `apple_subscription_bound` (this Apple subscription already belongs to another user or org; it is
  never moved); 409 `billed_by_stripe` (the purchase is recorded but the Stripe plan stays in force:
  the client tells the person to cancel one of them). A transaction without `appAccountToken` (an
  offer code redeemed in the App Store) is bound to the caller. Reporting the same transaction again
  is harmless; clients report every transaction StoreKit gives them (`Transaction.updates`,
  `currentEntitlements`) on launch and on Restore.
- Once bound, an Apple subscription (`originalTransactionId`) stays with its subject across
  renewals, lapses and resubscriptions.
- App Review and TestFlight buy in Apple's Sandbox. The hosted server accepts Sandbox transactions
  too; they last as long as Apple's accelerated Sandbox subscriptions do.

**One plan per subject.** The website refuses Checkout for a subject with an Apple plan in force and
shows how to manage it at Apple instead; it never starts a second subscription. When an Apple plan
ends, a Stripe one can be bought (and the other way round); there is no proration between them.

**Ending.** Deleting an org cancels a Stripe subscription. An Apple subscription can only be
cancelled by the person whose Apple ID pays for it: the server stops counting it for a deleted
subject and tells the owners to cancel it at Apple. Handing an org to a new owner leaves an Apple
subscription on the buyer's Apple ID, covering the org until they cancel it.

## 20. Offline cache

A client may keep what it needs to unlock and open vaults while the servers can't be reached. It
caches only what the server already hands out under the account's keys, and nothing in clear:

- the discovery document (§10) and the server URLs;
- the **wrapped keyset and its `kdf`** as `GET /me` returned them (§3, §5);
- the **vault list as `GET /vaults` returned it**: encrypted names and sealed vault keys (§6, §7);
- per vault, the server's encrypted `.kdbx` at the last sync and local edits not yet uploaded, as a
  `.kdbx` encrypted with the vault key;
- optionally the org list from `GET /me` (ids, names, roles, plan), which is not secret.

The refresh token stays in the keychain (§9) and the Secret Key stays where §2 puts it; neither is
written into the cache.

**Writing.** After every unlock and every `GET /me` / `GET /vaults` that succeeded, the client
replaces the cached copy, so a password change, email change or key rotation reaches the cache with
the next online call.

**Falling back.** Only when the server is **unreachable**: no HTTP response, no response within a
client's deadline, 502/503/504, or an error status without the server's `{error, code}` JSON (a CDN's
or proxy's error page, so it can't end a session either). A client may open from the cache after a short wait and check
with the server in the background, applying its answer when it comes; a refusal then ends the
session as usual. Any
other answer (401, 403, `invalid_*`, other 4xx) is acted on as without a cache: a client never
unlocks from the cache because the server refused. A wrong password fails as online, because the
cached keyset doesn't unwrap. While working from the cache the client says it is offline, keeps
edits locally, and turns off actions that need the server (new vaults, sharing, orgs, invitations).

**Reconnecting.** The client retries in the background. Once `GET /me` and `GET /vaults` succeed
again it refreshes the cache, drops vaults that are no longer listed (with their local copies), and
uploads local edits.

**Wiping.** Ending the session (sign-out, revoked refresh token, §9) deletes that user's cache, and
no other user's. When the session ended by accident (`token_reused`, `session_expired`), a client may
keep its encrypted local edits and merge them after the same user next signs in; everything else in
the cache goes. Locking and setting a session aside (§21) keep it. Clients offer a setting to not keep a cache (Qt: "Allow unlocking without a connection",
on by default); turning it off deletes the cache. iOS has no such setting, because its AutoFill
extension reads the cache.

**Trade-offs.** These are the usual costs of offline unlock; 1Password's model is the same, and
Bitwarden's is the same except that its disk copy alone allows password guessing.
- Guessing the password offline needs the cache **and** the Secret Key, which together derive the
  AUK (§3). With both, guesses are slowed only by the KDF, not by the server's rate limits.
- A revoked device (§11), or one whose user changed the password on another device, keeps working
  from its cache, with the old password, until it reconnects.

## 21. Remembered accounts, switching and shared sessions

A client can know several accounts, and keep several signed in, but has only one open at a time.
This section is client behaviour: nothing changes on the servers or the wire.

**Remembered accounts.** A client may keep a list of accounts it has signed in to, each with its
email, server and user id. The list holds no secrets, and is not synced.

- Add or update an entry after a sign-in, sign-up or recovery, unless the session is shared.
- Keep the entry on sign-out and when the server ends the session.
- Delete it only when the user removes the account from this device, which also deletes the stored
  Secret Key and device id for that account on that server, and works with no session.
- Update it where the stored Secret Key already follows an email change (§16).
- An entry can carry `deletion_scheduled_for`; §23 says when it is set, cleared and removed.

**Leaving an account.**

| | Session | Cache (§20) | Key behind biometrics | Secret Key, device id | Entry |
|---|---|---|---|---|---|
| Switch | Kept, set aside | Kept | Kept | Kept | Kept |
| Sign out | Ended | Deleted | Deleted | Kept | Kept |
| Remove from this device | Ended | Deleted | Deleted | Deleted | Deleted |

**Setting a session aside.** Switching locks the account and moves its session to a key of its own.
It tells the server nothing, and the session is not ended, so the device stays in the device list
and §20's wiping doesn't apply. Returning moves the session back and unlocks as usual, with no
OPAQUE sign-in and no TOTP.

- **Exactly one key holds a refresh token at any moment.** Refresh tokens rotate and reuse revokes
  the session (§9), so a session is moved and never copied. A client waits for a refresh in flight
  before moving the session. After a crash leaves the same user's session under both keys, the active
  key wins and the other is deleted.
- A client keeps at most one set-aside session per user per server.
- **Checking.** A client should refresh each set-aside session when it starts or comes to the
  foreground, at most once every 24 hours. That keeps the session within its 30 days (§9) and tells
  the device about a revocation. A refusal ends that session as in §9 and §20. The account's entry
  stays, and shows as signed out. Having no response changes nothing.
- A process that reads sessions separately from the app (an AutoFill extension) reads the active
  session afresh each time, so it can't refresh with a token that has since moved or rotated.
- Edits not yet uploaded stay encrypted in that user's cache and upload after that account's next
  unlock.

**Shared sessions.** A client running where other people use the same browser profile offers a
shared session at sign-in. A shared session persists nothing in the browser: no entry, email, Secret
Key, device id, tokens, sync state, cache or keys. It is kept in memory only, is never set aside, and
ends when the tab closes or the account locks. Its device row is deleted on sign-out where the server
can be reached. It doesn't use `/session/claim`, whose cookie outlives the tab. Native clients don't
offer shared sessions, since their secrets are in the OS account's keychain.

## 22. Share links

A user can share a copy of an item, or some text, with anyone through a link. The recipient needs no
account. The server keeps only ciphertext; the key is in the link's fragment, which browsers don't
send. (Decided 2026-11-07; the plan is `openvireo-dev-guide/plans/share-links.md`.)

**Keys and envelope.**

```
K        = 32 random bytes                                    # the link key
id       = 16 random bytes, base64url (22 characters)         # chosen by the server
P        = Argon2id(UTF-8(NFKC(passcode)), kdf.salt, kdf.m, kdf.t, kdf.p, len 32)   # only with a passcode
content  = HKDF(ikm=K, salt=P or empty, info="ov-share-v1", L=32)
ct       = wrap(content, pad(UTF-8(payload JSON)), "ov-share-v1|<id>")              # §4
auth     = HKDF(ikm=P, salt=empty, info="ov-share-auth-v1", L=32)                   # only with a passcode
```

- `kdf` is `{"alg":"argon2id","salt":<16 random bytes>,"m","t","p"}` with §3's defaults; clients
  refuse `m < 19456` or `t < 2` here too.
- `pad` appends `0x80`, then zero bytes up to the next multiple of 512. `unpad` drops trailing zero
  bytes and then one `0x80`; anything else is an error.
- The server stores `SHA-256(auth)` and needs `auth` before it returns `ct`. It can try passcodes
  against that hash but learns nothing without `K`.
- The link is `<share>/<id>#<K>`, with `K` base64url. `share` is the discovery field below, or
  `<web>/s` without it. Clients never send `K` anywhere, and the page never puts it in a request.

**Payload.**

```json
{"v":1,"kind":"entry","title":"Office Wi-Fi","shared_by":"Sam",
 "fields":[{"k":"username","label":"Username","value":"guest"},
           {"k":"password","label":"Password","value":"…","protected":true},
           {"k":"totp","label":"One-time code","value":"otpauth://totp/…"}]}
```

- `kind` is `entry` or `text`. A `text` share has one field, `{"k":"text","label":"Text","value":…}`.
- `k` is one of `username`, `password`, `url`, `notes`, `totp`, `custom`, `text`. Pages show
  unknown kinds as `custom`. `protected` fields are hidden until shown. A `totp` value is an
  `otpauth://` URI and the page shows the current code.
- `shared_by` is optional and chosen by the sender. Pages label it as stated by the sender.
- Pages render every value as plain text.
- `ct` is at most 64 KiB.

**Owner reference.** So that the owner's clients can list their links by item without the server
learning which item, a share carries `ref`:

```
ref_key = HKDF(ikm=enc_sk, salt=empty, info="ov-share-ref-v1", L=32)    # the owner's X25519 secret key (§5)
ref     = wrap(ref_key, UTF-8(JSON {"vault":<uuid or null>,"entry":<id or null>,"title":<string>}), "ov-share-ref|<id>")
```

`entry` is the item's KDBX UUID as 32 lowercase hex characters.

**Lifetime.** A share ends when it expires, when `opens` reaches `max_opens`, when its owner revokes
it, after 5 wrong passcodes, when its owner can no longer read its vault (removed, vault deleted or
archived), or when its org turns links off. An ended share's `ct` is deleted; its row is kept for 30
days for the owner's list. `expires_in` is 3600 to 2592000 seconds, and at most `share_link_days`
(§19). `max_opens` is null (unlimited) or 1 to 100. Only `POST …/open` counts as an open, so fetching
the page, and link previews, don't use one up.

**Org policy.** `item_share_links` on an org is `on` (the default), `managers` (only confirmed owners
and admins can share from its vaults) or `off`. Turning it to `off` ends the links from its vaults.
Shares from org vaults are audited (§11): `share.create`, `share.open` (recorded without an IP),
`share.revoke`.

**Plans.** `share_links` counts a subject's shares that haven't ended: the org's for org vaults,
otherwise the user's (personal vaults and text). `share_link_days` caps `expires_in`.

**Discovery.** `share` (optional): the base URL of the public page, e.g.
`"share":"https://share.openvireo.com"`. It is a separate origin from `web` on the hosted service.

**Test vectors.** `share` in `test-vectors.json`: one share without a passcode and one with, each
with `K`, `id`, the nonce, the payload, the padded plaintext, `content`, `ct`, and for the passcode
share `kdf`, `P` and `auth`; and a `ref`.

## 23. Account deletion

A user deletes their account from any client (web, iOS, desktop, CLI). Deletion is **scheduled**:
the account is locked at once and **erased** after a grace period, `ACCOUNT_DELETION_DAYS` on the
auth server (default 30). Signing in during the grace period offers only restoring the account or
signing out. An operator can erase at once from the admin panel.

**Preflight.** `GET /me/deletion` describes what scheduling (or, for a pending account, the erase)
will do: `{scheduled_for, would_erase_on, vaults, orgs, subscription, share_links, blocked}`.
`vaults` are the user's personal vaults with `encrypted_name` (decrypted by the client as for
`GET /vaults`) and `shared_with` (the other principals with a grant). Each of `orgs` has an
`outcome`:

| Membership | `outcome` | At erase |
|---|---|---|
| Member or admin | `leave` | Removed as by `DELETE /orgs/{org}/members/{user}`: grants revoked, vaults `rotation_required`, SA master keys dropped |
| Owner, another confirmed owner exists | `leave` | As above |
| Owner, no other confirmed member (invited and accepted members hold no keys) | `delete` | The org is deleted as by `DELETE /orgs/{org}` |
| Only confirmed owner, other confirmed members | `blocked` | – |

A `blocked` org lists its confirmed `members` (candidates for owner) and makes `blocked` true. The
client resolves it before scheduling: promote a member to owner (`PATCH /orgs/{org}/members/{user}`;
an admin needs no new keys, a member also needs `sa_master_keys`), or delete the org. The server
never chooses an owner for the user.

**Scheduling.** `DELETE /me` with `{"confirm": "<the account email>"}`, on a token issued in the last
5 minutes (as for a password change). 400 `confirm_mismatch` if the email differs; 409
`deletion_blocked` `{orgs:[{id,name}]}` while any org is blocked; 409 `deletion_pending` if already
scheduled. Otherwise it answers **202** `{"scheduled_for": RFC 3339}` and, at once:

- revokes every refresh token and deletes every device of the user;
- ends every share link the user owns (§22); restoring doesn't bring them back;
- sets the personal subscription, and those of orgs with outcome `delete`, to end at the period end
  (no refund) when Stripe bills them. An Apple-billed one (§19) can't be cancelled by the server: the
  preflight's `subscription` has `manage: "app_store"`, clients tell the user to cancel it in their
  Apple ID settings first (iOS opens the manage sheet), and the email repeats it;
- sends `access` `{"vault": null, "change": "deletion_scheduled", "at": scheduled_for}` (§11) to the
  user, and emails them the date and how to restore.

**Pending accounts.** Until the erase, the keyset, OPAQUE record, vaults, grants and memberships are
kept. Login still works, but the access token carries `del: scheduled_for` and **no `vlt`**, so the
sync server serves nothing. A `del` token is refused everywhere except `GET /me`, `GET /me/deletion`,
`POST /me/deletion/cancel`, `POST /auth/refresh` and `POST /auth/logout`, with 403
`account_pending_deletion` `{scheduled_for}`. `GET /me` has `deletion_scheduled_for` (null when not
pending). Others who share the user's personal vaults keep access until the erase. A pending owner
does not count as an owner of an org: removing or demoting the org's last other confirmed owner is
409, so a blocked org can't appear after scheduling. The email stays taken.

**Restoring.** `POST /me/deletion/cancel` (pending account, token issued in the last 5 minutes)
clears the schedule, lets subscriptions that haven't ended renew again, and answers 204. The client
then refreshes its token to get one without `del`.

**Erase.** A daily job erases accounts whose date has passed. It settles each org by its `outcome`;
deletes the user's grants on other vaults (marking those vaults `rotation_required`, notifying their
managers), their SA master keys, every vault they own including archived ones (§18), with the blobs
deleted through `DELETE {sync}/internal/vaults/{id}`, invitations to their email and the user row;
deletes the payment customer of the user and of deleted orgs (an Apple subscription that still
renews is no longer counted for anyone; its renewals are ignored); and sends `access`
`{"vault": null, "change": "account_deleted"}`. The audit keeps `account.delete` with the actor's
subject and no email. After the erase the email can sign up again. Nothing else is kept.

**Giving a vault away.** `POST /vaults/{vault}/owner` `{"principal": "user:<uuid>"}`, by the owner of
a personal vault, to a user who already holds a grant on the vault's current key version (else 409
`not_a_member`; 400 for an org vault). The server makes them the owner with `manage`, removes the old
owner's grants, sets `rotation_required` (the old owner knows the key) and notifies everyone with
access. No crypto is involved: the vault name is wrapped with a key derived from the vault key (§4).
A pending account can't call it, so clients offer it before scheduling.

**Clients.**

| Event | Offline cache (§20), key behind biometrics, AutoFill identities | Secret Key, device id, remembered entry (§21) |
|---|---|---|
| Scheduled (here, or `deletion_scheduled` / a refused refresh elsewhere) | Deleted | Kept; the entry stores `deletion_scheduled_for` |
| Restored, or signed in and `GET /me` shows no date | – | Entry's date cleared |
| `account_deleted`, or a day past the stored date | Deleted | Deleted |

A client signs in to a pending account to a screen offering **Restore account** and **Sign out**, and
shows remembered pending accounts with their date. OPAQUE answers the same for unknown users, so a
client can't learn from the server that an account was erased; it clears the entry by date instead.
Deleting and restoring need the server; clients disable them offline.
