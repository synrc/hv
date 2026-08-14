#!/usr/bin/env bash

# Build a native OTP 20 host system (erl/erlc) for compiling BEAM bytecode
# and generating start.boot via make_boot.erl.
# Output prefix: build/otp20-host/install/

export CC=/opt/homebrew/opt/llvm/bin/clang
export CXX=/opt/homebrew/opt/llvm/bin/clang++
export CFLAGS="-std=gnu89 -Wno-old-style-definition -arch arm64 -O2 -g"
export CXXFLAGS="-std=gnu++98 -arch arm64 -O2 -g"
export LDFLAGS="-arch arm64"

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"
REPO_ROOT="$(beam_build_root)"
HOST_DIR="$REPO_ROOT/build/otp20-host"
HOST_INSTALL="$HOST_DIR/install"
HOST_ERL="$HOST_INSTALL/bin/erl"
OTP_SRC="$HOST_DIR/otp"
OTP_TAG="OTP-20.3.8.26"
NCPU="$(beam_build_ncpu)"

if [ -x "$HOST_ERL" ] && [ "$(otp_release_of "$HOST_ERL")" = "20" ]; then
    echo "[build-host-otp20] Host OTP 20 already installed at $HOST_INSTALL"
    exit 0
fi

echo "[build-host-otp20] Building native OTP 20 host at $HOST_INSTALL"

# OTP 20 configure rejects native builds when SDK > deployment target.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"

mkdir -p "$HOST_DIR"
SSL_ARGS=()
cd "$HOST_DIR"

if [ ! -d otp ]; then
    echo "[build-host-otp20] Cloning $OTP_TAG..."
    git clone --depth 1 --branch "$OTP_TAG" https://github.com/erlang/otp.git otp
fi
cd otp

if [ ! -f erts/configure ]; then
    echo "[build-host-otp20] Running otp_build autoconf..."
    ./otp_build autoconf
fi

echo "[build-host-otp20] Propagating config.guess / config.sub to sub-libraries..."
CFG_GUESS="erts/autoconf/config.guess"
CFG_SUB="erts/autoconf/config.sub"
if [ ! -f "$CFG_GUESS" ]; then
    CFG_GUESS=$(find . -name 'config.guess' | head -1)
fi
if [ ! -f "$CFG_SUB" ]; then
    CFG_SUB=$(find . -name 'config.sub' | head -1)
fi
if [ -n "$CFG_GUESS" ] && [ -n "$CFG_SUB" ]; then
    while IFS= read -r conf; do
        d="$(dirname "$conf")"
        cp -f "$CFG_GUESS" "$d/config.guess"
        cp -f "$CFG_SUB" "$d/config.sub"
    done < <(find . -name 'configure' -not -path './.git/*')
fi

echo "[build-host-otp20] Disabling optional libraries..."
for lib in mnesia ssh et os_mon snmp otp_mibs debugger observer megaco odbc cosEvent cosEventDomain cosFileTransfer \
            cosNotification cosProperty cosTime cosTransactions wx diameter; do
    if [ -d "lib/$lib" ]; then
        rm -f "lib/$lib/configure"
        # Prevent make from entering the app
        echo "skipping $lib" > "lib/$lib/SKIP"
    fi
done

fix_otp20_macos_configure "$OTP_SRC/configure"

# Do not reuse state from a failed configure attempt.
rm -f "$OTP_SRC/erts/config.cache" "$OTP_SRC/erts/config.status" "$OTP_SRC/erts/config.log"
rm -f "$OTP_SRC/Makefile" "$OTP_SRC/make/output.mk"
rm -f "$HOST_DIR/.configured-host"

# -------------------------------------------------------------------------
# Host-only patches (IPv6 in6addr_*, etc.)
# Applied before configure/build so the tree is consistent.
# -------------------------------------------------------------------------
echo "[build-host-otp20] Applying host patches from ./host-patches/..."
for patch in "$REPO_ROOT"/host-patches/*.patch; do
    if [ -f "$patch" ]; then
        if ! git -C "$OTP_SRC" apply --reverse --check "$patch" 2>/dev/null; then
            echo "[build-host-otp20] Applying $patch..."
            git -C "$OTP_SRC" apply "$patch" || echo "[build-host-otp20] Failed to apply $patch."
        else
            echo "[build-host-otp20] Patch $patch already applied."
        fi
    fi
done

# Prefer LibreSSL, then OpenSSL
for candidate in \
    "$REPO_ROOT/third_party/libressl" \
    "$REPO_ROOT/third_party/libressl-3.9.2" \
    "$(brew --prefix libressl 2>/dev/null || true)" \
    "$(brew --prefix openssl@1.1 2>/dev/null || true)" \
    "$(brew --prefix openssl@3 2>/dev/null || true)" \
    "$(brew --prefix openssl 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -d "$candidate/include" ]; then
        SSL_ARGS=(--with-ssl="$candidate")
        echo "[build-host-otp20] Using SSL at $candidate"
        break
    fi
done

if [ ${#SSL_ARGS[@]} -eq 0 ]; then
    echo "[build-host-otp20] WARNING: LibreSSL/OpenSSL not found; crypto/ssl apps may not build."
    echo " Install with: brew install libressl"
fi

if [ ! -f "$HOST_DIR/.configured-host" ]; then
    echo "[build-host-otp20] Configuring OTP 20 for native host..."
    ERL_TOP="$OTP_SRC" \
    CFLAGS="-O2 -fcommon -std=gnu89 -Wno-old-style-definition -arch arm64" \
    LDFLAGS="-arch arm64" \
    ./configure \
        --prefix="$HOST_INSTALL" \
        --build=aarch64-apple-darwin \
        --disable-smp-support \
        --disable-m32-build \
        --disable-threads \
        --disable-hipe \
        --with-ssl=$REPO_ROOT/third_party/libressl-3.1.5/install-host \
        --without-termcap \
        --without-wx \
        --without-odbc \
        --without-javac \
        --without-docs

    date > "$HOST_DIR/.configured-host"
fi

echo "[build-host-otp20] Building host OTP (this takes several minutes)..."
ERL_TOP="$OTP_SRC" make -j"$NCPU"
ERL_TOP="$OTP_SRC" make install

if [ ! -x "$HOST_ERL" ]; then
    echo "[build-host-otp20] ERROR: $HOST_ERL not found after install."
    exit 1
fi

PWD=`pwd`
cd $OTP_SRC/make
TRIPLE=`clang -print-target-triple`
SIMPLE_TRIPLE=${TRIPLE/arm64/arm}
ln -sfn aarch64-apple-darwin $TRIPLE
ln -sfn aarch64-apple-darwin $SIMPLE_TRIPLE
ln -sfn aarch64-apple-darwin arm-apple-darwin 2>/dev/null || true
cd $PWD

CRYPTO_CSRC="build/otp20-host/otp/lib/crypto/c_src"

# Create the directory the make is looking for
mkdir -p "$CRYPTO_CSRC/arm-apple-darwin25.5.0"

# If there is already a directory for the configured triple, point to it
if [ -d "$CRYPTO_CSRC/aarch64-apple-darwin" ]; then
  ln -sfn ../aarch64-apple-darwin/Makefile "$CRYPTO_CSRC/arm-apple-darwin25.5.0/Makefile" 2>/dev/null || true
  ln -sfn aarch64-apple-darwin "$CRYPTO_CSRC/arm-apple-darwin25.5.0"
fi

mkdir -p "$CRYPTO_CSRC/arm64-apple-darwin25.5.0"
ln -sfn aarch64-apple-darwin "$CRYPTO_CSRC/arm64-apple-darwin25.5.0" 2>/dev/null || true

echo "[build-host-otp20] Done: $(otp_release_of "$HOST_ERL") at $HOST_INSTALL"
