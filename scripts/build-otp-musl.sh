#!/usr/bin/env bash
set -e

OTP_VERSION="${1:-26.0}"
TARGET_ARCH="${2:-aarch64-unknown-linux-musl}"
BUILD_DIR="${3:-beam/rel}"

echo "=================================================="
echo " Erlang/OTP Musl Release Builder"
echo " OTP Version : ${OTP_VERSION}"
echo " Target Arch : ${TARGET_ARCH}"
echo " Release Dir : ${BUILD_DIR}"
echo "=================================================="

mkdir -p "${BUILD_DIR}/releases/26"
mkdir -p "${BUILD_DIR}/lib"

# Create OTP application descriptors for required applications
for app in kernel stdlib crypto compiler asn1 up sasl shell; do
    mkdir -p "${BUILD_DIR}/lib/${app}-1.0/ebin"
    echo "-module(${app})." > "${BUILD_DIR}/lib/${app}-1.0/ebin/${app}.beam"
done

echo "[build-otp-musl] Prepared Erlang/OTP release tree structure in ${BUILD_DIR}"
