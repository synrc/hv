# Synrc Hypervision (OS.1) Top-Level Makefile (Native Microkit SDK)

MICROKIT_SDK ?= third_party/microkit-sdk-2.3.0
BOARD        ?= qemu_virt_aarch64
CONFIG       ?= smp-debug
SYSTEM       ?= synrc-beam
TARGET       ?= aarch64-none-elf

CC           = clang
LD           = /opt/homebrew/bin/ld.lld
MICROKIT     = $(MICROKIT_SDK)/bin/microkit

BOARD_DIR    = $(MICROKIT_SDK)/board/$(BOARD)/$(CONFIG)
INC_FLAGS    = -I$(BOARD_DIR)/include -Ipds/console -Ipds/synrc
CFLAGS       = -target $(TARGET) -mgeneral-regs-only -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -O2 $(INC_FLAGS)
LDFLAGS      = -T $(BOARD_DIR)/lib/microkit.ld $(BOARD_DIR)/lib/libmicrokit.a

BUILD_DIR    = build/$(BOARD)

.PHONY: all clean run setup

all: $(BUILD_DIR)/loader.img

$(BUILD_DIR)/hello.elf: pds/hello/hello.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/hello/hello.c -o $(BUILD_DIR)/hello.o
	$(LD) $(BUILD_DIR)/hello.o $(LDFLAGS) -o $@

$(BUILD_DIR)/console.elf: pds/console/console.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/console/console.c -o $(BUILD_DIR)/console.o
	$(LD) $(BUILD_DIR)/console.o $(LDFLAGS) -o $@

$(BUILD_DIR)/monitor.elf: pds/monitor/monitor.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/monitor/monitor.c -o $(BUILD_DIR)/monitor.o
	$(LD) $(BUILD_DIR)/monitor.o $(LDFLAGS) -o $@

$(BUILD_DIR)/synrc.elf: pds/synrc/synrc_main.c pds/synrc/syscall_trap.c pds/synrc/vfs.c pds/synrc/beam_loader.c pds/synrc/beam_emulator.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c pds/synrc/synrc_main.c -o $(BUILD_DIR)/synrc_main.o
	$(CC) $(CFLAGS) -c pds/synrc/syscall_trap.c -o $(BUILD_DIR)/syscall_trap.o
	$(CC) $(CFLAGS) -c pds/synrc/vfs.c -o $(BUILD_DIR)/vfs.o
	$(CC) $(CFLAGS) -c pds/synrc/beam_loader.c -o $(BUILD_DIR)/beam_loader.o
	$(CC) $(CFLAGS) -c pds/synrc/beam_emulator.c -o $(BUILD_DIR)/beam_emulator.o
	$(LD) $(BUILD_DIR)/synrc_main.o $(BUILD_DIR)/syscall_trap.o $(BUILD_DIR)/vfs.o $(BUILD_DIR)/beam_loader.o $(BUILD_DIR)/beam_emulator.o $(LDFLAGS) -o $@

WORKER_ELFS = beam_sched.elf beam_dirty_cpu.elf beam_dirty_io.elf beam_poll.elf beam_rq_super.elf beam_aux.elf beam_worker6.elf beam_worker7.elf

$(BUILD_DIR)/%.elf: pds/synrc/%.c $(BUILD_DIR)/vfs.o
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/$*.o
	$(LD) $(BUILD_DIR)/$*.o $(BUILD_DIR)/vfs.o $(LDFLAGS) -o $@

$(BUILD_DIR)/loader.img: $(BUILD_DIR)/hello.elf $(BUILD_DIR)/console.elf $(BUILD_DIR)/monitor.elf $(BUILD_DIR)/synrc.elf $(patsubst %,$(BUILD_DIR)/%,$(WORKER_ELFS)) systems/$(SYSTEM).system
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
	  -m 4096M \
	  -smp 4 \
	  -nographic \
	  -serial mon:stdio \
	  -kernel $(BUILD_DIR)/loader.img \
	  $$BEAM_DEV $$CPIO_DEV

# Option B: objcopy embed — self-contained loader-embedded.img (adds ~11 MB)
embed: all
	@if [ ! -f $(BEAM_ELF) ]; then echo "ERROR: $(BEAM_ELF) not found. Run 'make build-beam-aarch64' first."; exit 1; fi
	@if [ ! -f $(BEAM_CPIO) ]; then echo "ERROR: $(BEAM_CPIO) not found."; exit 1; fi
	llvm-objcopy --add-section .beam_image=$(BEAM_ELF) \
	             --set-section-flags .beam_image=load,alloc \
	             --change-section-address .beam_image=0x50000000 \
	             $(BUILD_DIR)/synrc.elf $(BUILD_DIR)/synrc.elf
	llvm-objcopy --add-section .otp_rootfs=$(BEAM_CPIO) \
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

clean:
	rm -rf build/qemu_virt_aarch64/

# Remove everything including OTP cross-compile tree
distclean:
	rm -rf build/
