Synrc Hypervision
=================

Synrc Hypervision is an approch and specification for running unmodified unicore Erlang/OTP BEAM virtual machines
within novel isolated syscall provider `synrc` side to side with `Apline Linux` PD for drivers inside seL4 microkit.

Requirements
------------

* Bare-metal Type-1 lightweight hypervisor (sel4)
* Isolation seL4 Microkit syscall layer for BEAM (synrc) 1K LOC unikernel for VirtIO API (QEMU/KVM/Proxmox)
* Unmodified Ericsson BEAM (beam)
* Smallest scalable (from DC to IoT) Linux for drivers (alpine)

Try
---

Zero you need is to build muscl-cross in Alpine/Ubuntu WSL, in macOS you can build muscl-cross using brew.

### Alpine/Chimera Linux

```
$ apk update
$ apk add binutils binutils-aarch64 binutils-aarch64-none-elf build-base \
      gcc-aarch64-none-elf perl m4 ncurses-dev openssl-dev lld clang \
      qemu-aarch64 qemu-system-aarch64 autoconf mc ruby make tar zip
```

### Debian/Ubuntu Linux

```
$ apt update
$ apt install build-essential binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu \
      perl m4 libncurses-dev libssl-dev lld clang qemu-user qemu-system-arm \
      autoconf mc ruby make tar zip
```

First you need is to build patched LibreSSL 3.1.5.

```
$ ./scripts/fetch-third-party.sh
$ ./scripts/build-ssl.sh
```

Second you need is to build patched last unicore Erlang/OTP 20.0 then build HV Unicore release.

```
$ make build-beam-aarch64
$ make clean && make all && make run
```

Abstract
--------

The Erlang/OTP BEAM runtime provides robust concurrency, fault isolation,
and soft real-time behaviour that make it attractive for high-availability systems,
yet it traditionally depends on a large general-purpose operating system whose
trusted computing base (TCB) undermines strong security and certification arguments.
We present a hybrid architecture that hosts an unmodified BEAM on the formally verified
seL4 microkernel while retaining practical device support through a minimal Alpine Linux guest.

A purpose-built thin host (synrc) implements only the small set of Linux-compatible system calls
required by a musl-linked OTP runtime and executes as an isolated seL4 protection domain under
the Microkit framework. Device drivers that would otherwise enlarge the TCB are
confined to an Alpine Linux virtual machine managed by a lightweight virtual-machine monitor;
communication between the BEAM domain and the driver domain occurs exclusively through
capability-mediated shared-memory channels defined by the seL4 Device Driver Framework (sDDF).
The resulting system therefore combines the formal isolation guarantees of seL4, the minimal
attack surface of a BEAM-specific host, and the hardware coverage of an existing Linux driver ecosystem.

We describe the static system architecture, the mapping of selected NIST SP 800-53 control
families onto seL4 capabilities and compile-time configuration, and a concrete bootstrap
path on commodity hardware (Raspberry Pi 4). The design demonstrates that a production-grade
BEAM application can operate with a dramatically reduced TCB while preserving the ability to
utilise complex devices, offering a practical route toward higher-assurance, certifiable distributed systems.

### Binary Ports

Erlang's traditional `open_port` mechanism relies on POSIX `fork`/`exec` which contradicts
seL4's static architecture. For the current release, we support external binaries by
converting them into **Statically Linked Erlang Drivers** (C code linked directly into the BEAM).
In future releases, we plan to implement true hardware-isolated ports by mapping them to
independent Microkit Protection Domains (PDs) communicating via shared memory IPC.

Tree
----

```
hv/
├── README.md                       # Project overview, quick start, architecture summary, build instructions
├── LICENSE                         # BSD-2-Clause (seL4-compatible)
├── Makefile                        # Top-level build entry point (BOARD, CONFIG, feature flags)
│
├── boards/                         # Board-specific support
│   ├── qemu_virt_aarch64/          # For reviewers who lack Pi 4 hardware
│   └── rpi4b_8gb/                  # Raspberry Pi 4 (8 GB) platform files, memory map, UART, device tree fragments
│
├── systems/                        # Microkit system descriptions (static architecture)
│   ├── hello.system                # Minimal single-PD bring-up (Phase 0)
│   ├── linux.system                # VMM + Alpine guest only (no BEAM)
│   ├── synrc-beam.system           # BEAM only (no Linux)
│   └── hv.system                   # Synrc Hypervision: seL4 + Synrc BEAM + Alpine driver VM
│
├── pds/                            # Source for each protection domain
│   ├── hello/                      # Trivial “hello world” PD (serial output)
│   ├── vmm/                        # libvmm-based VMM PD that starts the Alpine guest
│   ├── synrc/                      # Adapted Tyn (syscall trap + BEAM host) as a Microkit PD
│   ├── console/                    # UART / console PD (exclusive device ownership)
│   └── monitor/                    # Metrics & audit PD (NIST AU support)
│
├── linux/                          # Linux guest artefacts (Alpine)
│   └── alpine/
│       ├── rootfs/                 # Minimal Alpine root filesystem
│       ├── kernel-config           # Kernel .config (UIO, target drivers, minimal features)
│       └── uio-helper/             # Userspace bridge: Linux drivers ↔ sDDF rings
│
├── third_party/                    # Upstream dependencies (git submodules, pinned commits)
│   ├── libvmm/                     # seL4/Microkit VMM library
│   └── sddf/                       # seL4 Device Driver Framework
│
├── scripts/                        # Automation (Python-free)
│   ├── build-linux.sh              # Produce Alpine rootfs + kernel
│   └── fetch-third-party.sh        # Fetch Erlang/OTP and VMM
│
├── patches/                        # Patches for Elrang/OTP
│   ├── 0001-ttsl_drv-baremetal.patch
│   └── 0002-sys_drivers-baremetal.patch
│
└── beam-build/                     # Erlang/OTP 20
    └── build-beam-aarch64.sh       # Build for muslc, Alpine and seL4
```

Article
-------

Note that this project doesn't depend on Rust or Cloudozer code.
However there are some portions of Rust in seL4 bootstrapping process.
For Erlang/OTP 18.0 for Citrix/Xen API Hypervisor look for LING `cloudozer/ling` source code.
There is no SSL and LwIP as TCP/IP stack, same as in HV `synrc/hv`.

* [1]. Namdak Tonpa. [Synrc Hypervision](https://hv.synrc.com/hv.pdf). 2026
* [2]. Namdak Tonpa. [BEAMP.SMP](https://hv.synrc.com/hv-smp.pdf). 2026
* [2]. Namdak Tonpa. [Byte-code Interpreters](https://hv.synrc.com/hv-bc.pdf). 2026
* [3]. Namdak Tonpa. [Infosec OS.1](https://hv.synrc.com/hv-infosec.pdf). 2026

Tests and Debugging
-------------------

This project includes a Ruby-based automated verification test suite to validate
that the cross-compiled BEAM emulator runs on the seL4 guest, that the `crypto`
NIF starts automatically, and that cryptographic functions (e.g., MD5 hashing)
run successfully. To run the verification test:

```bash
$ ruby tests/crypto_test.rb
```

### Debugging Procedures

If the compilation, bootstrapping, or tests break, follow these troubleshooting steps:

1. **Verify LibreSSL installation**:
   Ensure `third_party/libressl-3.1.5` was compiled and installed for `aarch64-linux-musl`:
   ```bash
   $ ls -la third_party/libressl-3.1.5/install-aarch64/lib/libcrypto.a
   ```

2. **Toolchain Sysroot**:
   The cross-compilation configure script queries the target compiler's sysroot dynamically (`aarch64-linux-musl-gcc -print-sysroot`). If headers are not found or conflict with macOS host headers, ensure this command outputs the correct musl sysroot path.

3. **Stray `make_boot.beam` Conflict**:
   Erlang's code loader prioritizes the current directory (`.`). Ensure there is no stray `make_boot.beam` file in the root directory:
   ```bash
   $ rm -f make_boot.beam
   ```

4. **Interactive Eshell Inspection**:
   Run the VM manually (`make run`), wait for the prompt, and check the applications status and cryptographic functionality:
   ```erlang
   1> application:which_applications().
   2> crypto:hash(md5, <<"test">>).
   ```

Credits
-------

* Namdak Tonpa
