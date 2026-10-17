# KeePassXC accounts, sharing and service accounts (1Password/Bitwarden-style)

## Context

The `kpnx` workspace already has these parts, but they are not connected:

| Part | Path | What it is today |
|---|---|---|
| Qt desktop | `keepassxc/` (branch `feature/auto-sync`) | Syncs a whole .kdbx with the sync server: `SyncEngine` does a three-way field merge, uses ETags, live updates over SSE, version history, and stores tokens in the OS keychain |
| Sync server | `keepassxc-sync-server/` (Drogon C++) | Stores encrypted .kdbx blobs per user, with ETag optimistic locking and history. Storage is SQLite/filesystem or Postgres+S3. Auth is **per-device opaque tokens created by an admin CLI** |
| Auth server | `keepassxc-auth/` (Flask SaaS starter) | HTML login with the password sent to the server, cookie sessions, Stripe billing, admin. **No JSON API, no tokens, no keys, no migrations** |
| Web app | `keepassxc-web/` (Next 16 + kdbxweb) | Local-only PWA that opens .kdbx files from disk/IndexedDB, with **no network code** |

**Goal:** a single account that unlocks every vault on Qt and the web, live sync, vaults shared with people and orgs, and service accounts for CI, servers and Kubernetes. The server must never be able to read secrets.

**Decisions made:** zero-knowledge crypto (OPAQUE + Secret Key, wrapped vault keys); the Flask server issues EdDSA JWTs and the sync server verifies them without calling Flask; Qt and a web-only browser client (no Tauri/Capacitor); service accounts get CLI read/`run`, write/rotate, REST/SDK, GitHub Actions and Kubernetes.

## Modes and server choice (Qt and web)

There are three ways to use the apps. **Only the hosted KeePassXC instance runs the auth server.** A self-hosted instance is the sync server on its own, with no accounts.

| Mode | Server | Login | Keys | Sharing / orgs / service accounts |
|---|---|---|---|---|
| **KeePassXC account** (default) | Hosted: Flask auth + sync server | OPAQUE + Secret Key → JWT | Random vault keys, sealed per member | Yes |
| **Self-hosted sync** | The user's own `sync_server` only | Device token from a pairing code (or from the admin CLI) | The .kdbx master password, as today | No |
| **No account** | None | None | The .kdbx master password | No |

**Start screen:**
- A sign-in form for the hosted instance: email, master password, and the Secret Key the first time on a device.
- **Create account**
- **Use my own sync server**: asks for the server URL and a pairing code, then gives the existing per-database sync server setup.
- **Use without an account**: opens today's local experience, unchanged.

**Self-hosted sync and no-account modes** are full modes, not something to deprecate:
- **Qt:** open/create a local .kdbx with a master password, plus the existing Remote Sync options (command-based remote, and the sync server with **device tokens and pairing codes**). This is `feature/auto-sync` as it stands. Self-hosted sync only pre-fills the server URL and token for new databases.
- **Web:** local files and IndexedDB as today (`UploadScreen`). In self-hosted mode it also claims a pairing code and syncs through the SDK sync client using a device token, with the same ETag, merge and SSE logic as account vaults.
- Users can switch at any time: "Sign in" from either mode, then "Move to account" on each database, which re-keys it to a vault key and uploads it to the hosted instance. Signing out goes back to the start screen, and local databases stay usable.
- Settings remember the last choice, so the start screen is skipped on later launches.

**Server endpoints and defaults:**
- The hosted URL is a build-time default: Qt has a CMake option `KPXC_DEFAULT_ACCOUNT_SERVER`; the web app has `NEXT_PUBLIC_DEFAULT_SERVER`. The hosted Flask server publishes `GET /.well-known/kpx-configuration` → `{issuer, api, sync, jwks_uri, opaque}`, so clients find its sync server.
- A self-hosted sync server is configured by its URL; clients check it with the existing `GET /api/v1/health`.
- The sync server's `SYNC_ALLOWED_ORIGINS` lets a self-hosted server be reached from the hosted web app; the README documents it.

## Core design

### Keys (the same spec everywhere; written to `keepassxc/docs/plans/accounts-and-sharing.md` with test vectors)
- **Account Unlock Key (AUK)** = HKDF-SHA256(Argon2id(master password, salt), Secret Key). The Secret Key is 128 random bits, shown once in an "Emergency Kit" and kept in the device keychain once that device is trusted.
- **Keyset per principal:** an X25519 encryption keypair and an Ed25519 signing keypair. The private halves are encrypted with the AUK (XChaCha20-Poly1305) and stored on the auth server. Public keys are readable by org members.
- **Vault = one .kdbx on the sync server.** Its composite key is a **random 256-bit vault key used as a key-file component**: Qt uses `FileKey` from an in-memory `QBuffer`, the web uses kdbxweb `Credentials(null, keyBytes)`. The KDF cost can be low because the key has full entropy. This keeps the existing whole-file sync, merge and history untouched.
- **Vault access** = the vault key sealed to each member's public key (ephemeral X25519 → HKDF → XChaCha20-Poly1305, "kpx-seal-v1"), stored on the auth server as `vault_access(vault, principal, sealed_key, role)`.
- **Revoking a member** happens on the client: it rotates the vault key (re-keys the .kdbx and re-seals it for the remaining members) in one step.
- **Libraries:** Botan in Qt (already a dependency; it has X25519, Ed25519, HKDF, XChaCha20-Poly1305 and Argon2), `@noble/curves`, `@noble/ciphers` and `@noble/hashes` in TS, and **libopaque** (C core, with Python and WASM bindings) for OPAQUE in all three.

### Tokens
- Flask signs **EdDSA JWTs** valid for 10 minutes and publishes `/.well-known/jwks.json`. Claims: `sub` (`user:<uuid>` or `sa:<uuid>`), `dev`, `org`, `vlt: [{id, r: read|write|manage}]` and `jti`.
- Refresh tokens are opaque, rotating, bound to one device and stored hashed. Clients refresh after any membership change, which arrives as an SSE event.
- **Service account token** = `kpx_sa_` + base64url(`{v, url, sa_id, secret, kek}`). The server stores only a hash of `secret` and the SA keyset encrypted with `kek`, so the server can never unwrap it. Exchanging it at `POST /api/v1/auth/service-account` returns a JWT plus the encrypted keyset.

## Phase 1: auth server (`keepassxc-auth/`)
1. Write the **first Alembic baseline** of the current schema in `db/versions/`. Fix the latent bugs that block an API: `load_user` crashes when the user is None, `distutils.strtobool` in `config/settings.py`, and `User.serialize_token`.
2. Switch to UUID public ids. New models live in new blueprints (`keepassxc/blueprints/api/`, `orgs/`, `vaults/`, `service_accounts/`) and reuse `ResourceMixin` from `lib/util_sqlalchemy.py`:
   - `users` gains `uuid`, `opaque_record`, `kdf_params`, `keyset` (encrypted private keys + public keys) and `totp_secret`. The `password` column is dropped once migration is done.
   - `devices`, `refresh_tokens`, `organizations`, `org_members(role owner|admin|member)`, `vaults(uuid, org_id|owner_id, encrypted_name)`, `vault_access(vault, principal_type, principal_id, sealed_key, role)`, `service_accounts(org, name, keyset, created_by, expires_at)`, `service_account_secrets(hash, prefix, last_used_at, expires_at)`, `audit_events`.
3. The JSON API blueprint `/api/v1` is CSRF-exempt, uses bearer auth through a `token_required(scope)` decorator next to `blueprints/user/decorators.py`, and is rate-limited with the existing Flask-Limiter (`@limiter.limit` on every auth route):
   - Auth: `auth/register/{start,finish}`, `auth/login/{start,finish}` (OPAQUE + optional TOTP), `auth/refresh`, `auth/logout`, `auth/service-account`
   - Keys and directory: `me`, `me/keyset`, `users/{id}/public-key`, `devices` (list/revoke)
   - Orgs, members, invitations; `vaults` CRUD; `vaults/{id}/access` (grant/revoke/rotate with sealed keys)
   - `service-accounts` CRUD, `service-accounts/{id}/secrets` (issue/revoke), `audit`
   - `/.well-known/jwks.json`, with key rotation via `flask keys rotate` in `cli/`
4. The HTML login/signup pages move to a small JS OPAQUE module in `assets/`, so the website never sees the password either. Billing and admin keep their cookie session, which is set from the JWT login.

## Phase 2: sync server (`keepassxc-sync-server/`)
1. Add a `JwtVerifier` (new `src/Jwt.{h,cpp}`) that fetches and caches the JWKS from `SYNC_AUTH_JWKS_URL`, refetches when it sees an unknown `kid`, and checks `iss`/`aud`/`exp`. `SyncApi::authenticate` (`src/SyncApi.cpp:232`) returns an `Identity` extended with `subject` and a `vault → role` map. `SYNC_AUTH_MODE=jwt|legacy|both`: **the default is `legacy`**, which is self-hosted and works exactly as today with no auth server. The hosted deployment sets `both` (or `jwt`) together with `SYNC_AUTH_JWKS_URL`. JWT code only runs when it is configured.
2. **Namespace by vault, not user:** `DatabaseRepository` (`src/Storage.h`) takes a vault id. Routes become `vaults/{id}` (GET/HEAD need read, PUT needs write, create/DELETE need manage). `events` is filtered to the token's vaults.
3. Add a `migrate-to-vaults` admin command in `src/main.cpp`, alongside `import`/`migrate`, for both the filesystem and Pg/S3 backends.
4. CORS for the web origin (`SYNC_ALLOWED_ORIGINS`) and expose `ETag`/`X-Database-Revision`.
5. Device pairing routes **stay**, because they are how self-hosted login works. Legacy databases keep the per-user namespace (`databases/{id}`) and account vaults use `vaults/{id}`, so the two modes never collide. The `vaults/*` routes exist only when JWT auth is on. On the hosted instance, `migrate-to-vaults` runs only for users moving from legacy to an account; "Move to account" from a self-hosted server is a client-side re-key and upload to the hosted instance.

## Phase 3: Qt desktop (`keepassxc/src/`)
1. New `src/account/`:
   - `AuthClient` (QNetworkAccessManager, same style as `SyncServerClient`)
   - `AccountManager` (OPAQUE login, keyset kept in memory and cleared on lock, JWT refresh, refresh token + Secret Key in `SyncTokenStore`)
   - `AccountCrypto` (Botan; the seal/unseal and AUK derivation from the spec)
   - `VaultKeyring` (vault id → `CompositeKey`)
2. `RemoteParams` (`src/gui/remote/RemoteSettings.h`) gains `Type::AccountVault` with `accountId` and `vaultId`. `SyncServerClient` takes a **token provider callback** instead of a static `serverToken`, so `SyncEngine`, `SyncScheduler` and `SyncEventStream` work unchanged.
3. UI: a **start screen** shown before the existing welcome widget (`src/gui/WelcomeWidget`), with hosted sign-in, Create account, **Use my own sync server** (URL + pairing code through the existing `SyncServerClient::claimPairingCode`, then today's flow with that server pre-filled), and **Use without an account**, which goes straight to today's welcome/open-database flow. A sign-up wizard with an Emergency Kit PDF; a sidebar listing account vaults in `src/gui/vault/`; "Unlock account" opens every vault with no per-vault password (upstream Quick Unlock/Touch ID wraps the AUK); a Share dialog (members, roles, revoke → rotate); "Move to account" re-keys an existing synced .kdbx to a fresh vault key and uploads it.
4. Tests: `tests/TestAccountCrypto.cpp` (spec vectors), `tests/TestAccountManager.cpp` (mocked `send()` as in `TestRemoteSyncServer`), and an extension of `tests/gui/TestGuiAutoSync.cpp`.

## Phase 4: shared TS SDK + web client
1. New `kpnx-sdk/` npm workspace at `/Users/bongani/kpnx/kpnx-sdk/packages/sdk`:
   - Crypto spec in TS, OPAQUE (libopaque wasm)
   - Auth and sync REST clients
   - Vault open/save via kdbxweb (moved out of `keepassxc-web/src/lib/keepass-crypto.ts`)
   - **A port of the three-way merge** from `SyncEngine.cpp` (`resolveFields`/`combineEntries`), so web, CLI and Qt resolve conflicts the same way, using the same base copy
2. `keepassxc-web`:
   - A start screen (new `src/components/StartScreen.tsx`) with hosted sign-in, sign-up, **Use my own sync server** (URL + pairing code, then local databases each linked to a server database id, and the device token kept in IndexedDB), and **Use without an account**, which goes to the existing `UploadScreen`. `KeePassContext` gains `mode: 'account' | 'selfhosted' | 'local'`; vault list from `/vaults`
   - Open/save through the SDK, keeping the base copy and ETag in IndexedDB (`src/lib/db-cache.ts`)
   - Conditional PUT, with merge and retry on 412; live updates over SSE
   - Share and org admin screens; service-account creation (the keypair is generated in the browser and the token shown once)
   - Local-file mode stays as it is
   - Update `README.md`/`contribution.md`: "only encrypted blobs and sealed keys leave the device". Remove `@vercel/analytics`.

## Phase 5: service accounts (all built on the SDK)
- **`kpx` CLI** (`packages/cli`, shipped as a single binary): `kpx read kpx://<vault>/<entry>/<field>`, `kpx run --env-file .env.tpl -- cmd` (replaces references and masks output), `kpx inject`, `kpx item create|edit`, `kpx rotate <ref> --generate`. Reads `KPX_SERVICE_ACCOUNT_TOKEN`; human login also works.
- **Writes** use the same conditional PUT and merge/retry loop; vault roles limit them to `write`.
- **`kpx connect`** (`packages/connect`): a self-hosted REST server holding one SA token. It offers `GET/PUT /v1/vaults/{id}/items/...` with its own bearer tokens, giving REST access to any language.
- **GitHub Action** (`packages/github-action`, a JS action): loads secrets into the environment and masks them.
- **Kubernetes:** an External Secrets Operator **webhook provider** pointed at `kpx connect` (manifests and Helm values in `packages/connect/deploy/`), with no custom operator in v1.
- Admin UI on web and Qt: create an SA, grant vaults with read/write, issue/revoke/expire secrets, view the audit log.

## Phase 6: hardening
Key rotation on revoke enforced by the server (an access change that leaves an old vault key is rejected once members are removed), a per-vault audit trail from sync server logs (`sub`), TOTP then WebAuthn MFA, public-key fingerprint verification for sharing, an optional org account-recovery key, and a JWKS key rollover runbook.

## Verification
- **Crypto interop:** the shared JSON test vectors pass in Qt (`ctest -R AccountCrypto`) and TS (`vitest`); Python only checks OPAQUE.
- **Auth server:** `./run flask db upgrade`, then `./run test` with new `test/blueprints/api/*` (register/login/refresh/vault access/SA exchange; the server never receives a password or vault key in the payload).
- **Sync server:** extend `sync_server/test/ApiTest.cpp` with JWT cases (bad signature, expired token, wrong vault, read-only PUT gets 403, SSE filtering) and run with `ctest`.
- **End to end:** `docker compose` with Flask + Postgres + sync server:
  1. Sign up in the web app, then sign in on Qt and see the same vault.
  2. Edit on both and confirm the merge.
  3. Share with a second user, then revoke and confirm the key rotated and the old key fails.
  4. Create an SA, then run `kpx run -- env` and `kpx rotate` and see the change on Qt.
  5. Run the GitHub Action on a test repo.
  - **Modes:** "Use without an account" on Qt and the web opens a local .kdbx with no network calls (check the browser devtools network tab). Run `sync_server` alone with its default `compose.yaml` (no Flask, `SYNC_AUTH_MODE=legacy`). Pick "Use my own sync server" on Qt and the web, claim pairing codes, edit the same database on both, and confirm the merge. The existing `ApiTest.cpp` and `TestRemoteSyncServer` still pass unchanged. "Move to account" converts both a local and a self-hosted database into a hosted vault.
  6. On a kind cluster, ESO syncs a secret through `kpx connect`.
