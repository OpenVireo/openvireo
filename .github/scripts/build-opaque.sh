#!/usr/bin/env bash
#
# Builds libsodium, liboprf and libopaque as static libraries into PREFIX, for KPXC_FEATURE_ACCOUNTS.
# Usage: build-opaque.sh PREFIX   (then configure OpenVireo with -DOPAQUE_ROOT=PREFIX)
#
# The libraries are linked statically so packaging does not have to deploy them. On macOS, set
# MACOSX_DEPLOYMENT_TARGET to match the app's deployment target, and MACOS_ARCH (arm64 or x86_64) to
# cross-compile for the other architecture. Pins follow docs/plans/accounts/SPEC.md §8.

set -euo pipefail

SODIUM_VERSION=1.0.20
SODIUM_SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
LIBOPRF_COMMIT=f925b87
LIBOPAQUE_COMMIT=6e9ac92

PREFIX=$(mkdir -p "$1" && cd "$1" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

CC=cc
SODIUM_CONFIGURE_ARGS=()
if [ -n "${MACOS_ARCH:-}" ]; then
    CC="cc -arch $MACOS_ARCH"
    SODIUM_CONFIGURE_ARGS+=(--host="${MACOS_ARCH/arm64/aarch64}-apple-darwin")
fi

sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1
}

cd "$WORK"
curl -fsSLO "https://github.com/jedisct1/libsodium/releases/download/${SODIUM_VERSION}-RELEASE/libsodium-${SODIUM_VERSION}.tar.gz"
if [ "$(sha256 "libsodium-${SODIUM_VERSION}.tar.gz")" != "$SODIUM_SHA256" ]; then
    echo "libsodium-${SODIUM_VERSION}.tar.gz: checksum mismatch" >&2
    exit 1
fi
tar xzf "libsodium-${SODIUM_VERSION}.tar.gz"
(cd "libsodium-${SODIUM_VERSION}" \
    && CC="$CC" ./configure --prefix="$PREFIX" --disable-shared --enable-static --with-pic --quiet \
        ${SODIUM_CONFIGURE_ARGS[@]+"${SODIUM_CONFIGURE_ARGS[@]}"} \
    && make -j"$JOBS" install)

git clone --quiet https://github.com/stef/liboprf
git -C liboprf checkout --quiet "$LIBOPRF_COMMIT"
git clone --quiet https://github.com/stef/libopaque
git -C libopaque checkout --quiet "$LIBOPAQUE_COMMIT"

# Both makefiles find libsodium through pkgconf
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

mkdir -p "$PREFIX/include/oprf" "$PREFIX/lib"
(cd liboprf/src && make -j"$JOBS" CC="$CC" liboprf.a)
cp liboprf/src/*.h "$PREFIX/include/oprf/"
cp liboprf/src/liboprf.a "$PREFIX/lib/"

# libopaque defaults to -march=native, which would tie the release binaries to the build machine's CPU.
# CFLAGS from the environment replaces that default and keeps the makefile's own additions.
(cd libopaque/src \
    && CFLAGS="-Wall -O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fasynchronous-unwind-tables -ftrapv" \
       CPATH="$PREFIX/include" \
       make -j"$JOBS" CC="$CC" OPRFHOME="$WORK/liboprf/src" libopaque.a)
cp libopaque/src/opaque.h "$PREFIX/include/"
cp libopaque/src/libopaque.a "$PREFIX/lib/"

echo "Installed libsodium, liboprf and libopaque to $PREFIX"
