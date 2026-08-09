#!/usr/bin/env bash

IMG="${1:-build/qemu_virt_aarch64/loader.img}"

if [ ! -f "${IMG}" ]; then
    IMG="kernel8.img"
fi

qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=2 \
  -cpu cortex-a53 \
  -m 2048M \
  -nographic \
  -serial mon:stdio \
  -kernel "${IMG}"
