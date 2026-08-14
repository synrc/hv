#!/usr/bin/env bash

# Shared helpers for OTP 20 beam / rootfs build scripts.
# Hosts   : Apple M1/M4, Intel Mac, Alpine/Linux x86_64 & aarch64,
#           Raspberry Pi 500, NetBSD x86_32, NetBSD x86_64
# Targets : aarch64-linux-musl (ARM64), arm-linux-musleabihf (ARM32)

beam_triples() {
    if [ -z "${BUILD_TRIPLE:-}" ]; then
        local arch os
        arch="$(uname -m)"
        os="$(uname -s)"

        case "$os" in
            Darwin)
                case "$arch" in
                    arm64|aarch64) BUILD_TRIPLE="aarch64-apple-darwin" ;;
                    x86_64)        BUILD_TRIPLE="x86_64-apple-darwin" ;;
                    *)             BUILD_TRIPLE="${arch}-apple-darwin" ;;
                esac
                ;;
            Linux)
                if [ -f /etc/alpine-release ]; then
                    BUILD_TRIPLE="${arch}-alpine-linux-musl"
                else
                    BUILD_TRIPLE="${arch}-linux-musl"
                fi
                ;;
            NetBSD)
                case "$arch" in
                    i386|i486|i586|i686) BUILD_TRIPLE="i386-unknown-netbsd" ;;
                    x86_64|amd64)        BUILD_TRIPLE="x86_64-unknown-netbsd" ;;
                    *)                   BUILD_TRIPLE="${arch}-unknown-netbsd" ;;
                esac
                ;;
            *)
                BUILD_TRIPLE="${arch}-unknown"
                ;;
        esac
    fi

    HOST_TRIPLE="${HOST_TRIPLE:-$BUILD_TRIPLE}"

    # ----- Target (default ARM64 musl) -----
    TARGET_TRIPLE="${TARGET_TRIPLE:-aarch64-linux-musl}"

    # ----- Cross compiler prefix -----
    case "$TARGET_TRIPLE" in
        aarch64-linux-musl)          CROSS_PREFIX="aarch64-linux-musl-" ;;
        arm-linux-musleabihf|\
        armv7-linux-musleabihf)      CROSS_PREFIX="arm-linux-musleabihf-" ;;
        *)                           CROSS_PREFIX="${TARGET_TRIPLE}-" ;;
    esac

    # Native build → no prefix
    if [ "$TARGET_TRIPLE" = "$BUILD_TRIPLE" ] || \
       [ "$TARGET_TRIPLE" = "$HOST_TRIPLE" ]; then
        CROSS_PREFIX=""
    fi

    CROSS_CC="${CROSS_CC:-${CROSS_PREFIX}gcc}"
    CROSS_CXX="${CROSS_CXX:-${CROSS_PREFIX}g++}"
    CROSS_AR="${CROSS_AR:-${CROSS_PREFIX}ar}"
    CROSS_RANLIB="${CROSS_RANLIB:-${CROSS_PREFIX}ranlib}"
}

beam_is_cross() {
    [ "$TARGET_TRIPLE" != "$BUILD_TRIPLE" ] && [ "$TARGET_TRIPLE" != "$HOST_TRIPLE" ]
}

beam_is_apple_host() {
    case "$BUILD_TRIPLE" in *-apple-darwin*) return 0 ;; *) return 1 ;; esac
}

beam_is_alpine_host() {
    case "$BUILD_TRIPLE" in *-alpine-linux-musl) return 0 ;; *) return 1 ;; esac
}

beam_is_netbsd_host() {
    case "$BUILD_TRIPLE" in *-netbsd*) return 0 ;; *) return 1 ;; esac
}


beam_build_root() {
    cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

parse_apps_config() {
    local cfg="${1:-$(dirname "${BASH_SOURCE[0]}")/apps.config}"
    sed -n 's/^[[:space:]]*{apps,[[:space:]]*\[\(.*\)\]}.*$/\1/p' "$cfg" \
        | tr -d ' ' | tr ',' ' '
}

beam_build_ncpu() {
    nproc 2>/dev/null || sysctl -n hw.logicalcpu
}

fix_otp20_macos_configure() {
    local configure="$1"
    local major
    major="$(sw_vers -productVersion 2>/dev/null | cut -d. -f1 || echo 0)"
    if [ "${major:-0}" -lt 15 ] || [ ! -f "$configure" ]; then
        return 0
    fi
    if grep -q 'patched macOS deploy check' "$configure"; then
        return 0
    fi
    echo "[beam-build] Patching OTP 20 configure for macOS ${major}.x deployment check..."
    sed -i '' 's/#if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ > $int_macosx_version/#if 0 \/\* patched macOS deploy check *\//' "$configure"
}

otp_release_of() {
    local erl="$1"
    "$erl" -noshell -eval 'io:format("~s", [erlang:system_info(otp_release)]), halt().'
}
