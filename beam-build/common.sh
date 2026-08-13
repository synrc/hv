#!/usr/bin/env bash
# Shared helpers for OTP 20 beam / rootfs build scripts.

beam_build_root() {
    cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

# Space-separated app names from beam-build/apps.config
parse_apps_config() {
    local cfg="${1:-$(dirname "${BASH_SOURCE[0]}")/apps.config}"
    sed -n 's/^[[:space:]]*{apps,[[:space:]]*\[\(.*\)\]}.*$/\1/p' "$cfg" \
        | tr -d ' ' | tr ',' ' '
}

beam_build_ncpu() {
    nproc 2>/dev/null || sysctl -n hw.logicalcpu
}

# OTP 20 configure compares __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ against a
# broken int_macosx_version on macOS 15+ (e.g. 26.5.2 becomes 2652, not 260502).
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

# OTP release string from an erl binary (e.g. "20")
otp_release_of() {
    local erl="$1"
    "$erl" -noshell -eval 'io:format("~s", [erlang:system_info(otp_release)]), halt().'
}
