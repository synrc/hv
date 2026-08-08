qemu-system-aarch64 \
  -machine virt,virtualization=on,gic-version=2 \
  -cpu cortex-a76 \
  -m 512M \
  -nographic \
  -semihosting \
  -kernel kernel8.img
