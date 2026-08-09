Synrc Hypervision
=================

* Bare-metal Type-1 lightweight hypervisor (sel4)
* POSIX minimal layer for BEAM (tyn)
* Unmodified Ericsson BEAM (beam)

Story
-----

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

Article
-------

* [1]. Namdak Tonpa. [Synrc Hypervision](https://hv.synrc.com/hv.pdf). 2026

Credits
-------

* Namdak Tonpa
