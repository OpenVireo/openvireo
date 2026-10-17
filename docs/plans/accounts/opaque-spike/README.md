# OPAQUE cross-implementation spike

Checks that the libopaque JS/WASM build (client) and the libopaque C core through its Python binding
(server) complete registration and login together. Result on 2026-10-17, libopaque `6e9ac92`,
liboprf `f925b87`, libsodium 1.0.22:

| Client build | Right password | Wrong password |
|---|---|---|
| repo `js/dist/libopaque.js` @ 6e9ac92 | login OK, `sk` and `export_key` match | `RecoverCredentials failed` |
| npm `libopaque@0.0.0-alpha.2` | **fails in `finalizeRequest`** (incompatible) | — |

## Reproduce (macOS)

```sh
brew install libsodium pkg-config
git clone https://github.com/stef/libopaque && git -C libopaque checkout 6e9ac92
git clone https://github.com/stef/liboprf && git -C liboprf checkout f925b87
(cd liboprf/src && make CC=clang)                       # the debug-symbol install step may fail; the dylibs are enough
mkdir -p inc && ln -sfn $PWD/liboprf/src inc/oprf
(cd libopaque/src && CPATH=/opt/homebrew/include:$PWD/../../inc:$PWD/../../liboprf/src \
   LIBRARY_PATH=/opt/homebrew/lib:$PWD/../../liboprf/src make CC=clang OPRFHOME=$PWD/../../liboprf/src libopaque.dylib)
python3 -m venv venv && venv/bin/pip install pysodium
# Fix the dylib install names if dyld can't find liboprf / liboprf-noiseXK (install_name_tool -change ...)
LIBOPAQUE=$PWD/libopaque/src/libopaque.dylib node run.mjs $PWD/libopaque/js/dist/libopaque.js $PWD/libopaque/python "pw" "pw"
```

`server.py` loads libopaque through `$LIBOPAQUE`, because macOS's `/usr/bin/python3` drops `DYLD_*` variables.
