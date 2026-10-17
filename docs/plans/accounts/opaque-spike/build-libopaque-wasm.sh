#!/usr/bin/env bash
# Builds a release (random, non-tracing) libopaque.js from libopaque 6e9ac92 + liboprf f925b87.
# Upstream's js/dist/libopaque.js at that commit is a -DTRACE -DNORANDOM debug build: don't use it.
#
# Usage: build-libopaque-wasm.sh <empty work dir>   (needs git and Docker; writes <dir>/libopaque.js)
set -euo pipefail

work=$(cd "$1" && pwd)
cd "$work"
git clone -q https://github.com/stef/libopaque
git -C libopaque checkout -q 6e9ac92f9a2289679e04b0b0c5fdc307bb3de54e
git -C libopaque submodule update -q --init --recursive js/libsodium.js
git clone -q https://github.com/stef/liboprf
git -C liboprf checkout -q f925b87
# libopaque includes <oprf/toprf.h>
mkdir -p inc && ln -sfn ../liboprf/src inc/oprf

# emsdk 1.40.1 is what libopaque's js/README documents; its Debian buster repos moved to the archive.
docker run --rm --platform linux/amd64 -v "$work":/src -w /src/libopaque/js \
  -e CPATH=/src/inc \
  -e PKG_CONFIG_PATH=/src/libopaque/js/libsodium.js/libsodium/libsodium-js-sumo/lib/pkgconfig \
  emscripten/emsdk:1.40.1 bash -euc '
    echo "deb http://archive.debian.org/debian buster main" > /etc/apt/sources.list
    apt-get -o Acquire::Check-Valid-Until=false update -qq
    apt-get install -y -qq pkg-config uncrustify libtool autoconf automake >/dev/null
    ln -sf "$(command -v pkg-config)" /usr/local/bin/pkgconf
    # libsodium.js: only the static library is needed; its JS wrapper step may fail on newer node
    make libsodium DEFINES= || test -f libsodium.js/libsodium/libsodium-js-sumo/lib/libsodium.a
    make oprf libopaque.so libopaquejs DEFINES= LIBOPRFHOME=/src/liboprf/src'
cp libopaque/js/dist/libopaque.js "$work/libopaque.js"

# A release build must send a fresh client nonce in every KE1 (bytes 32..63); a NORANDOM build sends 00 01 02 ...
node -e '
  const o = require(process.argv[1]);
  o.ready.then(() => {
    const n = () => Buffer.from(o.createCredentialRequest({ pwdU: "x" }).pub).subarray(32, 64).toString("hex");
    const a = n(), b = n();
    if (a === b || a.startsWith("000102")) { console.error("FAIL: fixed randomness"); process.exit(1); }
    console.log("OK: random nonces");
  });' "$work/libopaque.js"
shasum -a 256 "$work/libopaque.js"
