# Synrc Hypervision Architecture (OS.1)

Synrc Hypervision is an approach and specification for running unmodified Erlang/OTP BEAM virtual machines within an isolated syscall provider (Tyn) side-by-side with an Alpine Linux driver VM inside seL4 Microkit.

## OS.1 Naming Taxonomy

1. **OS.1: Hypervisor (seL4 Microkernel)**
   - Formally verified microkernel offering capability-based access control, spatial/temporal isolation, and static component layout.
   - Uses seL4 Mixed-Criticality Scheduling (MCS) for static budget and period allocation.

2. **OS.1: System (Alpine Linux VMM)**
   - Minimal Alpine Linux guest operating as a driver virtual machine managed by `libvmm`.
   - Isolates complex Linux device drivers (Ethernet, Storage) from the trusted computing base (TCB).
   - Translates between Linux driver interfaces and seL4 Device Driver Framework (sDDF) shared-memory rings via a userspace `uio-helper`.

3. **OS.1: Language (Erlang/OTP BEAM)**
   - Unmodified musl-linked Erlang/OTP runtime hosting applications (`kernel`, `stdlib`, `crypto`, `compiler`, `asn1`, `up`, `shell`).
   - Hosted by **Tyn**, a thin protection domain implementing ~50 Linux system call traps required by musl/BEAM.

---

## Component Topology & Control Flow

```
                                OS.1: Language
                    +-------------------------------------+
                    |       BEAM Virtual Machine          |
                    |  (unmodified OTP, musl-linked rel)  |
                    +-------------------------------------+
                                       | (Linux syscall traps)
                    +-------------------------------------+
                    |       Tyn Protection Domain         |
                    |   (Syscall Trap Host & In-Mem VFS)  |
                    +-------------------------------------+
                                       | (sDDF Shared Memory Rings)
+-------------------+                  |                  +-------------------+
|    Console PD     | <----------------+----------------> |    Monitor PD     |
|   (UART Owner)    |                                     | (Metrics & Audit) |
+-------------------+                                     +-------------------+
                                       | (sDDF Shared Memory Rings)
                    +-------------------------------------+
                    |        OS.1: System (Alpine)        |
                    |  (UIO Helper + Linux Device Drivers)|
                    +-------------------------------------+
                                       |
                    =======================================
                         OS.1: Hypervisor (seL4 MCS)
                    =======================================
```

---

## Data and Control Paths

- **Console / Serial**: UART device is owned exclusively by the `Console` protection domain. All string output and interactive stdin input from `Tyn` and `Monitor` pass through capability channels and shared ring buffers.
- **Monitoring & Audit**: Metrics, health checks, and audit logs are transmitted asynchronously over Microkit notification channels to the `Monitor` domain.
- **Networking**: Ethernet frames flow from the NIC -> Alpine driver -> `uio-helper` -> sDDF net ring -> `Tyn` -> BEAM `gen_tcp`/`gen_udp`/Bandit.
- **Storage**: Block I/O flows from SSD/Storage -> Alpine driver or native sDDF driver -> sDDF block ring -> `Tyn` -> BEAM file/Mnesia/ETS.

---

## Microkit Capabilities & Resource Partitioning

- **Protection Domains (PDs)**: Fixed at build time, single-threaded isolated components. Maximum 63 PDs per system.
- **Memory Regions**: Explicitly mapped with Read/Write/Execute permissions. No dynamic shared memory creation at runtime.
- **Channels**: Point-to-point capability channels for notifications and protected procedure calls (PPC).
- **Static Scheduling**: Fixed priorities and MCS parameters (budget, period, core affinity).
