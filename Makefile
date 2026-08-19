# Synrc Hypervision (OS.1) Top-Level Makefile (Native Microkit SDK)

REPO_ROOT    := $(CURDIR)
MICROKIT_SDK ?= $(REPO_ROOT)/third_party/microkit-sdk-2.3.0
BOARD        ?= qemu_virt_aarch64
CONFIG       ?= debug
SYSTEM       ?= synrc-beam
TARGET       ?= aarch64-none-elf
CC           = clang
ifeq ($(shell uname -s),Darwin)
  LD = /opt/homebrew/bin/ld.lld
else
  LD = ld.lld
endif
MICROKIT     = $(MICROKIT_SDK)/bin/microkit
BOARD_DIR    = $(MICROKIT_SDK)/board/$(BOARD)/$(CONFIG)
INC_FLAGS    = -I$(BOARD_DIR)/include -Ipds/console -Ipds/synrc
CFLAGS       = -target $(TARGET) -mgeneral-regs-only -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -O2 $(INC_FLAGS)
MK_LDFLAGS   = -T $(BOARD_DIR)/lib/microkit.ld $(BOARD_DIR)/lib/libmicrokit.a
BUILD_DIR    = build/$(BOARD)

.PHONY: all clean run setup

all: $(BUILD_DIR)/loader.img

$(BUILD_DIR)/hello.elf: pds/hello/hello.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/hello/hello.c -o $(BUILD_DIR)/hello.o
	$(LD) $(BUILD_DIR)/hello.o $(MK_LDFLAGS) -o $@

$(BUILD_DIR)/console.elf: pds/console/console.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/console/console.c -o $(BUILD_DIR)/console.o
	$(LD) $(BUILD_DIR)/console.o $(MK_LDFLAGS) -o $@

$(BUILD_DIR)/monitor.elf: pds/monitor/monitor.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/monitor/monitor.c -o $(BUILD_DIR)/monitor.o
	$(LD) $(BUILD_DIR)/monitor.o $(MK_LDFLAGS) -o $@

$(BUILD_DIR)/synrc.elf: pds/synrc/synrc_main.c pds/synrc/syscall_trap.c pds/synrc/vfs.c pds/synrc/beam_loader.c pds/synrc/beam_emulator.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/synrc/synrc_main.c -o $(BUILD_DIR)/synrc_main.o
	$(CC) $(CFLAGS) -c pds/synrc/syscall_trap.c -o $(BUILD_DIR)/syscall_trap.o
	$(CC) $(CFLAGS) -c pds/synrc/vfs.c -o $(BUILD_DIR)/vfs.o
	$(CC) $(CFLAGS) -c pds/synrc/beam_loader.c -o $(BUILD_DIR)/beam_loader.o
	$(CC) $(CFLAGS) -c pds/synrc/beam_emulator.c -o $(BUILD_DIR)/beam_emulator.o
	$(LD) $(BUILD_DIR)/synrc_main.o $(BUILD_DIR)/syscall_trap.o $(BUILD_DIR)/vfs.o $(BUILD_DIR)/beam_loader.o $(BUILD_DIR)/beam_emulator.o $(MK_LDFLAGS) -o $@

$(BUILD_DIR)/loader.img: $(BUILD_DIR)/hello.elf $(BUILD_DIR)/console.elf $(BUILD_DIR)/monitor.elf $(BUILD_DIR)/synrc.elf systems/$(SYSTEM).system
	$(MICROKIT) systems/$(SYSTEM).system \
	  --search-path $(BUILD_DIR) \
	  --board $(BOARD) \
	  --config $(CONFIG) \
	  -o $@

BEAM_ELF  = build/beam.aarch64.elf
# Prefer OTP 20 rootfs (unversioned paths); fall back to original OTP 26 cpio
BEAM_CPIO = $(or $(wildcard build/otp-rootfs-20.cpio),build/otp-rootfs.cpio)

# Option A: QEMU -device loader (default)
# Loads beam.aarch64.elf at 0x50000000 and otp-rootfs.cpio at 0x54000000
# if the files exist (produced by make build-beam-aarch64).
run: all
	@BEAM_DEV=""; CPIO_DEV=""; \
	if [ -f $(BEAM_ELF) ]; then \
	  BEAM_DEV="-device loader,file=$(BEAM_ELF),addr=0x50000000,force-raw=on"; \
	fi; \
	if [ -f $(BEAM_CPIO) ]; then \
	  CPIO_DEV="-device loader,file=$(BEAM_CPIO),addr=0x54000000,force-raw=on"; \
	fi; \
	qemu-system-aarch64 \
	  -machine virt,virtualization=on,gic-version=2 \
	  -cpu cortex-a53 \
	  -m 2048M \
	  -nographic \
	  -serial mon:stdio \
	  -kernel $(BUILD_DIR)/loader.img \
	  $$BEAM_DEV $$CPIO_DEV

# Option B: objcopy embed — self-contained loader-embedded.img (adds ~11 MB)
embed:
	@if [ ! -f $(BEAM_ELF) ]; then echo "ERROR: $(BEAM_ELF) not found. Run 'make build-beam-aarch64' first."; exit 1; fi
	@if [ ! -f $(BEAM_CPIO) ]; then echo "ERROR: $(BEAM_CPIO) not found."; exit 1; fi
	rm -f $(BUILD_DIR)/synrc.elf
	$(MAKE) all
	aarch64-linux-musl-objcopy --add-section .beam_image=$(BEAM_ELF) \
	             --set-section-flags .beam_image=load,alloc \
	             --change-section-address .beam_image=0x50000000 \
	             $(BUILD_DIR)/synrc.elf $(BUILD_DIR)/synrc.elf
	aarch64-linux-musl-objcopy --add-section .otp_rootfs=$(BEAM_CPIO) \
	             --set-section-flags .otp_rootfs=load,alloc \
	             --change-section-address .otp_rootfs=0x54000000 \
	             $(BUILD_DIR)/synrc.elf $(BUILD_DIR)/synrc.elf
	$(MICROKIT) systems/$(SYSTEM).system \
	  --search-path $(BUILD_DIR) \
	  --board $(BOARD) \
	  --config $(CONFIG) \
	  -o $(BUILD_DIR)/loader-embedded.img
	@echo "Self-contained image: $(BUILD_DIR)/loader-embedded.img"

# Build OTP 20 non-SMP BEAM for AArch64 using Homebrew musl-cross toolchain
build-beam-aarch64:
	bash beam-build/build-beam-aarch64.sh

.PHONY: proxmox
proxmox: embed
	@echo "=== Producing Proxmox-compatible artifacts ==="
	@mkdir -p $(BUILD_DIR)
	@if [ ! -f $(BUILD_DIR)/loader-embedded.img ]; then \
	  echo "ERROR: $(BUILD_DIR)/loader-embedded.img not found."; exit 1; \
	fi
	@echo "Converting loader-embedded.img to QCOW2..."
	qemu-img convert -f raw -O qcow2 \
	  $(BUILD_DIR)/loader-embedded.img \
	  $(BUILD_DIR)/hv-proxmox.qcow2
	@echo "Generating Proxmox VE VM creation script..."
	@printf '#!/usr/bin/env bash\n\
# Automated Proxmox VE VM deployment script for Synrc Hypervision (OS.1)\n\
set -euo pipefail\n\
VMID=$${1:-9000}\n\
echo "Creating AArch64 VM $${VMID} on Proxmox..."\n\
qm create "$${VMID}" \\\n\
  --name hv-sel4-beam \\\n\
  --arch aarch64 \\\n\
  --machine virt \\\n\
  --cpu max \\\n\
  --cores 2 \\\n\
  --memory 2048 \\\n\
  --ostype l26 \\\n\
  --onboot 0\n\
echo "Copying loader-embedded.img to PVE templates directory..."\n\
mkdir -p /var/lib/vz/template/qemu\n\
cp -f ./loader-embedded.img /var/lib/vz/template/qemu/loader-embedded.img\n\
echo "Configuring Direct Kernel Boot and Serial Console..."\n\
qm set "$${VMID}" \\\n\
  --serial0 socket \\\n\
  --kernel /var/lib/vz/template/qemu/loader-embedded.img\n\
echo "VM $${VMID} successfully created."\n\
echo "To boot:   qm start $${VMID}"\n\
echo "To view:   qm terminal $${VMID}"\n' > $(BUILD_DIR)/deploy-hv-proxmox.sh
	@chmod +x $(BUILD_DIR)/deploy-hv-proxmox.sh
	@echo ""
	@echo "=== Build Complete ==="
	@echo "1. Image:  $(BUILD_DIR)/hv-proxmox.qcow2"
	@echo "2. Deploy script: $(BUILD_DIR)/deploy-hv-proxmox.sh"
	@echo ""
	@echo "To deploy on your Proxmox VE host:"
	@echo "  scp $(BUILD_DIR)/loader-embedded.img $(BUILD_DIR)/deploy-hv-proxmox.sh root@YOUR_PROXMOX_IP:~/"
	@echo "  ssh root@YOUR_PROXMOX_IP './deploy-hv-proxmox.sh [VMID]'"
	@echo "  ssh root@YOUR_PROXMOX_IP 'qm terminal [VMID]'"

clean:
	rm -rf build/qemu_virt_aarch64/


test:
	ruby tests/crypto_test.rb

# Remove everything including OTP cross-compile tree
distclean:
	rm -rf build/
