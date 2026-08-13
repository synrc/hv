#!/usr/bin/env bash
# Synrc Hypervision (OS.1) - Third Party Dependency Fetch & Recreate Script
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"

echo "=== Synrc Hypervision (OS.1) Recreating Third Party Directory ==="
echo "Target directory: ${THIRD_PARTY_DIR}"

rm -rf "${THIRD_PARTY_DIR}"
mkdir -p "${THIRD_PARTY_DIR}"

echo "[1/3] Fetching au-ts/libvmm..."
git clone --depth 1 https://github.com/au-ts/libvmm.git "${THIRD_PARTY_DIR}/libvmm"

echo "[2/3] Fetching au-ts/sddf..."
git clone --depth 1 https://github.com/au-ts/sddf.git "${THIRD_PARTY_DIR}/sddf"

echo "[3/3] Fetching seL4 Microkit SDK (2.3.0)..."
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ]; then ARCH="aarch64"; fi
if [ "$OS" = "darwin" ]; then OS="macos"; fi

SDK_TAR="microkit-sdk-2.3.0-${OS}-${ARCH}.tar.gz"
SDK_URL="https://github.com/seL4/microkit/releases/download/2.3.0/${SDK_TAR}"

echo "Downloading ${SDK_URL}..."
curl -L -s -o "${THIRD_PARTY_DIR}/${SDK_TAR}" "${SDK_URL}"
tar -xf "${THIRD_PARTY_DIR}/${SDK_TAR}" -C "${THIRD_PARTY_DIR}/"
rm "${THIRD_PARTY_DIR}/${SDK_TAR}"

echo "=== Third Party Dependencies Refetched Successfully ==="
ls -la "${THIRD_PARTY_DIR}"
