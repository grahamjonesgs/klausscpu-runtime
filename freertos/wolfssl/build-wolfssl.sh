#!/usr/bin/env bash
# build-wolfssl.sh — Clone and build wolfSSL for KlaussCPU FreeRTOS.
#
# Run from runtime/freertos/wolfssl/ (or any directory — uses absolute paths).
#
# Produces: wolfssl-install/lib/libwolfssl.a
#           wolfssl-install/include/wolfssl/...
#
# Prerequisites:
#   - LLVM built at <repo-root>/build/
#   - picolibc-install/ present at runtime/picolibc-install/
#   - cmake 3.16+, git

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME_DIR="$(realpath "$SCRIPT_DIR/../..")"
# Toolchain lives in the separate klausscpu-llvm fork; point KLAUSSCPU_LLVM_BIN
# at <klausscpu-llvm>/build/bin.  See ../../README.md.
LLVM_BIN="${KLAUSSCPU_LLVM_BIN:?set KLAUSSCPU_LLVM_BIN to <klausscpu-llvm>/build/bin (the built toolchain). See ../../README.md}"
LLVM_BUILD="$(cd "$LLVM_BIN/.." && pwd)"
PICOLIBC="$RUNTIME_DIR/picolibc-install"
INSTALL_DIR="$SCRIPT_DIR/wolfssl-install"
WOLFSSL_SRC="$SCRIPT_DIR/wolfssl-src"
BUILD_DIR="$SCRIPT_DIR/wolfssl-build"
WOLFSSL_TAG="v5.7.2-stable"

if [ ! -f "$LLVM_BUILD/bin/clang" ]; then
    echo "ERROR: clang not found at $LLVM_BUILD/bin/clang"; exit 1
fi
if [ ! -d "$PICOLIBC/include" ]; then
    echo "ERROR: picolibc not found at $PICOLIBC"
    echo "Run:  cd $RUNTIME_DIR && bash build-picolibc.sh"; exit 1
fi

echo "==> wolfSSL build for KlaussCPU"
echo "    LLVM:    $LLVM_BUILD"
echo "    install: $INSTALL_DIR"

if [ ! -d "$WOLFSSL_SRC" ]; then
    echo "==> Cloning wolfSSL $WOLFSSL_TAG..."
    git clone --depth 1 --branch "$WOLFSSL_TAG" \
        https://github.com/wolfSSL/wolfssl.git "$WOLFSSL_SRC"
fi

# wolfSSL's zephyr/module.yml ships without a `name:`, so Zephyr derives the
# module name from the directory — here `wolfssl-src`.  That breaks wolfSSH's
# `depends: [wolfssl]` when both source trees are passed via
# EXTRA_ZEPHYR_MODULES (the Zephyr ssh_shell build).  Pin the name to `wolfssl`.
MODYML="$WOLFSSL_SRC/zephyr/module.yml"
if [ -f "$MODYML" ] && ! grep -q '^name:' "$MODYML"; then
    echo "==> Pinning Zephyr module name to 'wolfssl' in module.yml"
    printf 'name: wolfssl\n%s' "$(cat "$MODYML")" > "$MODYML"
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$INSTALL_DIR"
cd "$BUILD_DIR"

cmake "$WOLFSSL_SRC" \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/wolfssl-toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DWOLFSSL_EXAMPLES=no \
    -DWOLFSSL_TESTS=no \
    -DWOLFSSL_CRYPT_TESTS=no \
    -DWOLFSSL_USER_SETTINGS=yes \
    -DWOLFSSL_INSTALL=yes \
    -DBUILD_SHARED_LIBS=no \
    -DWOLFSSL_FREERTOS=yes \
    -DWOLFSSL_WERROR=no \
    -DWOLFSSL_ED25519=yes \
    -DWOLFSSL_CURVE25519=yes \
    -DWOLFSSL_ECC=yes \
    -G "Unix Makefiles"

make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" install

echo ""
echo "==> wolfSSL built: $INSTALL_DIR/lib/libwolfssl.a"
