#!/bin/bash
BEAM_ELF="third_party/tyn/src/beam.aarch64.elf"
CPIO_DEV="-device loader,file=third_party/tyn/src/otp-rootfs-20.cpio,addr=0x54000000,force-raw=on"
qemu-system-aarch64 -machine virt,virtualization=on,gic-version=2 -cpu cortex-a53 -m 2048M -nographic -serial mon:stdio -kernel build/qemu_virt_aarch64/loader.img -device loader,file=$BEAM_ELF,addr=0x50000000,force-raw=on $CPIO_DEV -s > qemu_output_3.txt 2>&1 &
QEMU_PID=$!
sleep 4
/opt/homebrew/bin/aarch64-elf-gdb -batch -ex "target remote localhost:1234" -ex "set architecture aarch64" -ex "bt" -ex "info registers" -ex "x/20i \$pc - 20" -ex "quit" > gdb_batch_3.txt 2>&1
kill -9 $QEMU_PID
cat gdb_batch_3.txt
