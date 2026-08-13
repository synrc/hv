#!/usr/bin/env bash
# Build OTP 20 (non-SMP, no threads, static musl) for AArch64
# Uses Homebrew filosottile/musl-cross toolchain: brew install filosottile/musl-cross/musl-cross
#
# Produces:
# build/beam.aarch64.elf — cross-compiled BEAM emulator
# build/otp-rootfs-20.cpio — /otp rootfs with apps from apps.config
# and start.boot from make_boot.erl
#
# Requires a native OTP 20 host (build/otp20-host/install) for erlc/erl.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
REPO_ROOT="$(beam_build_root)"
APPS_CONFIG="$SCRIPT_DIR/apps.config"
OUT_ELF="$REPO_ROOT/build/beam.aarch64.elf"
OUT_CPIO="$REPO_ROOT/build/otp-rootfs-20.cpio"
BUILD_DIR="$REPO_ROOT/build/otp20-aarch64"
HOST_INSTALL="$REPO_ROOT/build/otp20-host/install"
HOST_OTP_SRC="$REPO_ROOT/build/otp20-host/otp"
HOST_ERL="$HOST_INSTALL/bin/erl"
HOST_ERLC="$HOST_INSTALL/bin/erlc"
OTP_SRC="$BUILD_DIR/otp"
STAGING="$BUILD_DIR/rootfs-staging"
NCPU="$(beam_build_ncpu)"
OTP_TAG="OTP-20.3.8.26"
APPS="$(parse_apps_config "$APPS_CONFIG")"

if [ -z "$APPS" ]; then
    echo "[build-beam-aarch64] ERROR: no apps in $APPS_CONFIG"
    exit 1
fi
echo "[build-beam-aarch64] Apps from apps.config: $APPS"

# -------------------------------------------------------------------------
# 1. Host OTP 20 (erl/erlc from matching sources)
# -------------------------------------------------------------------------
bash "$SCRIPT_DIR/build-host-otp20.sh"

# -------------------------------------------------------------------------
# 2. Cross toolchain
# -------------------------------------------------------------------------
CROSS_GCC="$(command -v aarch64-linux-musl-gcc 2>/dev/null || true)"
if [ -z "$CROSS_GCC" ]; then
    echo "[build-beam-aarch64] ERROR: aarch64-linux-musl-gcc not found."
    echo " Install with: brew install filosottile/musl-cross/musl-cross"
    exit 1
fi
echo "[build-beam-aarch64] Using cross-compiler: $CROSS_GCC"

# Detect native build triple (Apple Silicon vs Intel)
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    arm64|aarch64) BUILD_TRIPLE="aarch64-apple-darwin" ;;
    x86_64)        BUILD_TRIPLE="x86_64-apple-darwin" ;;
    *)             BUILD_TRIPLE="$(uname -m)-apple-darwin" ;;
esac
echo "[build-beam-aarch64] Build triple: $BUILD_TRIPLE"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# -------------------------------------------------------------------------
# 3. Clone / prepare cross OTP source tree
# -------------------------------------------------------------------------
if [ ! -d otp ]; then
    echo "[build-beam-aarch64] Cloning $OTP_TAG..."
    git clone --depth 1 --branch "$OTP_TAG" https://github.com/erlang/otp.git otp
fi
cd otp

if [ ! -f erts/configure ]; then
    echo "[build-beam-aarch64] Running otp_build autoconf..."
    ./otp_build autoconf
fi

echo "[build-beam-aarch64] Propagating config.guess / config.sub to sub-libraries..."
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

echo "[build-beam-aarch64] Applying bare-metal patches from ./patches/..."
for patch in "$REPO_ROOT"/patches/*.patch; do
    if [ -f "$patch" ]; then
        if ! git -C "$BUILD_DIR/otp" apply --reverse --check "$patch" 2>/dev/null; then
            echo "[build-beam-aarch64] Applying $patch..."
            git -C "$BUILD_DIR/otp" apply "$patch" || echo "[build-beam-aarch64] Failed to apply $patch."
        else
            echo "[build-beam-aarch64] Patch $patch already applied."
        fi
    fi
done

echo "[build-beam-aarch64] Disabling optional library configure scripts..."
for lib in snmp megaco odbc cosEvent cosEventDomain cosFileTransfer \
            cosNotification cosProperty cosTime cosTransactions wx diameter; do
    rm -f "lib/$lib/configure"
done

# -------------------------------------------------------------------------
# 4. Cross-configure and build emulator only (--without-ssl for bare metal)
# -------------------------------------------------------------------------
if [ ! -f "$BUILD_DIR/.configured-cross" ]; then
    if [ -f Makefile ] && { [ -d bin/aarch64-unknown-linux-musl ] || [ -d bin/aarch64-linux-musl ]; }; then
        echo "[build-beam-aarch64] Cross OTP tree already configured."
        date > "$BUILD_DIR/.configured-cross"
    fi
fi

if [ ! -f "$BUILD_DIR/.configured-cross" ]; then
    echo "[build-beam-aarch64] Configuring OTP 20 for aarch64-linux-musl..."
    ERL_TOP="$OTP_SRC" \
    CC="$CROSS_GCC" \
    CFLAGS="-O2 -static -fcommon -std=gnu89 -Wno-old-style-definition" \
    LDFLAGS="-static" \
    ./configure \
        --host=aarch64-linux-musl \
        --build="$BUILD_TRIPLE" \
        --disable-smp-support \
        --disable-threads \
        --disable-hipe \
        --without-termcap \
        --without-ssl \
        --without-wx \
        --without-odbc \
        --without-javac \
        --without-docs
    date > "$BUILD_DIR/.configured-cross"
fi

echo "[build-beam-aarch64] Building emulator (this takes a few minutes)..."
ERL_TOP="$OTP_SRC" make -j"$NCPU" emulator

BEAM_BIN=""
for candidate in \
    "bin/aarch64-linux-musl/beam" \
    "bin/aarch64-unknown-linux-musl/beam" \
    "erts/bin/beam"; do
    if [ -f "$candidate" ]; then
        BEAM_BIN="$candidate"
        break
    fi
done

if [ -z "$BEAM_BIN" ]; then
    echo "[build-beam-aarch64] ERROR: beam binary not found after build."
    find . -name "beam" -type f 2>/dev/null || true
    exit 1
fi

cp -f "$BEAM_BIN" "$OUT_ELF"
echo "[build-beam-aarch64] BEAM emulator: $OUT_ELF"
file "$OUT_ELF"

# -------------------------------------------------------------------------
# 5. Build OTP apps (BEAM bytecode) with host OTP 20 erlc
# Uses the host tree sources — same tag, correct compiler version.
# -------------------------------------------------------------------------
echo "[build-beam-aarch64] Building OTP apps with host erlc..."
export PATH="$HOST_INSTALL/bin:$PATH"

if [ ! -d "$HOST_OTP_SRC" ]; then
    echo "[build-beam-aarch64] ERROR: host OTP sources not found at $HOST_OTP_SRC"
    exit 1
fi

for app in $APPS; do
    echo "[build-beam-aarch64] make -C lib/$app opt"
    ERL_TOP="$HOST_OTP_SRC" make -j"$NCPU" -C "$HOST_OTP_SRC/lib/$app" opt
done

# -------------------------------------------------------------------------
# 6. Stage rootfs and generate start.boot via make_boot.erl
# -------------------------------------------------------------------------
echo "[build-beam-aarch64] Staging rootfs and generating start.boot..."
rm -rf "$STAGING"
mkdir -p "$STAGING/otp/bin"

for app in $APPS; do
    SRC="$HOST_OTP_SRC/lib/$app/ebin"
    if [ ! -d "$SRC" ] || [ -z "$(ls -A "$SRC"/*.beam 2>/dev/null)" ]; then
        echo "[build-beam-aarch64] ERROR: no .beam files in $SRC for app $app"
        exit 1
    fi
    DST="$STAGING/otp/lib/$app/ebin"
    mkdir -p "$DST"
    find "$SRC" \( -name "*.beam" -o -name "*.app" -o -name "*.appup" \) -exec cp -f {} "$DST/" \;
done

MAKE_BOOT_BEAM="$BUILD_DIR/make_boot.beam"
"$HOST_ERLC" -o "$BUILD_DIR" "$SCRIPT_DIR/make_boot.erl"

# Ensure the beam is loadable
if [ ! -f "$BUILD_DIR/make_boot.beam" ]; then
    echo "[build-beam-aarch64] ERROR: make_boot.beam not produced"
    exit 1
fi

"$HOST_ERL" -noshell \
    -pa "$BUILD_DIR" \
    -run make_boot main "$STAGING" "$APPS_CONFIG" \
    -s init stop

if [ ! -f "$STAGING/otp/bin/start.boot" ]; then
    echo "[build-beam-aarch64] ERROR: make_boot.erl did not produce start.boot"
    exit 1
fi

cd "$STAGING"
find otp -type f | sort | cpio -o -H newc > "$OUT_CPIO"
echo "[build-beam-aarch64] OTP 20 rootfs: $OUT_CPIO ($(ls -lh "$OUT_CPIO" | awk '{print $5}'))"
echo "[build-beam-aarch64] Rootfs contents (sample):"
cpio -t < "$OUT_CPIO" | grep -E "start\.boot|\.app$" | head -20
echo "[build-beam-aarch64] Done."
