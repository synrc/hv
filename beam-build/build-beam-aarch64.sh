#!/usr/bin/env bash
# Build OTP 20 (non-SMP, no threads, static musl) for AArch64
# Uses Homebrew filosottile/musl-cross toolchain: brew install filosottile/musl-cross/musl-cross
# Output: third_party/tyn/src/beam.aarch64.elf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_ELF="$REPO_ROOT/third_party/tyn/src/beam.aarch64.elf"
BUILD_DIR="$REPO_ROOT/build/otp20-aarch64"

CROSS_GCC="$(command -v aarch64-linux-musl-gcc 2>/dev/null || true)"
if [ -z "$CROSS_GCC" ]; then
    echo "[build-beam-aarch64] ERROR: aarch64-linux-musl-gcc not found."
    echo "  Install with: brew install filosottile/musl-cross/musl-cross"
    exit 1
fi
echo "[build-beam-aarch64] Using cross-compiler: $CROSS_GCC"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone OTP 20.3 if not already present
if [ ! -d otp ]; then
    echo "[build-beam-aarch64] Cloning OTP-20.3.8.26..."
    git clone --depth 1 --branch OTP-20.3.8.26 https://github.com/erlang/otp.git otp
fi

cd otp

# Generate configure scripts (requires autoconf, m4)
if [ ! -f erts/configure ]; then
    echo "[build-beam-aarch64] Running otp_build autoconf..."
    ./otp_build autoconf
fi

# Propagate config.guess / config.sub to all sub-library configure directories.
# otp_build autoconf places these in erts/autoconf/ but snmp/. and other libs
# need them in their own srcdir. Copy unconditionally so incremental runs also fix it.
echo "[build-beam-aarch64] Propagating config.guess / config.sub to sub-libraries..."
CFG_GUESS="erts/autoconf/config.guess"
CFG_SUB="erts/autoconf/config.sub"
# Fallback: find any copy if the above path changed
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
        cp -f "$CFG_SUB"   "$d/config.sub"
    done < <(find . -name 'configure' -not -path './.git/*')
fi

# Patch: apply bare-metal modifications (tty_sl mock, skip fork)
echo "[build-beam-aarch64] Applying bare-metal patch..."
if ! git -C "$BUILD_DIR/otp" apply --reverse --check "$REPO_ROOT/third_party/tyn/src/otp-baremetal.patch" 2>/dev/null; then
    git -C "$BUILD_DIR/otp" apply "$REPO_ROOT/third_party/tyn/src/otp-baremetal.patch" || echo "[build-beam-aarch64] Patch already applied or failed."
else
    echo "[build-beam-aarch64] Patch already applied."
fi

# Disable optional sub-library configure scripts that are not needed for
# the emulator and fail on cross-compilation (snmp, megaco, corba, etc).
# Removing their configure file is the safest approach: lib/configure
# only descends into a subdir if its configure file exists.
echo "[build-beam-aarch64] Disabling optional library configure scripts..."
for lib in snmp megaco odbc cosEvent cosEventDomain cosFileTransfer \
            cosNotification cosProperty cosTime cosTransactions wx diameter; do
    rm -f "lib/$lib/configure"
done

# Configure for AArch64 musl static — non-SMP, no threads, no HiPE, no termcap
# -std=gnu89: OTP 20 / zlib use old K&R-style function definitions
# -Wno-old-style-definition: suppress warnings that become errors with newer GCC
echo "[build-beam-aarch64] Configuring OTP 20 for aarch64-linux-musl..."
ERL_TOP="$BUILD_DIR/otp" \
CC="$CROSS_GCC" \
CFLAGS="-O2 -static -fcommon -std=gnu89 -Wno-old-style-definition" \
LDFLAGS="-static" \
./configure \
    --host=aarch64-linux-musl \
    --build=x86_64-apple-darwin \
    --disable-smp-support \
    --disable-threads \
    --disable-hipe \
    --without-termcap \
    --without-ssl \
    --without-wx \
    --without-odbc \
    --without-javac \
    --without-docs

# Build the emulator only
echo "[build-beam-aarch64] Building emulator (this takes a few minutes)..."
ERL_TOP="$BUILD_DIR/otp" make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)" emulator

# Also build the Erlang/OTP library .beam files using the host erlc
# (these are platform-independent BEAM bytecode, so host compiler works)
echo "[build-beam-aarch64] Building OTP kernel/stdlib libraries..."
ERL_TOP="$BUILD_DIR/otp" make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)" \
    -C "$BUILD_DIR/otp/lib/kernel" opt 2>/dev/null || true
ERL_TOP="$BUILD_DIR/otp" make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)" \
    -C "$BUILD_DIR/otp/lib/stdlib" opt 2>/dev/null || true

# Find the beam binary
BEAM_BIN=""
for candidate in \
    "bin/aarch64-unknown-linux-musl/beam" \
    "bin/aarch64-linux-musl/beam" \
    "erts/bin/beam"; do
    if [ -f "$candidate" ]; then
        BEAM_BIN="$candidate"
        break
    fi
done

if [ -z "$BEAM_BIN" ]; then
    echo "[build-beam-aarch64] ERROR: beam binary not found after build. Files:"
    find . -name "beam" -type f 2>/dev/null || true
    exit 1
fi

echo "[build-beam-aarch64] Stripping $BEAM_BIN -> $OUT_ELF"
aarch64-linux-musl-strip "$BEAM_BIN" -o "$OUT_ELF"

# Build OTP 20 rootfs cpio (unversioned paths: /otp/lib/kernel/ebin/, /otp/lib/stdlib/ebin/)
echo "[build-beam-aarch64] Building OTP 20 rootfs cpio..."
OUT_CPIO="$REPO_ROOT/third_party/tyn/src/otp-rootfs-20.cpio"
STAGING="$BUILD_DIR/rootfs-staging"
# rm -rf "$STAGING"
mkdir -p "$STAGING/otp/bin" \
         "$STAGING/otp/lib/kernel/ebin" \
         "$STAGING/otp/lib/stdlib/ebin"

# start.boot — generated during emulator build
BOOT=""
for b in \
    "$BUILD_DIR/otp/bin/start.boot" \
    "$BUILD_DIR/otp/lib/kernel/src/start.boot" \
    "$BUILD_DIR/otp/bootstrap/bin/start.boot"; do
    [ -f "$b" ] && BOOT="$b" && break
done
if [ -n "$BOOT" ]; then
    cp "$BOOT" "$STAGING/otp/bin/"
fi

# .beam files from lib sources (or bootstrap fallback)
for dir in kernel stdlib compiler; do
    SRC="$BUILD_DIR/otp/lib/$dir/ebin"
    if [ ! -d "$SRC" ] || [ -z "$(ls -A "$SRC"/*.beam 2>/dev/null)" ]; then
        SRC="$BUILD_DIR/otp/bootstrap/lib/$dir/ebin"
    fi
    DST="$STAGING/otp/lib/$dir/ebin"
    mkdir -p "$DST"
    if [ -d "$SRC" ]; then
        find "$SRC" \( -name "*.beam" -o -name "*.app" -o -name "*.appup" \) | while read -r f; do
            cp -f "$f" "$DST/"
        done
    fi
done

cd "$STAGING"
find otp -type f | sort | cpio -o -H newc > "$OUT_CPIO"
echo "[build-beam-aarch64] OTP 20 rootfs: $OUT_CPIO ($(ls -lh "$OUT_CPIO" | awk '{print $5}'))"
cpio -t < "$OUT_CPIO" | grep -E "start\.boot|kernel\.beam|stdlib\.beam" | head -5

echo "[build-beam-aarch64] Done: $OUT_ELF"
ls -lh "$OUT_ELF"
file "$OUT_ELF"
