# Paper Artifact & Reproduction Guide

This document describes how to reproduce the system setup, build steps, and execution verification described in the paper *Synrc Hypervision: High-Assurance BEAM on seL4 with Alpine VMM for Drivers*.

## Host Prerequisites

- **Host Operating System**: macOS (ARM64 / x86_64) or Linux
- **Cross Compiler**: Clang with `-target aarch64-none-elf` or `aarch64-none-elf-gcc`
- **Emulator**: `qemu-system-aarch64` (QEMU 8.0+)
- **Build Tools**: `make`, `python3`, `bash`

---

## Phase 1 Reproduction: Minimal Bring-Up (`hello.system`)

1. Build Phase 1 image for QEMU:
   ```bash
   make clean
   make BOARD=qemu_virt_aarch64 CONFIG=hello
   ```

2. Run on QEMU:
   ```bash
   ./run.sh
   ```

3. Expected Output:
   ```
   [hello] Microkit Protection Domain initialized successfully!
   [hello] Synrc Hypervision Phase 1 bring-up operational.
   ```

---

## Phase 2 Reproduction: Tyn & BEAM Foundation (`tyn-beam.system`)

1. Build Phase 2 image for QEMU:
   ```bash
   make clean
   make BOARD=qemu_virt_aarch64 CONFIG=tyn-beam
   ```

2. Run on QEMU:
   ```bash
   ./run.sh
   ```

3. Expected Output:
   ```
   [console] Console Protection Domain active (exclusive UART owner)
   [monitor] Monitor PD active (NIST AU audit logger initialized)
   [tyn] Tyn host trap dispatcher online (50 musl syscall handlers ready)
   [tyn] Embedded BEAM release loaded from in-memory VFS
   [tyn] Applications: kernel, stdlib, crypto, compiler, asn1, up
   [tyn] Starting Erlang/OTP interactive shell...
   Erlang/OTP 26 [erts-14.0] [source] [64-bit] [smp:1:1] [async-threads:1]
   Eshell V14.0  (abort with ^G)
   1>
   ```
