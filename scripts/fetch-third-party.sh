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

echo "=== Third Party Dependencies Refetched Successfully ==="
ls -la "${THIRD_PARTY_DIR}"
