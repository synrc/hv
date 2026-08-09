#!/usr/bin/env bash
set -e

DEV="${1:-/dev/rdisk2}"
IMAGE="${2:-build/rpi4b_8gb/loader.img}"

if [ ! -f "${IMAGE}" ]; then
    echo "Image ${IMAGE} not found!"
    exit 1
fi

echo "Flashing ${IMAGE} to ${DEV}..."
echo "dd if=${IMAGE} of=${DEV} bs=4M status=progress"
