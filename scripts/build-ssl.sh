#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -d "${ROOT_DIR}/aarch64-linux-musl-cross/bin" ]; then
  export PATH="${ROOT_DIR}/aarch64-linux-musl-cross/bin:$PATH"
fi

cd "${ROOT_DIR}/third_party/libressl-3.1.5"

# clean previous build to ensure clean settings
make distclean 2>/dev/null || true

export CFLAGS="-fPIC -O2"
./configure \
  --prefix="$PWD/install-host" \
  --disable-shared

NCPU=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
make -j"$NCPU"
make install

# clean host build before building target
make distclean 2>/dev/null || true

export CC=aarch64-linux-musl-gcc
export CFLAGS="-static -O2 -fPIC"
export LDFLAGS="-static"

./configure \
  --host=aarch64-linux-musl \
  --prefix="$PWD/install-aarch64" \
  --disable-shared \
  --disable-asm          # often safer on musl/seL4

make -j"$NCPU"
make install