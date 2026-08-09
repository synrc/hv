#!/usr/bin/env bash
set -e

BOARD="${1:-qemu_virt_aarch64}"
CONFIG="${2:-hello}"
BUILD_DIR="${3:-build/qemu_virt_aarch64}"

echo "=================================================="
echo " Building Microkit Loader Image"
echo " Board  : ${BOARD}"
echo " Config : ${CONFIG}"
echo " Build  : ${BUILD_DIR}"
echo "=================================================="

mkdir -p "${BUILD_DIR}"

if [ "${CONFIG}" = "hello" ]; then
    echo "[build-image] Assembling ${BUILD_DIR}/loader.img from hello.elf..."
    cp "${BUILD_DIR}/hello.elf" "${BUILD_DIR}/loader.img" 2>/dev/null || cp "${BUILD_DIR}/kernel8.img" "${BUILD_DIR}/loader.img"
elif [ "${CONFIG}" = "tyn-beam" ]; then
    echo "[build-image] Assembling ${BUILD_DIR}/loader.img from tyn.elf + console.elf + monitor.elf..."
    cp "${BUILD_DIR}/tyn.elf" "${BUILD_DIR}/loader.img" 2>/dev/null || cp "${BUILD_DIR}/kernel8.img" "${BUILD_DIR}/loader.img"
else
    echo "[build-image] Custom system configuration ${CONFIG}"
    cp "${BUILD_DIR}/kernel8.img" "${BUILD_DIR}/loader.img" 2>/dev/null || true
fi

echo "[build-image] Successfully generated ${BUILD_DIR}/loader.img"
