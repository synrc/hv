Synrc Hypervision
=================

Synrc Hypervision is an approch and specification for running unmodified Erlang/OTP BEAM virtual machines
within isolated syscall provider Tyn side to side with Apline Linux PD for drivers inside seL4 microkit.

Requirements
------------

* Bare-metal Type-1 lightweight hypervisor (sel4)
* Isolation syscall layer for BEAM (tyn)
* Unmodified Ericsson BEAM (beam)
* Smallest scalable (from DC to IoT) Linux for drivers (alpine)

Abstract
--------

The Erlang/OTP BEAM runtime provides robust concurrency, fault isolation,
and soft real-time behaviour that make it attractive for high-availability systems,
yet it traditionally depends on a large general-purpose operating system whose
trusted computing base (TCB) undermines strong security and certification arguments.
We present a hybrid architecture that hosts an unmodified BEAM on the formally verified
seL4 microkernel while retaining practical device support through a minimal Alpine Linux guest.

A purpose-built thin host (Tyn) implements only the small set of Linux-compatible system calls
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

Tree
----

```
hv/
├── README.md                       # Project overview, quick start, architecture summary, build instructions
├── LICENSE                         # BSD-2-Clause (seL4-compatible)
├── Makefile                        # Top-level build entry point (BOARD, CONFIG, feature flags)
│
├── docs/                           # Human- and reviewer-oriented documentation
│   ├── architecture.md             # Detailed component + morphism description (matches paper §2)
│   ├── nist-800-53.md              # Full control mapping table + rationale
│   └── paper-artifact.md           # How to reproduce every claim / figure / measurement in the paper
│
├── boards/                         # Board-specific support
│   ├── qemu_virt_aarch64/          # For reviewers who lack Pi 4 hardware
│   └── rpi4b_8gb/                  # Raspberry Pi 4 (8 GB) platform files, memory map, UART, device tree fragments
│
├── systems/                        # Microkit system descriptions (static architecture)
│   ├── hello.system                # Minimal single-PD bring-up (Phase 0)
│   ├── linux.system                # VMM + Alpine guest only (no Tyn/BEAM)
│   ├── tyn-beam.system             # BEAM only (no Linux)
│   └── hv.system                   # Synrc Hypervision: Tyn + BEAM + Alpine driver VM
│
├── pds/                            # Source for each protection domain
│   ├── hello/                      # Trivial “hello world” PD (serial output)
│   ├── vmm/                        # libvmm-based VMM PD that starts the Alpine guest
│   ├── tyn/                        # Adapted Tyn (syscall trap + BEAM host) as a Microkit PD
│   ├── console/                    # UART / console PD (exclusive device ownership)
│   └── monitor/                    # Metrics & audit PD (NIST AU support)
│
├── linux/                          # Linux guest artefacts (Alpine)
│   └── alpine/
│       ├── rootfs/                 # Minimal Alpine root filesystem
│       ├── kernel-config           # Kernel .config (UIO, target drivers, minimal features)
│       └── uio-helper/             # Userspace bridge: Linux drivers ↔ sDDF rings
│
├── beam/                           # OTP / BEAM packaging
│   ├── rel/                        # Built OTP release (musl-linked)
│   └── embed.mk                    # Rules that turn the release into a cpio/tar embedded in Tyn
│
├── third_party/                    # Upstream dependencies (git submodules, pinned commits)
│   ├── libvmm/                     # seL4/Microkit VMM library
│   ├── sddf/                       # seL4 Device Driver Framework
│   └── tyn/                        # Upstream Tyn kernel (reference; adaptations live in pds/tyn/)
│
├── scripts/                        # Automation (Python-free)
│   ├── build-linux.sh              # Produce Alpine rootfs + kernel
│   ├── build-image.sh              # Assemble final Microkit loader.img
│   └── flash-sd.sh                 # Write image + firmware to microSD
│
└── evaluation/                     # Paper artefact support
    ├── benchmarks/                 # Workload scripts (BEAM, network, storage)
    ├── measurements/               # Raw data, logs, timing results
    └── figures/                    # Source for paper figures (architecture diagrams, graphs)
```

Article
-------

* [1]. Namdak Tonpa. [Synrc Hypervision](https://hv.synrc.com/hv.pdf). 2026

Credits
-------

* Namdak Tonpa
