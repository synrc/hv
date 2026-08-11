#!/usr/bin/env bash
# Build OTP 20 (non-SMP, no threads, static musl) for AArch64
# Uses Homebrew filosottile/musl-cross toolchain: brew install filosottile/musl-cross/musl-cross
# Output: third_party/tyn/src/beam.aarch64.elf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_ELF="$REPO_ROOT/build/beam.aarch64.elf"
BUILD_DIR="$REPO_ROOT/build/otp29-aarch64"

CROSS_GCC="$(command -v aarch64-linux-musl-gcc 2>/dev/null || true)"
if [ -z "$CROSS_GCC" ]; then
    echo "[build-beam-aarch64] ERROR: aarch64-linux-musl-gcc not found."
    echo "  Install with: brew install filosottile/musl-cross/musl-cross"
    exit 1
fi
echo "[build-beam-aarch64] Using cross-compiler: $CROSS_GCC"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download OTP 29.0.5 if not already present
if [ ! -d otp ]; then
    echo "[build-beam-aarch64] Downloading OTP-29.0.5..."
    curl -L -s -o otp.tar.gz https://github.com/erlang/otp/archive/refs/tags/OTP-29.0.5.tar.gz
    mkdir -p otp
    tar -xf otp.tar.gz -C otp --strip-components=1
    rm otp.tar.gz
fi

cd otp

# Generate configure scripts (requires autoconf, m4)
if [ ! -f erts/configure ]; then
    echo "[build-beam-aarch64] Running otp_build autoconf..."
    ./otp_build autoconf
fi

echo "[build-beam-aarch64] Building LibreSSL 3.9.2..."
LIBRESSL_SRC="$REPO_ROOT/third_party/libressl-3.9.2"
LIBRESSL_PREFIX="$BUILD_DIR/libressl"
if [ ! -f "$LIBRESSL_PREFIX/lib/libcrypto.a" ]; then
    cd "$LIBRESSL_SRC"
    # Clean any previous builds
    make clean || true
    CC="$CROSS_GCC" CFLAGS="-O2 -fPIC -static" ./configure \
        --host=aarch64-linux-musl \
        --prefix="$LIBRESSL_PREFIX" \
        --disable-shared \
        --enable-static \
        --with-openssldir="$LIBRESSL_PREFIX/etc/ssl"
    make
    make install
    cd "$BUILD_DIR/otp"
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

# Patch: apply bare-metal modifications
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

# Disable optional sub-library configure scripts that are not needed for
# the emulator and fail on cross-compilation (snmp, megaco, corba, etc).
# Removing their configure file is the safest approach: lib/configure
# only descends into a subdir if its configure file exists.
# Disable optional sub-library configure scripts that are not needed
echo "[build-beam-aarch64] Disabling optional library configure scripts..."
for lib in snmp megaco odbc cosEvent cosEventDomain cosFileTransfer \
            cosNotification cosProperty cosTime cosTransactions wx diameter; do
    rm -f "lib/$lib/configure"
done



# Configure for AArch64 musl static
echo "[build-beam-aarch64] Configuring OTP 29.0.5 for aarch64-linux-musl..."
# Cache sizeof answers for cross-compilation — configure can't run test programs.
# aarch64 musl has 64-bit off_t and long.
ac_cv_sizeof_off_t=8 \
ac_cv_sizeof_long=8 \
ERL_TOP="$BUILD_DIR/otp" \
CC="$CROSS_GCC" \
CFLAGS="-O2 -static -fcommon -Wno-error -Wno-unused-function" \
LDFLAGS="-static" \
./configure \
    --host=aarch64-linux-musl \
    --build=x86_64-apple-darwin \
    --disable-year2038 \
    --without-termcap \
    --with-ssl="$LIBRESSL_PREFIX" \
    --without-wx \
    --without-odbc \
    --without-javac \
    --without-docs

# Remove stale depend.mk files — a previously killed build can leave NUL bytes
# that corrupt make's dependency parser on the next run.
find "$BUILD_DIR/otp/erts/emulator" -name "depend.mk" -delete 2>/dev/null || true

# Build the emulator only (pass TARGET to avoid picking up the host darwin triple)
# Single line — no continuation: avoids trailing-space-after-backslash issues.
echo "[build-beam-aarch64] Building emulator (this takes a few minutes)..."
NCPU="$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"
ERL_TOP="$BUILD_DIR/otp" make -j"$NCPU" -C erts/emulator TARGET=aarch64-unknown-linux-musl

# Also build the Erlang/OTP library .beam files using the host erlc
# (these are platform-independent BEAM bytecode, so host compiler works)
echo "[build-beam-aarch64] Building OTP apps..."
APPS=$(cat "$SCRIPT_DIR/apps.config" | grep -o '\[.*\]' | tr -d '[],')

# Manually configure crypto and ssl before building them.
# --with-ssl-incl bypasses the cross-sysroot check and tells the lib configure
# exactly where to find OpenSSL/LibreSSL headers.
# NOTE: NIFs build as .so (shared), so no -static or pthread_stub.o in LDFLAGS here.
for lib in crypto ssl; do
    if [ -f "$BUILD_DIR/otp/lib/$lib/configure" ]; then
        echo "Configuring $lib..."
        cd "$BUILD_DIR/otp/lib/$lib"
        ERL_TOP="$BUILD_DIR/otp" \
        CC="$CROSS_GCC" \
        CFLAGS="-O2 -fcommon -I$LIBRESSL_PREFIX/include" \
        LDFLAGS="-L$LIBRESSL_PREFIX/lib" \
        ./configure \
            --host=aarch64-linux-musl \
            --build=x86_64-apple-darwin \
            --with-ssl="$LIBRESSL_PREFIX" \
            --with-ssl-incl="$LIBRESSL_PREFIX/include" \
            --disable-dynamic-ssl-lib
    fi
done
cd "$BUILD_DIR"

export ERL_COMPILER_OPTIONS="[]"

for app in $APPS; do
    if [ -d "$BUILD_DIR/otp/lib/$app" ]; then
        echo "Building $app..."
        ERL_TOP="$BUILD_DIR/otp" make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)" \
            TARGET="aarch64-unknown-linux-musl" \
            CFLAGS="-O2 -static -fcommon -I$LIBRESSL_PREFIX/include" \
            -C "$BUILD_DIR/otp/lib/$app" opt || echo "WARNING: $app build failed, continuing..."
    fi
done

# Find the beam binary — OTP 29 JIT produces beam.jit (installed as beam.smp)
BEAM_BIN=""
for candidate in \
    "$BUILD_DIR/otp/bin/aarch64-unknown-linux-musl/beam.smp" \
    "$BUILD_DIR/otp/bin/aarch64-unknown-linux-musl/beam.jit" \
    "$BUILD_DIR/otp/bin/aarch64-unknown-linux-musl/beam" \
    "$BUILD_DIR/otp/bin/aarch64-linux-musl/beam.smp" \
    "$BUILD_DIR/otp/bin/aarch64-linux-musl/beam" \
    "$BUILD_DIR/otp/erts/emulator/obj/aarch64-unknown-linux-musl/opt/smp/beam.smp" \
    "$BUILD_DIR/otp/erts/bin/aarch64-unknown-linux-musl/beam.smp" \
    "$BUILD_DIR/otp/erts/bin/beam"; do
    if [ -f "$candidate" ]; then
        BEAM_BIN="$candidate"
        break
    fi
done

if [ -z "$BEAM_BIN" ]; then
    echo "[build-beam-aarch64] ERROR: beam binary not found after build. Files:"
    find "$BUILD_DIR/otp/bin" "$BUILD_DIR/otp/erts/bin" \
        \( -name "beam.smp" -o -name "beam.jit" -o -name "beam" \) -type f 2>/dev/null || true
    exit 1
fi

echo "[build-beam-aarch64] Stripping $BEAM_BIN -> $OUT_ELF"
aarch64-linux-musl-strip "$BEAM_BIN" -o "$OUT_ELF"

# Build OTP 29 rootfs cpio (unversioned paths: /otp/lib/kernel/ebin/, /otp/lib/stdlib/ebin/)
echo "[build-beam-aarch64] Building OTP 29 rootfs cpio..."
OUT_CPIO="$REPO_ROOT/build/otp-rootfs-29.cpio"
STAGING="$BUILD_DIR/rootfs-staging"
# rm -rf "$STAGING"
mkdir -p "$STAGING/otp/bin" \
         "$STAGING/otp/lib/kernel/ebin" \
         "$STAGING/otp/lib/stdlib/ebin"

APPS=$(cat "$SCRIPT_DIR/apps.config" | grep -o '\[.*\]' | tr -d '[],')

# 1. Package .beam files from lib sources (or bootstrap fallback)
rm -rf "$STAGING/otp/lib"
mkdir -p "$STAGING/otp/lib"
for dir in $APPS; do
    SRC="$BUILD_DIR/otp/lib/$dir/ebin"
    # Fall back to bootstrap if the built ebin has no beams
    if [ -z "$(ls "$SRC"/*.beam 2>/dev/null)" ]; then
        SRC="$BUILD_DIR/otp/bootstrap/lib/$dir/ebin"
    fi
    DST="$STAGING/otp/lib/$dir/ebin"
    mkdir -p "$DST"
    if [ -d "$SRC" ]; then
        # cp -f directly — avoids pipe subshell masking errors
        cp -f "$SRC"/*.beam "$DST/" 2>/dev/null || true
        cp -f "$SRC"/*.app  "$DST/" 2>/dev/null || true
        cp -f "$SRC"/*.appup "$DST/" 2>/dev/null || true
        echo "  Staged $dir: $(ls "$DST"/*.beam 2>/dev/null | wc -l | tr -d ' ') beams"
    else
        echo "  WARNING: no ebin found for $dir"
    fi
done

# 2. Dynamically generate start.boot using custom generator
echo "[build-beam-aarch64] Generating custom start.boot..."
# Compile to REPO_ROOT so erl -run finds it in the current directory (default code path).
# Without this, a stale make_boot.beam in the project root would be loaded instead.
erlc -o "$REPO_ROOT" "$REPO_ROOT/beam-build/make_boot.erl"
if ! erl -noshell -pa "$REPO_ROOT" -run make_boot main "$STAGING" "$REPO_ROOT/beam-build/apps.config"; then
    echo "ERROR: Failed to generate start.boot!"
    exit 1
fi

# Run cpio from inside STAGING so paths are relative (macOS BSD cpio has no -D)
(cd "$STAGING" && find otp -type f | sort | cpio -o -H newc) > "$OUT_CPIO"
echo "[build-beam-aarch64] OTP 29 rootfs: $OUT_CPIO ($(ls -lh "$OUT_CPIO" | awk '{print $5}'))"
cpio -t < "$OUT_CPIO" | grep -E "start\.boot|kernel\.beam|stdlib\.beam" | head -5

echo "[build-beam-aarch64] Done: $OUT_ELF"
ls -lh "$OUT_ELF"
file "$OUT_ELF"
