process.on("uncaughtException", (e) => { console.log(JSON.stringify({ fatal: String(e && e.message || e).slice(0, 200) })); process.exit(0); });
process.on("unhandledRejection", (e) => { console.log(JSON.stringify({ fatal: String(e && e.message || e).slice(0, 200) })); process.exit(0); });
await import("./client.mjs");
