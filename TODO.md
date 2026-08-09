All steps below avoid Python. Microkit itself is a native binary; we will not use any Python-based tools or scripts.

1. macOS Host Setup (Python-free)
Bash# Core tools only
brew install make dtc qemu git curl coreutils

# AArch64 Linux cross toolchain
brew tap messense/macos-cross-toolchains
brew install aarch64-unknown-linux-gnu

# Optional
brew install llvm
Do not install python@3.12 or any Python packages.
Microkit SDK
Apple Silicon:
Bashcd ~
curl -L https://github.com/seL4/microkit/releases/download/2.3.0/microkit-sdk-2.3.0-macos-aarch64.tar.gz -o sdk.tar.gz
tar xf sdk.tar.gz
export MICROKIT_SDK=$HOME/microkit-sdk-2.3.0
echo 'export MICROKIT_SDK=$HOME/microkit-sdk-2.3.0' >> ~/.zshrc
echo 'export PATH="$MICROKIT_SDK/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
Intel Mac: use the macos-x86-64 tarball instead.
Rust (for Tyn later)
Bashcurl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add aarch64-unknown-none
Repositories
Bashmkdir -p ~/tynse && cd ~/tynse
git clone https://github.com/au-ts/libvmm.git
git clone https://github.com/au-ts/sddf.git
git clone https://github.com/tyn-os/kernel.git tyn

2. Phase 0 – Hello (still pure C + Microkit)
Bashmkdir -p ~/tynse/hello && cd ~/tynse/hello
hello.c
C#include <microkit.h>
#include <stdio.h>

void init(void) {
    printf("Hello from seL4/Microkit on Pi 4\n");
}

void notified(microkit_channel ch) {}
hello.system
XML<?xml version="1.0" encoding="UTF-8"?>
<system>
  <protection_domain name="hello" priority="254">
    <program_image path="hello.elf" />
  </protection_domain>
</system>
Build (no Python)
BashBOARD=rpi4b_8gb
CONFIG=debug

$MICROKIT_SDK/bin/microkit hello.system \
  --search-path . \
  --board $BOARD \
  --config $CONFIG \
  -o loader.img
Flash loader.img + Raspberry Pi firmware. In config.txt:
textkernel=loader.img
arm_64bit=1
enable_uart=1

3. Alpine Guest (Python-free)
Create the Alpine rootfs on a Linux machine or in Docker (still no Python required for the final artifact).
On a Linux host / Docker (example):
Bash# Official minirootfs – pure tarball, no Python
ALPINE_VER=3.21.0
curl -L https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/alpine-minirootfs-${ALPINE_VER}-aarch64.tar.gz -o alpine.tar.gz
mkdir rootfs
tar -xzf alpine.tar.gz -C rootfs

# Add packages with apk (no Python)
# (run under proot/qemu-user or on real aarch64)
apk add --no-cache busybox openrc uio-tools iproute2
Copy the finished rootfs/ and a suitable Linux kernel Image to the Mac. All further work on the Mac stays Python-free.

4. libvmm + Alpine
libvmm examples are Makefile + C based. Replace the guest kernel/rootfs paths with your Alpine artifacts. No Python is involved in the Microkit or libvmm build.

5. UIO helper
Write the helper in plain C (or Rust). Cross-compile on macOS:
Bashaarch64-unknown-linux-gnu-gcc -static -O2 -o uio-helper uio-helper.c
Place the binary into the Alpine rootfs. No Python.

6. Tyn + BEAM
Tyn is Rust. OTP is built with its normal C toolchain.
Keep all build scripts in Make / shell / Cargo. Do not introduce Python.

7. Rules going forward

Microkit tool = native binary only
All build logic = Makefile + shell + Cargo
Guest userspace helpers = C or Rust
No python, pip, sdfgen, or any Python-based generator in the critical path


Immediate next actions (macOS, no Python)

Install the packages listed in section 1.
Build and boot the hello system on real Pi 4.
Get the libvmm “simple” example running under QEMU on the Mac.
Swap in a pre-built Alpine rootfs + kernel.
Continue with UIO helper and sDDF in pure C/Rust.

====================================================


OTP 20 non-SMP AArch64
Both embedding options (QEMU device loader by default, objcopy target for self-contained)
Homebrew musl-cross toolchain

