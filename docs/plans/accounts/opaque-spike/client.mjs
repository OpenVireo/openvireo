// OPAQUE client in WASM (libopaque.js) talking to the C/Python server
import { spawn } from "node:child_process";
import readline from "node:readline";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const [distPath, pyPath, pwd, loginPwd] = process.argv.slice(2);
const opaque = require(distPath);
await opaque.ready;
const py = spawn(process.env.PYTHON ?? "venv/bin/python3", [new URL("./server.py", import.meta.url).pathname, pyPath], { stdio: ["pipe", "pipe", "inherit"] });
const rl = readline.createInterface({ input: py.stdout })[Symbol.asyncIterator]();
const call = async (m) => { py.stdin.write(JSON.stringify(m) + "\n"); return JSON.parse((await rl.next()).value); };
const hex = (u) => Buffer.from(u).toString("hex"), unhex = (s) => new Uint8Array(Buffer.from(s, "hex"));
const ids = { idU: "alice@example.com", idS: "kpx.example" }, context = "kpx-opaque-v1";

let { sec, M } = opaque.createRegistrationRequest({ pwdU: pwd });
const { pub } = await call({ op: "reg", M: hex(M) });
const { rec, export_key } = opaque.finalizeRequest({ sec, pub: unhex(pub), ids });
await call({ op: "store", rec: hex(rec) });

const req = opaque.createCredentialRequest({ pwdU: loginPwd });
const { resp, sk } = await call({ op: "login", pub: hex(req.pub) });
let r;
try { r = opaque.recoverCredentials({ resp: unhex(resp), sec: req.sec, context, ids }); }
catch (e) { console.log(JSON.stringify({ recovered: false, err: String(e) })); py.kill(); process.exit(0); }
const auth = await call({ op: "auth", authU: hex(r.authU) });
console.log(JSON.stringify({ recovered: true, serverAuth: auth.ok, skMatch: hex(r.sk) === sk, exportKeyStable: hex(r.export_key) === hex(export_key) }));
py.kill();
