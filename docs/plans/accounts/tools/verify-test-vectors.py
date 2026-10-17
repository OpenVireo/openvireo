# Independent check of test-vectors.json with libsodium (pysodium) + hashlib
import json, sys, hmac, hashlib, base64, unicodedata, ctypes.util
_f = ctypes.util.find_library; ctypes.util.find_library = lambda n: '/opt/homebrew/lib/libsodium.dylib' if n == 'sodium' else _f(n)
import pysodium as s
V = json.load(open(sys.argv[1])); H = bytes.fromhex
b = lambda x: base64.urlsafe_b64decode(x + "=" * (-len(x) % 4))
def hkdf(ikm, salt, info, n=32):
    prk = hmac.new(salt or b"\0"*32, ikm, hashlib.sha256).digest(); return hmac.new(prk, info + b"\1", hashlib.sha256).digest()[:n]
ok = lambda c, m: print(("PASS " if c else "FAIL ") + m) or c
res = []
for v in V["auk"]:
    k = v["kdf"]; pw = unicodedata.normalize("NFKC", v["password"]).encode()
    st = s.crypto_pwhash(32, pw, b(k["salt"]), k["t"], k["m"]*1024, s.crypto_pwhash_ALG_ARGON2ID13)
    res.append(ok(st.hex() == v["argon2id_hex"] and hkdf(st, H(v["secret_key_hex"]), b"kpx-auk-v1").hex() == v["auk_hex"], f"auk m={k['m']} {v['password'][:10]}"))
ks = V["keyset"]; w = b(ks["wrapped"])
pt = s.crypto_aead_xchacha20poly1305_ietf_decrypt(w[25:], ks["aad"].encode(), w[1:25], H(ks["auk_hex"]))
res.append(ok(pt.hex() == ks["enc_sk_hex"] + ks["sign_seed_hex"], "keyset unwrap"))
pk, _ = s.crypto_sign_seed_keypair(H(ks["sign_seed_hex"])); epk = s.crypto_scalarmult_curve25519_base(H(ks["enc_sk_hex"]))
res.append(ok(pk == b(ks["public"]["sign_pk"]) and epk == b(ks["public"]["enc_pk"]), "keyset public keys"))
s.crypto_sign_verify_detached(b(ks["public"]["sig"]), b"kpx-pubkeys-v1" + epk + pk, pk); res.append(ok(True, "pubkey self-signature"))
fp = hashlib.sha256(b"kpx-fp-v1" + epk + pk).hexdigest()[:40]; res.append(ok(" ".join(fp[i:i+4] for i in range(0,40,4)) == ks["public"]["fingerprint"], "fingerprint"))
for i, v in enumerate(V["seal"]):
    bl = b(v["sealed"]); e, n = bl[1:33], bl[33:57]; sk = H(v["recipient_enc_sk_hex"])
    shared = s.crypto_scalarmult_curve25519(sk, e); key = hkdf(shared, e + s.crypto_scalarmult_curve25519_base(sk), b"kpx-seal-v1")
    res.append(ok(s.crypto_aead_xchacha20poly1305_ietf_decrypt(bl[57:], v["aad"].encode(), n, key).hex() == v["plaintext_hex"], f"unseal {i}"))
vn = V["vault_name"]; nk = hkdf(H(vn["vault_key_hex"]), b"", b"kpx-vault-name-v1"); bl = b(vn["encrypted_name"])
res.append(ok(s.crypto_aead_xchacha20poly1305_ietf_decrypt(bl[25:], vn["aad"].encode(), bl[1:25], nk).decode() == vn["name"], "vault name"))
sa = V["service_account"]; p = json.loads(b(sa["token"][len("kpx_sa_"):]))
res.append(ok(hashlib.sha256(b(p["secret"])).hexdigest() == sa["secret_hash_hex"] and p["secret"][:8] == sa["secret_prefix"], "sa token secret hash"))
bl = b(sa["wrapped_master"]); m = s.crypto_aead_xchacha20poly1305_ietf_decrypt(bl[25:], sa["wrapped_master_aad"].encode(), bl[1:25], b(p["kek"]))
bl = b(sa["wrapped_keyset"]); kp = s.crypto_aead_xchacha20poly1305_ietf_decrypt(bl[25:], b"kpx-keyset-v1", bl[1:25], m)
res.append(ok(m.hex() == sa["master_key_hex"] and kp.hex() == sa["keyset_enc_sk_hex"] + sa["keyset_sign_seed_hex"], "sa kek -> master -> keyset"))
rt = V["refresh_token"]; res.append(ok(hashlib.sha256(rt["token"].encode()).hexdigest() == rt["stored_hash_hex"], "refresh token hash"))
x = b(V["jwt"]["jwks"]["keys"][0]["x"])
for t in V["jwt"]["valid"]:
    h, pl, sg = t["token"].split("."); s.crypto_sign_verify_detached(b(sg), f"{h}.{pl}".encode(), x)
    res.append(ok(json.loads(b(pl)) == t["claims"], "jwt " + t["claims"]["sub"][:3]))
print("ALL PASS" if all(res) else "SOME FAILED")
