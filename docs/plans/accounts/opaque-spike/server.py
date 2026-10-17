# Line-delimited JSON OPAQUE server backed by the libopaque C core (Python binding)
import json, sys
sys.path.insert(0, sys.argv[1])
import ctypes.util, os
_f = ctypes.util.find_library; ctypes.util.find_library = lambda n: {'opaque': os.environ['LIBOPAQUE'], 'sodium': '/opt/homebrew/lib/libsodium.dylib'}.get(n) or _f(n)
import opaque

ids = opaque.Ids("alice@example.com", "kpx.example")
ctx = b"kpx-opaque-v1"
state = {}
for line in sys.stdin:
    m = json.loads(line); op = m["op"]; h = bytes.fromhex
    if op == "reg":
        sec, pub = opaque.CreateRegistrationResponse(h(m["M"]))
        state["regsec"] = sec; out = {"pub": pub.hex()}
    elif op == "store":
        state["rec"] = opaque.StoreUserRecord(state["regsec"], h(m["rec"])); out = {}
    elif op == "login":
        resp, sk, sec = opaque.CreateCredentialResponse(h(m["pub"]), state["rec"], ids, ctx)
        state["authU0"] = sec; out = {"resp": resp.hex(), "sk": sk.hex()}
    elif op == "auth":
        try: opaque.UserAuth(state["authU0"], h(m["authU"])); out = {"ok": True}
        except Exception as e: out = {"ok": False, "err": str(e)}
    print(json.dumps(out), flush=True)
