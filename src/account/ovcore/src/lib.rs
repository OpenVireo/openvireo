// Copyright (C) 2026 Bongani Mbigi
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 2 or (at
// your option) version 3 of the License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.

//! The desktop's link to openvireo-core.
//!
//! Linking this one static library brings in two C++ APIs:
//!
//! - `ov::` (`<ovcore.h>`): the core facade and its Host interfaces, from the core's own C++ binding
//!   (`bindings/ov-ffi-cpp`), re-exported unchanged.
//! - `ovd::` (`<ovdesktop.h>`, this file): the lower-level core functions the desktop's account code
//!   uses directly. They sit below the facade, so the facade binding doesn't bridge them.
//!   OWNER-REQUEST (W3-ID → W1-BC owner): bridge these in ov-ffi-cpp, then delete this bridge.
//!
//! Errors throw `rust::Error` whose `what()` is `"<wire code>: <message>"`, the ov-ffi-cpp
//! convention; `AccountCrypto` maps the codes it cares about. Secrets cross as `rust::Vec<uint8_t>`
//! and the caller wipes them. Every function is CPU-only and synchronous; the OPAQUE steps and the
//! Argon2id calls take long enough to keep them off the GUI thread.

// The facade binding: its `extern "C"` symbols and its C++ half are linked from here.
pub use ov_ffi_cpp;

use ov_crypto::{curves, envelope, keyset, primitives, secret_key, tokens, vault_name};
use ov_error::{OvError, OvErrorCode};
use zeroize::Zeroizing;

#[cxx::bridge(namespace = "ovd")]
mod ffi {
    /// The two messages of an OPAQUE step: what goes to the server, and the state the next step needs.
    struct OpaqueStart {
        /// `request` (registration) or `ke1` (login)
        message: Vec<u8>,
        /// Secret: wipe it
        state: Vec<u8>,
    }

    struct OpaqueLogin {
        auth_u: Vec<u8>,
        session_key: Vec<u8>,
        export_key: Vec<u8>,
    }

    /// The server half of an OPAQUE step (tests only).
    struct OpaqueServerStep {
        message: Vec<u8>,
        state: Vec<u8>,
    }

    /// A keyset's public half (SPEC §5): X25519 and Ed25519 public keys and the self-signature.
    struct PublicKeys {
        enc_pk: Vec<u8>,
        sign_pk: Vec<u8>,
        sig: Vec<u8>,
    }

    struct ServiceAccountToken {
        url: String,
        sa_id: String,
        secret: Vec<u8>,
        kek: Vec<u8>,
    }

    /// One change the sync merge made (ov-merge `MergeChange`).
    struct MergeChange {
        /// 0 added, 1 modified, 2 moved, 3 deleted, 4 metadata
        change: u8,
        /// 0 entry, 1 group, 2 database
        subject: u8,
        /// 16 bytes, or empty for the database
        uuid: Vec<u8>,
        details: String,
    }

    struct MergeResult {
        local: Vec<u8>,
        remote: Vec<u8>,
        local_changed: bool,
        remote_changed: bool,
        changes: Vec<MergeChange>,
    }

    extern "Rust" {
        // ---- ov v1 crypto (ov-crypto). Fixed nonces/ephemeral keys are for the test vectors only;
        // pass them empty to get fresh random ones.
        fn nfkc_utf8(text: &[u8]) -> Result<Vec<u8>>;
        fn generate_secret_key() -> Result<Vec<u8>>;
        fn format_secret_key(raw: &[u8]) -> Result<String>;
        fn parse_secret_key(text: &str) -> Result<Vec<u8>>;
        fn wrap(key: &[u8], plaintext: &[u8], aad: &[u8], nonce: &[u8]) -> Result<Vec<u8>>;
        fn unwrap(key: &[u8], wrapped: &[u8], aad: &[u8]) -> Result<Vec<u8>>;
        fn seal(
            recipient_pk: &[u8],
            plaintext: &[u8],
            aad: &[u8],
            ephemeral_sk: &[u8],
            nonce: &[u8],
        ) -> Result<Vec<u8>>;
        fn unseal(recipient_sk: &[u8], sealed: &[u8], aad: &[u8]) -> Result<Vec<u8>>;
        fn seal_derived_key(recipient_sk: &[u8], sealed: &[u8]) -> Result<Vec<u8>>;
        fn x25519_public_key(secret_key: &[u8]) -> Result<Vec<u8>>;
        fn x25519_shared(secret_key: &[u8], public_key: &[u8]) -> Result<Vec<u8>>;
        fn ed25519_public_key(seed: &[u8]) -> Result<Vec<u8>>;
        fn ed25519_sign(seed: &[u8], message: &[u8]) -> Result<Vec<u8>>;
        fn ed25519_verify(public_key: &[u8], message: &[u8], signature: &[u8]) -> bool;
        fn keyset_from_secrets(enc_sk: &[u8], sign_seed: &[u8]) -> Result<PublicKeys>;
        fn verify_public_keys(enc_pk: &[u8], sign_pk: &[u8], sig: &[u8]) -> bool;
        fn fingerprint(enc_pk: &[u8], sign_pk: &[u8]) -> String;
        fn vault_name_key(vault_key: &[u8]) -> Result<Vec<u8>>;
        fn refresh_token_hash(token: &str) -> String;
        fn build_service_account_token(url: &str, sa_id: &str, secret: &[u8], kek: &[u8]) -> Result<String>;
        fn parse_service_account_token(text: &str) -> Result<ServiceAccountToken>;
        fn service_account_secret_hash(secret: &[u8]) -> Result<String>;
        fn service_account_secret_prefix(secret: &[u8]) -> Result<String>;

        // ---- OPAQUE client (ov-opaque, SPEC §8). `password` is UTF-8; it is NFKC-normalised here.
        fn opaque_start_registration(password: &[u8]) -> Result<OpaqueStart>;
        fn opaque_finish_registration(state: &[u8], response: &[u8], id_u: &[u8], id_s: &[u8]) -> Result<Vec<u8>>;
        fn opaque_start_login(password: &[u8]) -> Result<OpaqueStart>;
        fn opaque_finish_login(
            state: &[u8],
            ke2: &[u8],
            id_u: &[u8],
            id_s: &[u8],
            context: &[u8],
        ) -> Result<OpaqueLogin>;

        // ---- tests only (built with the `test-server` feature; otherwise they fail)
        fn opaque_start_login_seeded(password: &[u8], seed: &[u8]) -> Result<OpaqueStart>;
        fn opaque_finish_login_seeded(
            state: &[u8],
            ke2: &[u8],
            id_u: &[u8],
            id_s: &[u8],
            context: &[u8],
            seed: &[u8],
        ) -> Result<OpaqueLogin>;
        fn opaque_server_registration_response(request: &[u8]) -> Result<OpaqueServerStep>;
        fn opaque_server_store_record(setup: &[u8], record: &[u8]) -> Result<Vec<u8>>;
        fn opaque_server_credential_response(
            ke1: &[u8],
            stored_record: &[u8],
            id_u: &[u8],
            id_s: &[u8],
            context: &[u8],
        ) -> Result<OpaqueServerStep>;
        fn opaque_server_user_auth(state: &[u8], auth_u: &[u8]) -> Result<Vec<u8>>;

        // ---- the sync merge (ov-merge on ov-kdbx), bytes in, bytes out
        //
        // Each copy is a KDBX file with its key, given as the 32-byte raw components of the
        // KeePassXC composite key in order (one or two: password and/or key file). `base` empty
        // means no common base. Returns both copies merged, written with their own key and KDF.
        fn merge_kdbx(
            local: &[u8],
            local_key: &[u8],
            remote: &[u8],
            remote_key: &[u8],
            base: &[u8],
            base_key: &[u8],
            now: i64,
        ) -> Result<MergeResult>;
    }
}

/// A failure, thrown in C++ as `rust::Error` with `what()` = `"<wire code>: <message>"`.
#[derive(Debug)]
pub struct Error(OvError);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.0.wire_code(), self.0.message)
    }
}

impl From<OvError> for Error {
    fn from(e: OvError) -> Self {
        Error(e)
    }
}

impl From<ov_opaque::OpaqueError> for Error {
    fn from(e: ov_opaque::OpaqueError) -> Self {
        Error(e.into())
    }
}

type R<T> = Result<T, Error>;

fn invalid(what: impl Into<String>) -> Error {
    Error(OvError::new(OvErrorCode::InvalidRequest, what.into()))
}

/// Panics never cross into C++: they become `internal`.
fn edge<T>(f: impl FnOnce() -> R<T>) -> R<T> {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(r) => r,
        Err(_) => Err(Error(OvError::internal("openvireo-core panicked"))),
    }
}

fn array<const N: usize>(what: &str, bytes: &[u8]) -> R<Zeroizing<[u8; N]>> {
    let arr: [u8; N] =
        bytes.try_into().map_err(|_| invalid(format!("{what} must be {N} bytes, got {}", bytes.len())))?;
    Ok(Zeroizing::new(arr))
}

fn optional_array<const N: usize>(what: &str, bytes: &[u8]) -> R<Option<Zeroizing<[u8; N]>>> {
    if bytes.is_empty() {
        Ok(None)
    } else {
        array::<N>(what, bytes).map(Some)
    }
}

fn utf8<'a>(what: &str, bytes: &'a [u8]) -> R<&'a str> {
    std::str::from_utf8(bytes).map_err(|_| invalid(format!("{what} isn't UTF-8")))
}

fn take(mut z: Zeroizing<Vec<u8>>) -> Vec<u8> {
    std::mem::take(&mut *z)
}

// ------------------------------------------------------------------------------------- crypto

fn nfkc_utf8(text: &[u8]) -> R<Vec<u8>> {
    edge(|| Ok(take(ov_crypto::auk::password_bytes(utf8("the text", text)?))))
}

fn generate_secret_key() -> R<Vec<u8>> {
    edge(|| Ok(secret_key::generate_secret_key()?.to_vec()))
}

fn format_secret_key(raw: &[u8]) -> R<String> {
    edge(|| {
        let raw = array::<{ secret_key::SECRET_KEY_BYTES }>("a Secret Key", raw)?;
        Ok(secret_key::format_secret_key(&raw).to_string())
    })
}

fn parse_secret_key(text: &str) -> R<Vec<u8>> {
    edge(|| Ok(secret_key::parse_secret_key(text)?.to_vec()))
}

fn wrap(key: &[u8], plaintext: &[u8], aad: &[u8], nonce: &[u8]) -> R<Vec<u8>> {
    edge(|| {
        let key = array::<{ primitives::KEY_BYTES }>("the key", key)?;
        let aad = utf8("the AAD", aad)?;
        match optional_array::<{ primitives::NONCE_BYTES }>("the nonce", nonce)? {
            Some(nonce) => Ok(envelope::wrap_with_nonce(&key, plaintext, aad, &nonce)),
            None => Ok(envelope::wrap(&key, plaintext, aad)?),
        }
    })
}

fn unwrap(key: &[u8], wrapped: &[u8], aad: &[u8]) -> R<Vec<u8>> {
    edge(|| {
        let key = array::<{ primitives::KEY_BYTES }>("the key", key)?;
        Ok(take(envelope::unwrap(&key, wrapped, utf8("the AAD", aad)?)?))
    })
}

fn seal(recipient_pk: &[u8], plaintext: &[u8], aad: &[u8], ephemeral_sk: &[u8], nonce: &[u8]) -> R<Vec<u8>> {
    edge(|| {
        let opts = envelope::SealOptions {
            ephemeral_secret_key: optional_array::<32>("the ephemeral key", ephemeral_sk)?,
            nonce: optional_array::<{ primitives::NONCE_BYTES }>("the nonce", nonce)?.map(|n| *n),
        };
        Ok(envelope::seal_with(recipient_pk, plaintext, utf8("the AAD", aad)?, &opts)?)
    })
}

fn unseal(recipient_sk: &[u8], sealed: &[u8], aad: &[u8]) -> R<Vec<u8>> {
    edge(|| {
        let sk = array::<32>("the secret key", recipient_sk)?;
        Ok(take(envelope::unseal(&sk, sealed, utf8("the AAD", aad)?)?))
    })
}

fn seal_derived_key(recipient_sk: &[u8], sealed: &[u8]) -> R<Vec<u8>> {
    edge(|| {
        let sk = array::<32>("the secret key", recipient_sk)?;
        Ok(envelope::seal_derived_key(&sk, sealed)?.to_vec())
    })
}

fn x25519_public_key(secret_key: &[u8]) -> R<Vec<u8>> {
    edge(|| Ok(curves::x25519_public_key(&*array::<32>("the secret key", secret_key)?).to_vec()))
}

fn x25519_shared(secret_key: &[u8], public_key: &[u8]) -> R<Vec<u8>> {
    edge(|| Ok(curves::x25519_shared(&*array::<32>("the secret key", secret_key)?, public_key)?.to_vec()))
}

fn ed25519_public_key(seed: &[u8]) -> R<Vec<u8>> {
    edge(|| Ok(curves::ed25519_public_key(&*array::<32>("the seed", seed)?).to_vec()))
}

fn ed25519_sign(seed: &[u8], message: &[u8]) -> R<Vec<u8>> {
    edge(|| Ok(curves::ed25519_sign(message, &*array::<32>("the seed", seed)?).to_vec()))
}

fn ed25519_verify(public_key: &[u8], message: &[u8], signature: &[u8]) -> bool {
    edge(|| Ok(curves::ed25519_verify(signature, message, public_key))).unwrap_or(false)
}

/// The public keys and self-signature the two private halves of a keyset make (SPEC §5).
fn keyset_from_secrets(enc_sk: &[u8], sign_seed: &[u8]) -> R<ffi::PublicKeys> {
    edge(|| {
        let ks = keyset::keyset_from_secrets(&*array::<32>("enc_sk", enc_sk)?, &*array::<32>("sign_seed", sign_seed)?);
        Ok(ffi::PublicKeys {
            enc_pk: ks.public.enc_pk.to_vec(),
            sign_pk: ks.public.sign_pk.to_vec(),
            sig: ks.public.sig.to_vec(),
        })
    })
}

fn verify_public_keys(enc_pk: &[u8], sign_pk: &[u8], sig: &[u8]) -> bool {
    edge(|| {
        let pk = keyset::PublicKeys {
            enc_pk: array::<32>("enc_pk", enc_pk).map(|a| *a)?,
            sign_pk: array::<32>("sign_pk", sign_pk).map(|a| *a)?,
            sig: sig.try_into().map_err(|_| invalid("sig must be 64 bytes"))?,
        };
        Ok(keyset::verify_public_keys(&pk))
    })
    .unwrap_or(false)
}

fn fingerprint(enc_pk: &[u8], sign_pk: &[u8]) -> String {
    keyset::fingerprint(enc_pk, sign_pk)
}

fn vault_name_key(vault_key: &[u8]) -> R<Vec<u8>> {
    edge(|| Ok(vault_name::vault_name_key(&*array::<32>("the vault key", vault_key)?).to_vec()))
}

fn refresh_token_hash(token: &str) -> String {
    tokens::refresh_token_hash(token)
}

fn build_service_account_token(url: &str, sa_id: &str, secret: &[u8], kek: &[u8]) -> R<String> {
    edge(|| {
        let secret = array::<32>("the secret", secret)?;
        let kek = array::<32>("the KEK", kek)?;
        Ok(tokens::build_sa_token(url, sa_id, &secret, &kek).to_string())
    })
}

fn parse_service_account_token(text: &str) -> R<ffi::ServiceAccountToken> {
    edge(|| {
        let t = tokens::parse_sa_token(text)?;
        Ok(ffi::ServiceAccountToken {
            url: t.url.clone(),
            sa_id: t.sa_id.clone(),
            secret: t.secret.to_vec(),
            kek: t.kek.to_vec(),
        })
    })
}

fn service_account_secret_hash(secret: &[u8]) -> R<String> {
    edge(|| Ok(tokens::sa_secret_hash(&*array::<32>("the secret", secret)?)))
}

fn service_account_secret_prefix(secret: &[u8]) -> R<String> {
    edge(|| Ok(tokens::sa_secret_prefix(&*array::<32>("the secret", secret)?)))
}

// ------------------------------------------------------------------------------------- OPAQUE

fn ids(id_u: &[u8], id_s: &[u8]) -> R<ov_opaque::OpaqueIds> {
    Ok(ov_opaque::OpaqueIds { id_u: utf8("idU", id_u)?.to_string(), id_s: utf8("idS", id_s)?.to_string() })
}

fn opaque_start_registration(password: &[u8]) -> R<ffi::OpaqueStart> {
    edge(|| {
        let r = ov_opaque::start_registration(&mut rand_core::OsRng, utf8("the password", password)?)?;
        Ok(ffi::OpaqueStart { message: r.request, state: take(r.state.to_bytes()) })
    })
}

fn opaque_finish_registration(state: &[u8], response: &[u8], id_u: &[u8], id_s: &[u8]) -> R<Vec<u8>> {
    edge(|| {
        let state = ov_opaque::ClientRegistrationState::from_bytes(state)?;
        let r = ov_opaque::finish_registration(&mut rand_core::OsRng, state, response, &ids(id_u, id_s)?)?;
        Ok(r.record)
    })
}

fn opaque_start_login(password: &[u8]) -> R<ffi::OpaqueStart> {
    edge(|| {
        let r = ov_opaque::start_login(&mut rand_core::OsRng, utf8("the password", password)?)?;
        Ok(ffi::OpaqueStart { message: r.ke1, state: take(r.state.to_bytes()) })
    })
}

fn login_result(r: ov_opaque::LoginFinish) -> ffi::OpaqueLogin {
    ffi::OpaqueLogin { auth_u: r.auth_u, session_key: take(r.session_key), export_key: take(r.export_key) }
}

fn opaque_finish_login(state: &[u8], ke2: &[u8], id_u: &[u8], id_s: &[u8], context: &[u8]) -> R<ffi::OpaqueLogin> {
    edge(|| {
        let state = ov_opaque::ClientLoginState::from_bytes(state)?;
        let r = ov_opaque::finish_login(
            &mut rand_core::OsRng,
            state,
            ke2,
            &ids(id_u, id_s)?,
            utf8("the context", context)?,
        )?;
        Ok(login_result(r))
    })
}

#[cfg(feature = "test-server")]
mod test_server {
    use super::*;
    use rand_chacha::rand_core::SeedableRng;
    use rand_chacha::ChaCha20Rng;

    fn rng(seed: &[u8]) -> R<ChaCha20Rng> {
        Ok(ChaCha20Rng::from_seed(*array::<32>("the seed", seed)?))
    }

    pub(crate) fn start_login_seeded(password: &[u8], seed: &[u8]) -> R<ffi::OpaqueStart> {
        let r = ov_opaque::start_login(&mut rng(seed)?, utf8("the password", password)?)?;
        Ok(ffi::OpaqueStart { message: r.ke1, state: take(r.state.to_bytes()) })
    }

    pub(crate) fn finish_login_seeded(
        state: &[u8],
        ke2: &[u8],
        id_u: &[u8],
        id_s: &[u8],
        context: &[u8],
        seed: &[u8],
    ) -> R<ffi::OpaqueLogin> {
        let state = ov_opaque::ClientLoginState::from_bytes(state)?;
        let r = ov_opaque::finish_login(&mut rng(seed)?, state, ke2, &ids(id_u, id_s)?, utf8("the context", context)?)?;
        Ok(login_result(r))
    }

    pub(crate) fn registration_response(request: &[u8]) -> R<ffi::OpaqueServerStep> {
        let r = ov_opaque::server::registration_response(&mut rand_core::OsRng, request)?;
        Ok(ffi::OpaqueServerStep { message: r.response, state: take(r.setup) })
    }

    pub(crate) fn store_record(setup: &[u8], record: &[u8]) -> R<Vec<u8>> {
        Ok(take(ov_opaque::server::store_record(setup, record)?))
    }

    pub(crate) fn credential_response(
        ke1: &[u8],
        stored_record: &[u8],
        id_u: &[u8],
        id_s: &[u8],
        context: &[u8],
    ) -> R<ffi::OpaqueServerStep> {
        let r = ov_opaque::server::credential_response(
            &mut rand_core::OsRng,
            ke1,
            stored_record,
            &ids(id_u, id_s)?,
            utf8("the context", context)?,
        )?;
        Ok(ffi::OpaqueServerStep { message: r.ke2, state: take(r.state) })
    }

    pub(crate) fn user_auth(state: &[u8], auth_u: &[u8]) -> R<Vec<u8>> {
        Ok(take(ov_opaque::server::user_auth(state, auth_u)?))
    }
}

#[cfg(not(feature = "test-server"))]
mod test_server {
    use super::*;

    fn off<T>() -> R<T> {
        Err(Error(OvError::new(OvErrorCode::Internal, "built without the test-server feature")))
    }
    pub(crate) fn start_login_seeded(_: &[u8], _: &[u8]) -> R<ffi::OpaqueStart> {
        off()
    }
    pub(crate) fn finish_login_seeded(
        _: &[u8],
        _: &[u8],
        _: &[u8],
        _: &[u8],
        _: &[u8],
        _: &[u8],
    ) -> R<ffi::OpaqueLogin> {
        off()
    }
    pub(crate) fn registration_response(_: &[u8]) -> R<ffi::OpaqueServerStep> {
        off()
    }
    pub(crate) fn store_record(_: &[u8], _: &[u8]) -> R<Vec<u8>> {
        off()
    }
    pub(crate) fn credential_response(_: &[u8], _: &[u8], _: &[u8], _: &[u8], _: &[u8]) -> R<ffi::OpaqueServerStep> {
        off()
    }
    pub(crate) fn user_auth(_: &[u8], _: &[u8]) -> R<Vec<u8>> {
        off()
    }
}

fn opaque_start_login_seeded(password: &[u8], seed: &[u8]) -> R<ffi::OpaqueStart> {
    edge(|| test_server::start_login_seeded(password, seed))
}

fn opaque_finish_login_seeded(
    state: &[u8],
    ke2: &[u8],
    id_u: &[u8],
    id_s: &[u8],
    context: &[u8],
    seed: &[u8],
) -> R<ffi::OpaqueLogin> {
    edge(|| test_server::finish_login_seeded(state, ke2, id_u, id_s, context, seed))
}

fn opaque_server_registration_response(request: &[u8]) -> R<ffi::OpaqueServerStep> {
    edge(|| test_server::registration_response(request))
}

fn opaque_server_store_record(setup: &[u8], record: &[u8]) -> R<Vec<u8>> {
    edge(|| test_server::store_record(setup, record))
}

fn opaque_server_credential_response(
    ke1: &[u8],
    stored_record: &[u8],
    id_u: &[u8],
    id_s: &[u8],
    context: &[u8],
) -> R<ffi::OpaqueServerStep> {
    edge(|| test_server::credential_response(ke1, stored_record, id_u, id_s, context))
}

fn opaque_server_user_auth(state: &[u8], auth_u: &[u8]) -> R<Vec<u8>> {
    edge(|| test_server::user_auth(state, auth_u))
}

// ------------------------------------------------------------------------------------- merge

/// The ov-kdbx key whose composite hash is KeePassXC's: SHA-256 of the components' raw keys in order.
fn composite_key(components: &[u8]) -> R<ov_kdbx::CompositeKey> {
    let (parts, rest) = components.as_chunks::<32>();
    if parts.is_empty() || parts.len() > 2 || !rest.is_empty() {
        return Err(invalid("a key is one or two 32-byte components"));
    }
    let mut key = ov_kdbx::CompositeKey::new().with_password_hash(&parts[0]);
    if let Some(second) = parts.get(1) {
        // A 32-byte key file is used as it is, so this is the second raw component
        key = key.with_key_file(second);
    }
    Ok(key)
}

fn merge_kdbx(
    local: &[u8],
    local_key: &[u8],
    remote: &[u8],
    remote_key: &[u8],
    base: &[u8],
    base_key: &[u8],
    now: i64,
) -> R<ffi::MergeResult> {
    edge(|| {
        let local_key = composite_key(local_key)?;
        let remote_key = composite_key(remote_key)?;
        let mut l = ov_kdbx::read(local, &local_key)?;
        let mut r = ov_kdbx::read(remote, &remote_key)?;
        let b = if base.is_empty() { None } else { Some(ov_kdbx::read(base, &composite_key(base_key)?)?) };
        let report = ov_merge::merge_in_place(&mut l, &mut r, b.as_ref(), now);
        let changes = report
            .changes
            .iter()
            .map(|c| ffi::MergeChange {
                change: match c.change {
                    ov_merge::ChangeType::Added => 0,
                    ov_merge::ChangeType::Modified => 1,
                    ov_merge::ChangeType::Moved => 2,
                    ov_merge::ChangeType::Deleted => 3,
                    ov_merge::ChangeType::Metadata => 4,
                },
                subject: match c.subject {
                    ov_merge::ChangeSubject::Entry => 0,
                    ov_merge::ChangeSubject::Group => 1,
                    ov_merge::ChangeSubject::Database => 2,
                },
                uuid: c.uuid.map(|u| u.as_bytes().to_vec()).unwrap_or_default(),
                details: c.details.clone(),
            })
            .collect();
        Ok(ffi::MergeResult {
            local: if report.local_changed { ov_kdbx::write(&l, &local_key)? } else { Vec::new() },
            remote: if report.remote_changed { ov_kdbx::write(&r, &remote_key)? } else { Vec::new() },
            local_changed: report.local_changed,
            remote_changed: report.remote_changed,
            changes,
        })
    })
}
