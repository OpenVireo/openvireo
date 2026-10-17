// Generates docs/plans/accounts/test-vectors.json for the KeePassXC account crypto spec (kpx v1).
// Every random input is fixed so other implementations can reproduce each output byte for byte.
import { ed25519, x25519 } from "@noble/curves/ed25519.js";
import { xchacha20poly1305 } from "@noble/ciphers/chacha.js";
import { hkdf } from "@noble/hashes/hkdf.js";
import { sha256 } from "@noble/hashes/sha2.js";
import { argon2id } from "@noble/hashes/argon2.js";
import * as kdbxweb from "kdbxweb";
import { writeFileSync } from "node:fs";

const utf8 = (s) => new TextEncoder().encode(s);
const hex = (b) => Buffer.from(b).toString("hex");
const unhex = (s) => new Uint8Array(Buffer.from(s, "hex"));
const b64u = (b) => Buffer.from(b).toString("base64url");
const cat = (...a) => { const o = new Uint8Array(a.reduce((n, x) => n + x.length, 0)); let i = 0; for (const x of a) { o.set(x, i); i += x.length; } return o; };
// Deterministic "random" bytes: SHA-256 counter stream over a label
const fixed = (label, n) => { let out = new Uint8Array(0); for (let i = 0; out.length < n; i++) out = cat(out, sha256(utf8(`kpx-test|${label}|${i}`))); return out.slice(0, n); };

// ---- Secret Key display format: "K1-" + Crockford base32 (26 chars) grouped 6-5-5-5-5
const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
function secretKeyFormat(raw) {
  let bits = ""; for (const b of raw) bits += b.toString(2).padStart(8, "0");
  bits = bits.padEnd(130, "0");
  let s = ""; for (let i = 0; i < 130; i += 5) s += CROCKFORD[parseInt(bits.slice(i, i + 5), 2)];
  return `K1-${s.slice(0, 6)}-${s.slice(6, 11)}-${s.slice(11, 16)}-${s.slice(16, 21)}-${s.slice(21, 26)}`;
}

// ---- Primitives from the spec
const VERSION = new Uint8Array([1]);
const wrap = (key, nonce, plaintext, aad) => cat(VERSION, nonce, xchacha20poly1305(key, nonce, utf8(aad)).encrypt(plaintext));
const unwrap = (key, blob, aad) => { if (blob[0] !== 1) throw new Error("version"); return xchacha20poly1305(key, blob.slice(1, 25), utf8(aad)).decrypt(blob.slice(25)); };
function seal(recipientPk, ephemeralSk, nonce, plaintext, aad) {
  const epk = x25519.getPublicKey(ephemeralSk);
  const shared = x25519.getSharedSecret(ephemeralSk, recipientPk);
  if (shared.every((b) => b === 0)) throw new Error("low-order point");
  const key = hkdf(sha256, shared, cat(epk, recipientPk), utf8("kpx-seal-v1"), 32);
  return { blob: cat(VERSION, epk, nonce, xchacha20poly1305(key, nonce, utf8(aad)).encrypt(plaintext)), key };
}
function unseal(recipientSk, blob, aad) {
  const epk = blob.slice(1, 33), nonce = blob.slice(33, 57);
  const shared = x25519.getSharedSecret(recipientSk, epk);
  const key = hkdf(sha256, shared, cat(epk, x25519.getPublicKey(recipientSk)), utf8("kpx-seal-v1"), 32);
  return xchacha20poly1305(key, nonce, utf8(aad)).decrypt(blob.slice(57));
}
const auk = (password, kdf, secretKeyRaw) => {
  const stretched = argon2id(utf8(password.normalize("NFKC")), kdf.saltBytes, { m: kdf.m, t: kdf.t, p: kdf.p, dkLen: 32 });
  return { stretched, auk: hkdf(sha256, stretched, secretKeyRaw, utf8("kpx-auk-v1"), 32) };
};
function keyset(label) {
  const encSk = fixed(`${label}/x25519`, 32), signSeed = fixed(`${label}/ed25519`, 32);
  const encPk = x25519.getPublicKey(encSk), signPk = ed25519.getPublicKey(signSeed);
  const sig = ed25519.sign(cat(utf8("kpx-pubkeys-v1"), encPk, signPk), signSeed);
  const fp = hex(sha256(cat(utf8("kpx-fp-v1"), encPk, signPk)).slice(0, 20)).match(/.{4}/g).join(" ");
  return { encSk, signSeed, encPk, signPk, sig, fp };
}

const vectors = { spec: "kpx v1", encoding: "binary values are base64url without padding unless the field name ends in _hex", generated_by: "gen.mjs with @noble/{curves,ciphers,hashes} 2.4.0 and kdbxweb 2.1.1" };

// 1. Secret Key
const skRaw = fixed("secret-key", 16);
vectors.secret_key = [{ raw_hex: hex(skRaw), display: secretKeyFormat(skRaw) },
  { raw_hex: "00000000000000000000000000000000", display: secretKeyFormat(new Uint8Array(16)) },
  { raw_hex: "ffffffffffffffffffffffffffffffff", display: secretKeyFormat(new Uint8Array(16).fill(255)) }];

// 2. AUK derivation (low cost params for tests; production defaults are m=65536 t=3 p=1)
vectors.auk = [];
for (const [password, m, t] of [["correct horse battery staple", 1024, 2], ["Ünïcödé pässwörd ﬁ", 1024, 2], ["correct horse battery staple", 65536, 3]]) {
  const kdf = { alg: "argon2id", saltBytes: fixed(`kdf-salt/${m}`, 16), m, t, p: 1 };
  const r = auk(password, kdf, skRaw);
  vectors.auk.push({ password, password_nfkc_hex: hex(utf8(password.normalize("NFKC"))), kdf: { alg: "argon2id", salt: b64u(kdf.saltBytes), m, t, p: 1 }, secret_key_hex: hex(skRaw), argon2id_hex: hex(r.stretched), auk_hex: hex(r.auk) });
}
const aukKey = unhex(vectors.auk[0].auk_hex);

// 3. Keyset of a user, wrapped with the AUK
const alice = keyset("alice"), bob = keyset("bob");
const wrappedKeyset = wrap(aukKey, fixed("alice/keyset-nonce", 24), cat(alice.encSk, alice.signSeed), "kpx-keyset-v1");
if (hex(unwrap(aukKey, wrappedKeyset, "kpx-keyset-v1")) !== hex(cat(alice.encSk, alice.signSeed))) throw new Error("keyset roundtrip");
const pub = (k) => ({ enc_pk: b64u(k.encPk), sign_pk: b64u(k.signPk), sig: b64u(k.sig), fingerprint: k.fp });
vectors.keyset = { principal: "user:alice", auk_hex: hex(aukKey), enc_sk_hex: hex(alice.encSk), sign_seed_hex: hex(alice.signSeed), nonce_hex: hex(fixed("alice/keyset-nonce", 24)), aad: "kpx-keyset-v1", public: pub(alice), wrapped: b64u(wrappedKeyset) };
vectors.public_keys = [{ principal: "user:bob", enc_sk_hex: hex(bob.encSk), sign_seed_hex: hex(bob.signSeed), ...pub(bob) }];

// 4. Vault key sealed to two members
const vaultId = "3f1c2a9e-7b4d-4e8a-9c21-5d6e7f809a1b";
const vaultKey = fixed("vault-key", 32);
vectors.seal = [alice, bob].map((k, i) => {
  const esk = fixed(`seal/${i}/ephemeral`, 32), nonce = fixed(`seal/${i}/nonce`, 24), aad = `kpx-vault-key|${vaultId}`;
  const { blob, key } = seal(k.encPk, esk, nonce, vaultKey, aad);
  if (hex(unseal(k.encSk, blob, aad)) !== hex(vaultKey)) throw new Error("seal roundtrip");
  return { recipient_enc_pk: b64u(k.encPk), recipient_enc_sk_hex: hex(k.encSk), ephemeral_sk_hex: hex(esk), ephemeral_pk_hex: hex(x25519.getPublicKey(esk)), nonce_hex: hex(nonce), aad, plaintext_hex: hex(vaultKey), derived_key_hex: hex(key), sealed: b64u(blob) };
});
vectors.seal_negative = [
  { case: "wrong aad (other vault)", sealed: vectors.seal[0].sealed, recipient_enc_sk_hex: hex(alice.encSk), aad: "kpx-vault-key|00000000-0000-0000-0000-000000000000", expect: "decryption failure" },
  { case: "wrong recipient", sealed: vectors.seal[0].sealed, recipient_enc_sk_hex: hex(bob.encSk), aad: vectors.seal[0].aad, expect: "decryption failure" },
  { case: "unknown version byte", sealed: b64u(cat(new Uint8Array([2]), unhex(Buffer.from(vectors.seal[0].sealed, "base64url").toString("hex")).slice(1))), recipient_enc_sk_hex: hex(alice.encSk), aad: vectors.seal[0].aad, expect: "unsupported version" },
];

// 5. Encrypted vault name
const nameKey = hkdf(sha256, vaultKey, new Uint8Array(0), utf8("kpx-vault-name-v1"), 32);
vectors.vault_name = { vault_id: vaultId, vault_key_hex: hex(vaultKey), name_key_hex: hex(nameKey), name: "Team Infrastructure", nonce_hex: hex(fixed("vault-name-nonce", 24)), aad: `kpx-vault-name|${vaultId}`, encrypted_name: b64u(wrap(nameKey, fixed("vault-name-nonce", 24), utf8("Team Infrastructure"), `kpx-vault-name|${vaultId}`)) };

// 6. Service account token and keys
const saId = "9b2e4f61-0c3a-4d7e-8f15-a2b3c4d5e6f7", sa = keyset("sa"), saMaster = fixed("sa/master", 32), secret = fixed("sa/secret", 32), kek = fixed("sa/kek", 32);
const tokenPayload = JSON.stringify({ v: 1, url: "https://accounts.keepassxc.example", sa_id: saId, secret: b64u(secret), kek: b64u(kek) });
const masterAad = `kpx-sa-master-v1|${saId}`;
const sealedMasterToAlice = seal(alice.encPk, fixed("sa/master-seal-eph", 32), fixed("sa/master-seal-nonce", 24), saMaster, masterAad).blob;
vectors.service_account = {
  sa_id: saId, payload_json: tokenPayload, token: "kpx_sa_" + b64u(utf8(tokenPayload)),
  secret_hash_hex: hex(sha256(secret)), secret_prefix: b64u(secret).slice(0, 8),
  master_key_hex: hex(saMaster), kek_hex: hex(kek),
  wrapped_master: b64u(wrap(kek, fixed("sa/master-nonce", 24), saMaster, masterAad)), wrapped_master_nonce_hex: hex(fixed("sa/master-nonce", 24)), wrapped_master_aad: masterAad,
  keyset_enc_sk_hex: hex(sa.encSk), keyset_sign_seed_hex: hex(sa.signSeed), public: pub(sa),
  wrapped_keyset: b64u(wrap(saMaster, fixed("sa/keyset-nonce", 24), cat(sa.encSk, sa.signSeed), "kpx-keyset-v1")), wrapped_keyset_nonce_hex: hex(fixed("sa/keyset-nonce", 24)),
  master_sealed_to_admin: { admin: "user:alice", ephemeral_sk_hex: hex(fixed("sa/master-seal-eph", 32)), nonce_hex: hex(fixed("sa/master-seal-nonce", 24)), aad: masterAad, sealed: b64u(sealedMasterToAlice) },
};

// 7. Refresh token storage
const rt = "kpx_rt_" + b64u(fixed("refresh", 32));
vectors.refresh_token = { token: rt, stored_hash_hex: hex(sha256(utf8(rt))) };

// 8. JWT (EdDSA) and JWKS
const jwtSeed = fixed("jwt-signing-key", 32), kid = "test-2026-10";
const jwks = { keys: [{ kty: "OKP", crv: "Ed25519", x: b64u(ed25519.getPublicKey(jwtSeed)), kid, use: "sig", alg: "EdDSA" }] };
const mkJwt = (claims, seed = jwtSeed, headerKid = kid) => {
  const h = b64u(utf8(JSON.stringify({ alg: "EdDSA", typ: "JWT", kid: headerKid }))), p = b64u(utf8(JSON.stringify(claims)));
  return `${h}.${p}.${b64u(ed25519.sign(utf8(`${h}.${p}`), seed))}`;
};
const iat = 1792224000; // 2026-10-17T00:00:00Z
const userClaims = { iss: "https://accounts.keepassxc.example", aud: ["kpx-api", "kpx-sync"], sub: "user:5a0b7c1d-2e3f-4a5b-8c6d-7e8f9a0b1c2d", dev: "d4e5f6a7-b8c9-4d0e-9f1a-2b3c4d5e6f70", vlt: [{ id: vaultId, r: "manage" }, { id: "6c7d8e9f-0a1b-4c2d-8e3f-4a5b6c7d8e9f", r: "read" }], iat, nbf: iat, exp: iat + 600, jti: "0d3c1b2a-4f5e-4a6b-9c7d-8e9f0a1b2c3d" };
const saClaims = { iss: userClaims.iss, aud: ["kpx-api", "kpx-sync"], sub: `sa:${saId}`, org: "7e8f9a0b-1c2d-4e3f-8a5b-6c7d8e9f0a1b", vlt: [{ id: vaultId, r: "write" }], iat, nbf: iat, exp: iat + 600, jti: "1e2f3a4b-5c6d-4e7f-8a9b-0c1d2e3f4a5b" };
vectors.jwt = {
  signing_seed_hex: hex(jwtSeed), jwks, now: iat + 60,
  valid: [{ claims: userClaims, token: mkJwt(userClaims) }, { claims: saClaims, token: mkJwt(saClaims) }],
  invalid: [
    { case: "expired", token: mkJwt({ ...userClaims, exp: iat + 30 }), expect: 401 },
    { case: "not yet valid", token: mkJwt({ ...userClaims, nbf: iat + 3600 }), expect: 401 },
    { case: "wrong audience", token: mkJwt({ ...userClaims, aud: ["kpx-api"] }), expect: 401 },
    { case: "wrong issuer", token: mkJwt({ ...userClaims, iss: "https://evil.example" }), expect: 401 },
    { case: "bad signature", token: mkJwt(userClaims, fixed("other-key", 32)), expect: 401 },
    { case: "unknown kid (verifier refetches JWKS once, then rejects)", token: mkJwt(userClaims, jwtSeed, "unknown-kid"), expect: 401 },
    { case: "alg none", token: `${b64u(utf8(JSON.stringify({ alg: "none", typ: "JWT", kid })))}.${b64u(utf8(JSON.stringify(userClaims)))}.`, expect: 401 },
  ],
};

// 9. A vault .kdbx encrypted with the vault key as a raw 32-byte key file component
kdbxweb.CryptoEngine.setArgon2Impl(async (password, salt, memory, iterations, length, parallelism, type) => {
  if (type !== kdbxweb.CryptoEngine.Argon2TypeArgon2id) throw new Error("expected argon2id");
  return argon2id(new Uint8Array(password), new Uint8Array(salt), { m: memory, t: iterations, p: parallelism, dkLen: length }).slice().buffer;
});
const creds = new kdbxweb.Credentials(null, vaultKey.slice());
const db = kdbxweb.Kdbx.create(creds, "Team Infrastructure");
db.setKdf(kdbxweb.Consts.KdfId.Argon2id);
db.header.kdfParameters.set("M", kdbxweb.VarDictionary.ValueType.UInt64, kdbxweb.Int64.from(1024 * 1024));
db.header.kdfParameters.set("I", kdbxweb.VarDictionary.ValueType.UInt64, kdbxweb.Int64.from(2));
db.header.kdfParameters.set("P", kdbxweb.VarDictionary.ValueType.UInt32, 1);
const entry = db.createEntry(db.getDefaultGroup());
entry.fields.set("Title", "GitHub deploy key");
entry.fields.set("UserName", "ci-bot");
entry.fields.set("Password", kdbxweb.ProtectedValue.fromString("hunter2-but-longer"));
entry.fields.set("URL", "https://github.com");
const kdbxBytes = new Uint8Array(await db.save());
const reopened = await kdbxweb.Kdbx.load(kdbxBytes.buffer, new kdbxweb.Credentials(null, vaultKey.slice()));
if (reopened.getDefaultGroup().entries[0].fields.get("Password").getText() !== "hunter2-but-longer") throw new Error("kdbx roundtrip");
writeFileSync(process.argv[2] + "/vault-fixture.kdbx", kdbxBytes);
vectors.vault_kdbx = { file: "vault-fixture.kdbx", vault_key_hex: hex(vaultKey), key_file_component: "the 32 raw vault key bytes (no password component)", kdf: { alg: "argon2id", m_kib: 1024, t: 2, p: 1 }, expect: { root_group: "Team Infrastructure", entry: { Title: "GitHub deploy key", UserName: "ci-bot", Password: "hunter2-but-longer", URL: "https://github.com" } } };

writeFileSync(process.argv[2] + "/test-vectors.json", JSON.stringify(vectors, null, 2) + "\n");
console.log("ok", Object.keys(vectors).join(", "));
