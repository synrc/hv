# VirtIO Model for Synrc Hypervision

## Goal

Add a minimal, low-latency VirtIO frontend to `synrc/hv` for unikernels running on QEMU/KVM/Proxmox, while meeting hard real-time and seL4 capability-isolation requirements.

The implementation must preserve the existing `hv` architecture while elevating storage to a first-class, near-native path under exclusive `hv` ownership of the physical NVMe controller:

```text
├── pds/
│   ├── host/                      # everything that runs with device capabilities
│   │   ├── virtio/                # VirtIO transport + queues
│   │   │   ├── virtio.c
│   │   │   ├── virtqueue.c
│   │   │   └── virtio_mmio.c
│   │   ├── nvme/                  # poll-mode NVMe backend
│   │   │   ├── nvme.c
│   │   │   ├── nvme_queue.c
│   │   │   └── nvme_pci.c
│   │   ├── net/                   # host-side network backend (optional later)
│   │   ├── console/               # already existed; move or keep
│   │   └── monitor/               # already existed; move or keep
│   │
│   └── guest/                     # code that runs inside the BEAM address space
│       ├── synrc/                 # existing BEAM + syscall layer (moved here)
│       └── virtio/                # guest-side VirtIO frontends
│           ├── net.c
│           ├── blk.c
│           └── console.c
```

**Key insight for storage**:

VirtIO is only an ABI between guest and host; it is not the storage path itself. For local NVMe the physical path is still the CPU talking directly to the NVMe device over PCIe (the device’s own controller sits on the PCIe bus). The host/nvme layer is therefore a thin translation between two ring-based asynchronous interfaces — VirtIO descriptors on one side and NVMe submission/completion queues on the other. This is the same family of techniques used by SPDK: poll-mode drivers, lockless queues, direct PCI BAR access, zero-copy DMA, and completion-queue polling.

**Hard architectural invariant (lowest-latency path):**

Physical NVMe controllers used for the low-latency block path **must** be owned and driven exclusively by the `hv` host protection domain. No Alpine (or other Linux) guest may claim the same controller via VFIO, PCI passthrough, or kernel driver.

This invariant is required for direct BAR mapping, host-memory SQ/CQ pairs, SMMU stream-ID control of guest memory, and poll-mode completion without an extra virtualization hop.

## 1. Scope

Initial implementation:

* VirtIO 1.x MMIO transport
* split virtqueues
* guest/host shared memory under seL4 capabilities
* feature negotiation
* queue setup
* queue notification
* used/available ring processing
* VirtIO network frontend
* VirtIO block frontend (backed by host/nvme for lowest latency)
* VirtIO console frontend
* interrupt-driven and polling operation
* zero-copy data paths where the guest memory model permits
* QEMU `virt` / AArch64 as the primary target
* host-side NVMe poll-mode driver (direct PCIe BAR, submission/completion queues)
* exclusive `hv` ownership of the physical NVMe controller
* real-time friendly design (bounded WCET paths, priority & scheduling-context control)

Explicitly out of scope initially:

* VirtIO PCI
* packed virtqueues
* vhost
* SR-IOV
* generic VirtIO bus framework
* dynamic device discovery framework
* multi-process driver architecture
* unnecessary portability layers
* bounce buffers for storage
* generic host block layer or filesystem

The implementation can be extended later without making these features part of the initial ABI.

## 2. Architecture (reconsidered for real-time + seL4 capabilities)

### 2.1 Protection-domain topology

```text
┌─────────────────────────────────────────────────────────────┐
│                     seL4 / Microkit                         │
│  capabilities control every resource: memory, IRQ, PCI,     │
│  SMMU stream-ID, scheduling context, notification           │
└─────────────────────────────────────────────────────────────┘
          │
          ├── hv PD  (high priority / dedicated scheduling context)
          │     ├── exclusive PCI capability for NVMe BDF
          │     ├── exclusive IRQ / MSI-X capability for NVMe
          │     ├── exclusive SMMU stream-ID(s) for NVMe
          │     ├── VirtIO MMIO register pages (device memory)
          │     ├── shared-memory frames with guest (mapped R/W)
          │     └── poll-mode loops (or high-priority IRQ handlers)
          │
          ├── BEAM unikernel PD(s)
          │     ├── VirtIO MMIO window (mapped from hv)
          │     ├── guest physical memory frames (capability-granted)
          │     └── notification endpoints for optional interrupt path
          │
          └── (optional) Alpine / other untrusted PDs
                └── must NOT receive capabilities for the production NVMe
```

All access is mediated by seL4 capabilities. There is no ambient authority.

### 2.2 Host components

```text
host/
├── virtio/
│   ├── virtio.c          # device state and feature negotiation
│   ├── virtqueue.c       # split rings and queue operations
│   └── virtio_mmio.c     # VirtIO MMIO transport
│
└── nvme/
    ├── nvme.c            # controller init, identify, namespace
    ├── nvme_queue.c      # SQ/CQ pairs, doorbells, polling
    └── nvme_pci.c        # PCIe BAR mapping, MSI/MSI-X (optional)
```

The NVMe backend is **not** a conventional VirtIO device driver. It is a translation layer:

```text
guest/virtio/blk.c
        │ VirtIO block request
        v
host/virtio
        │ direct metadata translation
        v
host/nvme
        │ NVMe SQ + doorbell
        v
PCIe NVMe controller
```

No filesystem. No Linux block layer. No generic host block abstraction. No bounce buffers. No cross-core handoff on the fast path.

### 2.3 Real-time considerations

* **Polling is preferred for the lowest-latency, most deterministic path.** Completion-queue phase-bit checks and VirtIO used-ring polls are pure memory loads; their WCET is bounded and small.
* **Interrupt path is secondary.** When used, IRQs are delivered only to the `hv` PD via seL4 notification objects that the PD has the capability to receive. Event suppression (VirtIO) and MSI-X masking keep interrupt rates under control.
* **Priority & scheduling contexts.** The `hv` PD that owns the NVMe queues must run at a priority (and with a scheduling-context budget/period under MCS) that guarantees it can meet the I/O latency bound. BEAM unikernel PDs that issue storage requests may run at lower priority; they cannot starve the NVMe poller.
* **No unbounded work in the critical path.** Descriptor chains, request translation, and CQ processing are constant-time or linear in a fixed maximum queue depth that is configured at system description time.
* **Avoid long-running kernel operations.** seL4 MCS (or carefully configured domains) should be used so that capability deletions, large CNode operations, etc. cannot occur on the storage critical path.
* **CPU affinity.** Each VirtIO-blk queue + corresponding NVMe queue pair is pinned to a single core. The BEAM scheduler thread that uses that queue is also pinned to the same core. Cross-core communication is eliminated from the fast path.

### 2.4 Capability control of devices and DMA

* **PCI ownership.** The Microkit system description grants the NVMe BDF (and its BARs) exclusively to the `hv` PD. No other PD receives a capability to that device.
* **Interrupt ownership.** MSI-X vectors or the legacy IRQ line for the NVMe controller are bound only to notification objects reachable by `hv`.
* **SMMU / IOMMU.** On AArch64 the SMMU stream-ID(s) belonging to the NVMe controller are configured so that the device may DMA only into the guest memory frames that `hv` has explicitly mapped into the corresponding IO address space. Guest physical addresses that appear in VirtIO descriptors are translated once (or cached) into the DMA addresses the SMMU will accept.
* **Shared memory.** Guest frames that form the VirtIO rings and data buffers are granted to both the BEAM PD (as its RAM) and the `hv` PD (as shared, typically read-write for rings and write-only or read-only for data according to direction). Capability rights are the only mechanism that permits the mapping.
* **No ambient DMA.** A buggy or malicious guest cannot program the NVMe controller to touch arbitrary host memory; the SMMU enforces the capability-derived address space.

### 2.5 Responsibilities of each file

#### `virtio.c`

Cold-path device management:

* device identification
* feature negotiation
* status management
* queue discovery
* queue initialization
* device configuration
* device activation

It must not become a generic runtime abstraction.

#### `virtqueue.c`

Hot-path queue implementation:

* descriptor allocation
* descriptor chains
* available ring updates
* used ring consumption
* descriptor reclamation
* event suppression
* queue polling

Keep the implementation as close to the VirtIO ring representation as possible. Avoid function calls in per-packet/per-request paths where static inline code can provide the same operation.

#### `virtio_mmio.c`

Transport only:

* MMIO register access
* device reset
* status registers
* feature registers
* queue registers
* queue notification
* interrupt/status handling
* device configuration registers

No network, block, or console policy belongs here.

#### `nvme.c` / `nvme_queue.c` / `nvme_pci.c`

* Map NVMe controller PCI BAR (capability-granted device memory)
* Create submission/completion queue pairs in host memory
* Translate VirtIO block descriptors into NVMe commands (opcode, LBA, length, PRP/SGL pointing at guest physical memory)
* Doorbell write for submission
* Poll completion queue (phase-bit)
* Translate NVMe completion into VirtIO used descriptor
* Multi-queue affinity (one NVMe queue pair per VirtIO queue / CPU)

### 2.6 Guest components

```text
guest/
└── virtio/
    ├── net.c
    ├── blk.c
    └── console.c
```

#### `net.c`

* RX / TX queues, zero-copy where possible, polling + interrupt-assisted, batching, descriptor recycling.

#### `blk.c`

* Synchronous and asynchronous block requests, request submission, completion polling, descriptor-chain reuse, direct guest-buffer I/O. The guest side remains pure VirtIO-blk; the host side maps it directly onto NVMe.

#### `console.c`

* Minimal TX / RX, polling, optional interrupt notification. No generic stream abstraction.

## 3. Why NVMe + thin VirtIO is the lowest-latency storage path

VirtIO itself cannot give the absolute lowest latency to a host NVMe array. VirtIO is a virtual-device interface. When physical NVMe drives are attached to the host CPU’s PCIe root complex, the absolute-lowest-latency path is for the host to own the NVMe controller directly (SPDK-style) and expose storage to the unikernel through the thinnest possible mechanism.

### Ideal architecture under seL4

```text
                    ┌───────────────┐
                    │   NVMe SSD    │
                    └────────┬──────┘
                            PCIe
                             │
                    ┌────────v────────┐
                    │  host CPU / RC  │
                    └────────┬────────┘
                             │
                     NVMe PCIe BAR
                     (capability-owned by hv)
                             │
                    ┌────────v────────┐
                    │ host/nvme       │
                    │ poll-mode       │
                    │ (hv PD)         │
                    └────────┬────────┘
                             │
                    shared memory queues
                    (capability-mapped)
                             │
                    ┌────────v────────┐
                    │ guest/virtio    │
                    │ virtio-blk      │
                    │ (BEAM PD)       │
                    └────────┬────────┘
                             │
                         BEAM / unikernel
```

### Why this is extremely fast

NVMe already has almost exactly the architecture we want:

```text
CPU memory
Submission Queue
┌─────────────────────────────┐
│ command 0                   │
│ command 1                   │
│ ...                         │
└─────────────────────────────┘
          │ doorbell MMIO
          v
       NVMe SSD
          |
          v
Completion Queue
┌─────────────────────────────┐
│ completion 0                │
│ completion 1                │
│ ...                         │
└─────────────────────────────┘
```

SPDK exploits exactly this model. Therefore we must **not** insert a traditional block stack between VirtIO and NVMe.

Instead:

```text
VirtIO descriptor  ──translate metadata only──►  NVMe command
NVMe completion    ──translate───────────────►  VirtIO used descriptor
```

### Ideal READ path

```text
BEAM (guest PD)
  │  virtio_blk_read(LBA, len, guest_buf)
  v
VirtIO descriptor (guest PA of buffer) + block header
  │
  v  (shared ring, capability-mapped)
hv PD
  │  translate → NVMe command (PRP/SGL = guest PA via SMMU)
  │  store to SQ, write doorbell
  v
NVMe controller  ──DMA──►  guest buffer
  │
  v  completion in CQ
hv PD polls CQ phase bit
  │  write VirtIO used ring
  v
BEAM observes completion
```

No data copy. No Alpine hop. Bounded software path.

### DMA requirement (critical)

```text
guest VA → guest PA → SMMU / DMA mapping → NVMe DMA address
```

* Bounce buffers are rejected.
* Direct DMA into guest memory is required.
* The SMMU stream-ID mapping is established under seL4 capability control and is never ambient.

### Polling & real-time

Prefer:

```text
CPU (pinned)
 ├── submit NVMe / VirtIO
 ├── poll NVMe CQ
 ├── poll VirtIO used ring
 └── execute BEAM
```

over interrupt chains that introduce scheduling latency and priority inversion risk. When interrupts are used they are strictly capability-mediated and event-suppressed.

### CPU affinity

```text
CPU 0 ── VirtIO queue 0 ── NVMe queue 0 ── BEAM thread 0
CPU 1 ── VirtIO queue 1 ── NVMe queue 1 ── BEAM thread 1
…
```

### Conceptual distinction

* Networking: BEAM → VirtIO-net → host network backend → NIC. VirtIO is reasonable.
* Local NVMe: BEAM → VirtIO-blk → host/nvme → PCIe NVMe. VirtIO is only the ABI; the physical path is never virtualized.

### What “lowest latency” means

The design removes the software mechanisms that normally add I/O latency: system calls, general-purpose block-stack traversal, data copies, locks, scheduler transitions, and completion interrupts. This is independently supported by SPDK architecture and measurements. Absolute numbers still require measurement on the concrete CPU / PCIe / NVMe / SMMU configuration.

### Caveat specific to hv

If Alpine currently owns the physical NVMe controller, the direct path is impossible. Either:

1. move NVMe ownership from Alpine to the hv host (required for lowest latency), or
2. use Alpine as the NVMe backend (adds another virtualization/IPC layer and cannot meet the goal).

**Physical NVMe ownership by hv itself is therefore an explicit architectural requirement.**

## 4. How to proceed with the ownership requirement

### 4.1 Formal invariant

Add (and keep) the following non-negotiable statement in architecture documents:

> Physical NVMe controllers used for the low-latency block path **must** be owned and driven exclusively by the `hv` host protection domain. No Alpine (or other Linux) guest may claim the same controller via VFIO, PCI passthrough, or kernel driver.

### 4.2 Ordered steps

| Step | Action | Why |
|------|--------|-----|
| **A** | Inventory current ownership | Confirm whether Alpine (or any other domain) currently binds the NVMe controller. Document the PCI BDF(s), BAR sizes, MSI-X vectors, and existing SMMU stream IDs. |
| **B** | Decide the ownership model | Prefer: `hv` is the sole owner from boot. A brief hand-off from a minimal early-boot domain is acceptable only if Alpine never subsequently receives the capability. |
| **C** | Update the Microkit / seL4 system description | Assign the NVMe PCI device (and its interrupt(s)) exclusively to the `hv` protection domain. Remove it from any Alpine or other guest capability set. Grant the corresponding SMMU stream-ID configuration rights to `hv`. |
| **D** | Implement the host NVMe poll-mode driver | `host/nvme/{nvme.c,nvme_queue.c,nvme_pci.c}` — map BAR, create SQ/CQ pairs, identify, poll CQ phase bit. Validate with a host-only test before connecting VirtIO. |
| **E** | Establish the IOMMU / SMMU mapping policy | Map only the guest memory regions that the unikernel is allowed to expose to storage. Cache the translations; never recompute them on the I/O path. |
| **F** | Wire VirtIO-blk → NVMe translation | Guest still speaks pure VirtIO-blk; host side does the thin metadata conversion. No bounce buffers. |
| **G** | Measure | Compare (a) pure host NVMe poll path, (b) VirtIO-blk → NVMe, (c) any residual Alpine path. The gap between (a) and (b) is the pure VirtIO translation cost. |

### 4.3 Practical implications

* Phase 6 (host NVMe) becomes a hard prerequisite for the production block path.
* Functional testing of VirtIO-blk can still use a simple (higher-latency) backend on QEMU while the real NVMe driver is under development.
* The guest Device Tree continues to describe only `virtio,mmio` nodes; the physical NVMe never appears in the guest DT.
* Alpine, if still present, must be configured without the NVMe device (no `vfio-pci`, no kernel `nvme` driver binding to those BDFs).

### 4.4 Risk / decision points

1. **Hardware platform** – Does the target expose the NVMe controller so that seL4/Microkit can claim it (PCIe visibility, SMMU support, MSI-X)?
2. **Boot sequence** – Who programs the SMMU and PCI BARs before `hv` starts? Prefer that `hv` itself does it after a minimal early boot stage.
3. **Multi-queue / CPU affinity** – Plan from day one that each VirtIO-blk queue maps 1:1 to an NVMe queue pair pinned to the same core that runs the corresponding BEAM scheduler thread.
4. **Failure isolation** – Because `hv` now owns the real storage controller, a bug in the NVMe driver can take the whole machine’s storage down. Keep the driver small, audited, freestanding, and free of dynamic allocation.

### 4.5 Immediate actions

1. Record the ownership invariant in this document (already present) and any top-level architecture document.
2. Produce a short “NVMe ownership” checklist (BDF, BAR, IRQ, SMMU stream ID, Microkit capability grant).
3. Start the host NVMe poll-mode skeleton in parallel with VirtIO MMIO — the two pieces meet only at the translation layer.
4. Keep QEMU virtio-blk functional tests alive so the guest side can progress independently.

## 5. Memory Model

VirtIO descriptors contain guest physical addresses.

The hot path must not repeatedly perform expensive guest-address translation.

Establish the relationship between guest physical memory and host-accessible memory during initialization (via seL4 frame capabilities).

For contiguous guest memory:

```text
host_address = host_guest_base
             + (guest_address - guest_base)
```

where valid ranges have already been established under capability grants.

The runtime queue path should use cached mappings or direct address calculation.

Memory ownership must be explicit.

Do not hide guest memory behind opaque allocation APIs in the hot path.

For NVMe DMA the same principle applies: the SMMU mapping from guest PA → DMA address is established once (or per-buffer when necessary) under capability control and reused; it is never recomputed on every I/O.

## 6. Virtqueue Design

Initial implementation uses split virtqueues.

Each queue contains:

```text
descriptor table
available ring
used ring
```

Maintain queue state locally:

```text
queue size
free descriptor head
free descriptor count
next available index
last consumed used index
notification state
```

Keep frequently accessed state together and cache-friendly.

Avoid unnecessary pointer chasing.

Prefer direct array access over generic containers.

Queue depth and maximum chain length are fixed at system-description time so that WCET of the hot path is statically bounded.

## 7. Hot-Path Rules

### No allocation

The normal runtime path must perform:

```text
0 dynamic allocations
```

Preallocate queue descriptors, request objects, packet buffers, and completion state.

### No locks where possible

Prefer:

```text
one queue ↔ one producer/consumer
```

For SMP, prefer multiple queues over a shared locked queue. Capability isolation already prevents other PDs from touching the queue.

### No copies

Network TX/RX and block I/O should use existing guest buffers directly whenever possible.

### No dynamic dispatch

Do not use virtual tables, callback-heavy driver objects, or generic bus dispatch in the packet/request path.

### No repeated address translation

Cache guest-memory mappings and SMMU translations.

### Minimize MMIO

MMIO accesses are significantly more expensive than normal memory operations.

Only notify the device when required. NVMe doorbells are also MMIO; batch them.

### Batch operations

Allow multiple descriptors to be submitted before notification.

Provide explicit flush semantics.

### Bounded WCET

Every operation on the storage critical path must have a statically analysable upper bound (fixed queue depth, fixed maximum descriptor chain length, no loops over unbounded guest data).

## 8. Memory Ordering

Memory barriers must follow the VirtIO memory model.

For submission:

```text
write descriptor
write buffer metadata
publish available ring
notify device
```

The publication operation requires release ordering.

For completion:

```text
read used index
acquire ordering
read used elements
reclaim descriptors
```

Do not weaken memory ordering merely for benchmark gains.

The implementation must be correct on AArch64.

Use the smallest barrier required by the protocol rather than full system barriers everywhere.

NVMe queue pairs follow the same principle: store to SQ → release → doorbell write; load CQ phase bit → acquire → process completion.

## 9. Notification and Interrupt Strategy

Support both:

```text
polling
interrupt-driven operation
```

Polling should be the lowest-latency and most deterministic path.

Interrupts should avoid unnecessary wakeups through VirtIO event suppression and should be delivered only via seL4 notification objects that the receiving PD has the capability to wait on.

Notifications should be batched:

```text
submit
submit
submit
submit
    │
    ▼
single notify
```

The API must not force one MMIO notification per packet or request.

For NVMe the analogous rule is: batch SQ entries, then a single doorbell write; poll CQ rather than enable interrupts for the fast path.

## 10. Network Fast Path

Target:

```text
virtio_net_send()
    ↓
prepare descriptor
    ↓
publish descriptor
    ↓
optional batch
    ↓
notify
```

RX:

```text
poll
 ↓
read used index
 ↓
consume descriptors
 ↓
deliver existing buffer
 ↓
recycle descriptor
```

The implementation should permit a busy-poll mode for latency-sensitive workloads.

## 11. Block Fast Path (VirtIO → NVMe)

Requests should be reusable.

Conceptually:

```text
request
 ├── header
 ├── data buffer (guest PA)
 └── status
```

Submission:

```text
prepare descriptor chain
publish
notify (or batch)
host translates → NVMe command
NVMe SQ + doorbell
```

Completion:

```text
poll NVMe CQ
translate → VirtIO used ring
reclaim request
return status
```

Avoid hash tables or generic request registries for completion lookup.

The descriptor/request relationship should be directly recoverable (e.g., by embedding the request index in the NVMe command ID or a parallel array).

## 12. Console Fast Path

Keep console implementation minimal.

TX:

```text
buffer
 ↓
descriptor
 ↓
available ring
 ↓
notify
```

RX:

```text
used ring
 ↓
buffer
```

No dynamic allocation in normal operation.

## 13. Device Tree for VirtIO devices (QEMU `virt` / AArch64)

QEMU’s `virt` machine automatically generates a Device Tree Blob (DTB) that it passes to the guest. Guest code must discover devices from the DTB; only a few addresses are hard-coded (Flash at 0x0, RAM at 0x4000_0000).

### VirtIO MMIO bindings

Kernel documentation (`Documentation/devicetree/bindings/virtio/mmio.yaml` / `.txt`):

Required properties:

* `compatible = "virtio,mmio";`
* `reg` — control registers base address and size (including configuration space)
* `interrupts` — interrupt generated by the device

Optional / related:

* `dma-coherent;`
* `iommus` — when the device accesses memory through an IOMMU (SMMU)
* `#iommu-cells` — only for a virtio-iommu device itself

Example node:

```dts
virtio@a000000 {
    compatible = "virtio,mmio";
    reg = <0x0 0xa000000 0x0 0x200>;
    interrupts = <0x0 0x10 0x1>;   /* SPI, edge */
    dma-coherent;
};
```

QEMU `virt` creates up to 32 virtio-mmio transports. Their base addresses and IRQs are described in the generated DTB; the exact locations may change between QEMU versions, therefore the guest (and hv) **must** parse the DTB rather than hard-code the addresses.

Modern practice also allows a child node describing the specific VirtIO device ID:

```dts
virtio@3000 {
    compatible = "virtio,mmio";
    reg = <0x3000 0x100>;
    interrupts = <41>;
    /* optional child */
    block {
        compatible = "virtio,device2";   /* VIRTIO_ID_BLOCK */
    };
};
```

### Implications for hv

* The host side of VirtIO (the MMIO registers that the guest writes) is implemented by hv; the guest sees the classic `virtio,mmio` nodes.
* When hv also owns the physical NVMe controller, the host NVMe driver is **not** described to the guest via DT; only the VirtIO-blk frontend appears in the guest DT.
* For direct DMA the SMMU stream-ID mapping between the physical NVMe and the guest memory region must be established by hv (under seL4 capability control). This mapping is invisible to the guest DT.
* Discovery of VirtIO MMIO devices inside the BEAM unikernel (or a minimal guest runtime) is performed by walking the FDT looking for `compatible = "virtio,mmio"`.

## 14. OpenAMP Source

Use OpenAMP as the primary reference/base for the VirtIO implementation where appropriate.

OpenAMP contains dedicated VirtIO implementation files, including:

```text
lib/virtio/virtio.c
lib/virtio/virtqueue.c
lib/include/openamp/virtio_ring.h
lib/include/openamp/virtio_mmio.h
```

The existing `virtio.c` identifies itself as BSD-2-Clause and attributes Bryan Venteicher. The repository as a whole is mixed-license, however, so only individually audited files may be imported.

Do not copy the OpenAMP repository wholesale.

Before importing any file:

1. record its SPDX identifier;
2. record its copyright holders;
3. inspect its dependencies;
4. inspect the licenses of those dependencies;
5. preserve the applicable notices;
6. record the exact upstream commit.

The imported implementation should be adapted to the `hv` memory and interrupt model rather than exposing OpenAMP's API throughout `hv`.

## 15. License and Provenance

Create a provenance record for every imported source file.

Example:

```text
source: OpenAMP/open-amp
commit: <pinned commit>
file: lib/virtio/virtio.c
license: BSD-2-Clause
copyright: Bryan Venteicher
local path: host/virtio/virtio.c
modifications: Synrc Hypervision adaptation
```

Preserve source copyright and SPDX headers.

Add the applicable third-party license text to the product distribution.

Do not claim the entire VirtIO implementation is BSD-2-Clause until every imported file has been audited.

## 16. Integration with `hv` (Microkit / seL4)

Integrate VirtIO only after the standalone queue/transport implementation works.

The existing `hv` build is based on the Microkit SDK and freestanding AArch64 compilation. The VirtIO (and NVMe) code must therefore remain compatible with:

```text
-target aarch64-none-elf
-ffreestanding
-fno-builtin
-nostdlib
```

Do not introduce libc dependencies.

Do not introduce dynamic allocation.

Do not introduce Rust or another runtime dependency.

Add the VirtIO and NVMe source files to the appropriate Microkit protection domain build.

**Capability configuration (system description):**

* Grant the NVMe PCI device and its IRQ/MSI-X exclusively to the `hv` PD.
* Grant the corresponding SMMU stream-ID configuration rights to `hv`.
* Map the VirtIO MMIO register pages and the shared guest frames into both `hv` and the BEAM PD with the minimal rights required.
* Assign scheduling contexts / priorities so that the NVMe poller can meet its latency bound.

Keep the initial integration limited to QEMU AArch64. Physical NVMe ownership by hv is required for the production low-latency configuration; until that is achieved, virtio-blk can be backed by a simpler (higher-latency) host implementation for functional testing.

## 17. Initial Device

Implement VirtIO MMIO first.

Recommended first milestone:

```text
QEMU
 │
 │ VirtIO MMIO
 ▼
hv
 │
 │ virtqueue
 ▼
guest
```

Start with a minimal VirtIO device that can prove:

* MMIO discovery (via DT)
* reset
* feature negotiation
* queue setup
* descriptor submission
* used-ring completion
* notification
* interrupt/polling behavior

Then implement network, block (with NVMe backend), and console.

## 18. Implementation Phases

### Phase 1 — Audit and extraction

* [ ] Audit OpenAMP VirtIO files.
* [ ] Pin the upstream commit.
* [ ] Record licenses and copyrights.
* [ ] Determine the minimal dependency closure.
* [ ] Copy only required files.
* [ ] Preserve SPDX/copyright information.

### Phase 2 — Virtqueue core

* [ ] Define `virtio.h`.
* [ ] Implement descriptor management.
* [ ] Implement split available ring.
* [ ] Implement split used ring.
* [ ] Implement descriptor chains.
* [ ] Implement descriptor recycling.
* [ ] Implement polling.
* [ ] Implement event suppression.
* [ ] Add AArch64 memory ordering.
* [ ] Verify queue behavior independently of devices.
* [ ] Confirm WCET bounds for fixed queue depths.

### Phase 3 — MMIO transport

* [ ] Implement VirtIO MMIO register access.
* [ ] Implement device reset.
* [ ] Implement feature negotiation.
* [ ] Implement queue selection.
* [ ] Implement queue configuration.
* [ ] Implement queue notification.
* [ ] Implement interrupt status.
* [ ] Implement device status transitions.
* [ ] Parse Device Tree for `virtio,mmio` nodes.

### Phase 4 — Minimal end-to-end device

* [ ] Connect one VirtIO MMIO device in QEMU.
* [ ] Submit one descriptor.
* [ ] Receive completion.
* [ ] Verify polling.
* [ ] Verify interrupt delivery (capability-mediated).
* [ ] Verify queue reset/reinitialization.

### Phase 5 — VirtIO console

* [ ] Implement TX.
* [ ] Implement RX.
* [ ] Implement polling.
* [ ] Implement interrupt operation.
* [ ] Eliminate runtime allocations.

### Phase 6 — Host NVMe poll-mode driver + ownership

* [ ] Inventory and revoke any existing Alpine ownership of the NVMe controller.
* [ ] Update Microkit system description: exclusive PCI + IRQ + SMMU capabilities for `hv`.
* [ ] Map NVMe PCI BAR.
* [ ] Create SQ/CQ pairs.
* [ ] Implement identify / namespace discovery.
* [ ] Implement admin and I/O command submission.
* [ ] Implement CQ phase-bit polling.
* [ ] Establish SMMU mappings for guest memory under capability control.
* [ ] Multi-queue affinity and scheduling-context assignment.
* [ ] Host-only latency measurement.

### Phase 7 — VirtIO block + NVMe translation

* [ ] Implement request header.
* [ ] Translate VirtIO-blk → NVMe command (no copy).
* [ ] Translate NVMe completion → VirtIO used.
* [ ] Implement read / write (sync + async).
* [ ] Reuse request/descriptor structures.
* [ ] Benchmark latency and throughput against pure host NVMe and against any residual Alpine path.

### Phase 8 — VirtIO network

* [ ] Implement TX.
* [ ] Implement RX.
* [ ] Preallocate RX buffers.
* [ ] Implement zero-copy RX.
* [ ] Implement zero-copy TX.
* [ ] Implement batching.
* [ ] Implement polling.
* [ ] Implement interrupt operation.
* [ ] Benchmark packet latency and throughput.

### Phase 9 — Optimization & real-time validation

Measure before changing the implementation.

Optimize:

* descriptor allocation;
* ring indexing;
* cache locality;
* memory barriers;
* guest-address translation;
* SMMU mapping reuse;
* MMIO notifications / NVMe doorbells;
* interrupt frequency;
* packet/request batching;
* buffer ownership;
* queue affinity;
* WCET of the critical path under MCS / domain scheduling.

Do not optimize abstractions that do not appear in measured hot paths.

## 19. API Design

Keep the API small.

The core should expose only what device implementations need.

Conceptually:

```c
virtio_init(...)
virtio_negotiate_features(...)
virtio_setup_queue(...)
virtio_queue_submit(...)
virtio_queue_poll(...)
virtio_queue_notify(...)
```

Device APIs should remain device-specific:

```c
virtio_net_send(...)
virtio_net_poll(...)
virtio_blk_read(...)
virtio_blk_write(...)
virtio_blk_poll(...)
virtio_console_write(...)
virtio_console_poll(...)
```

Host NVMe API (internal to hv PD):

```c
nvme_init(...)
nvme_create_qpair(...)
nvme_submit(...)
nvme_poll_cq(...)
```

Do not create: `device_driver`, `bus`, `transport_object`,
`dma_object`, `generic_request`, `generic_buffer`.

Unless a measured implementation requirement demonstrates the need.

## 20. Testing

Primary target:

```text
QEMU AArch64
-machine virt
```

Tests must cover:

* Device Tree discovery of `virtio,mmio` nodes;
* device reset;
* feature negotiation;
* queue initialization;
* descriptor allocation;
* descriptor chaining;
* ring wraparound;
* queue exhaustion;
* queue recycling;
* notification;
* interrupt handling (capability-mediated);
* polling;
* network TX / RX;
* block read / write (via NVMe backend);
* console TX / RX;
* direct DMA correctness (no bounce buffers);
* multi-queue affinity;
* exclusive NVMe ownership (Alpine must not see the device);
* WCET / latency bounds under the chosen scheduling policy.

Stress tests should intentionally exhaust descriptors and exercise ring wraparound.

## 21. Performance Criteria

Measure at minimum:

### Virtqueue

* descriptor submission latency;
* descriptor completion latency;
* queue-full behavior;
* ring-wrap performance.

### Network

* packet TX latency;
* packet RX latency;
* packets/sec;
* throughput;
* CPU cycles/packet;
* interrupt rate;
* polling rate.

### Block (VirtIO-blk → NVMe)

* request submission latency;
* completion latency;
* IOPS;
* throughput;
* CPU cycles/request;
* comparison against pure host NVMe baseline (to quantify VirtIO translation overhead);
* comparison against any residual Alpine path.

### Console

* write latency;
* sustained throughput.

### Real-time

* measured WCET of the storage critical path;
* interrupt latency when the interrupt path is enabled;
* effect of scheduling-context budget/period on I/O latency.

Benchmarks must distinguish:

```text
guest → virtqueue
virtqueue → MMIO notification / NVMe doorbell
host → used ring / NVMe CQ
used ring → guest
```

so that optimization targets remain identifiable.

## 22. Definition of Done

The initial VirtIO implementation is complete when:

* [ ] VirtIO MMIO works on QEMU AArch64 and is discovered via Device Tree.
* [ ] Virtqueues operate correctly under ring wraparound.
* [ ] No runtime allocation occurs on the hot path.
* [ ] No unnecessary locks occur on the single-queue path.
* [ ] Guest memory is not repeatedly translated.
* [ ] Network RX/TX supports zero-copy operation where possible.
* [ ] Block I/O supports synchronous and asynchronous operation via thin NVMe translation.
* [ ] Direct DMA into guest memory is used (no bounce buffers).
* [ ] Physical NVMe controller is exclusively owned by the `hv` PD (capability-enforced).
* [ ] SMMU stream-ID mappings are under `hv` control.
* [ ] Console operation works without a generic driver framework.
* [ ] Polling works.
* [ ] Interrupt-driven operation works and is capability-mediated.
* [ ] AArch64 memory ordering is explicitly correct.
* [ ] Critical-path WCET is bounded and measured.
* [ ] VirtIO source provenance and licenses are recorded.
* [ ] The implementation builds with the existing freestanding `hv` toolchain.
* [ ] QEMU integration tests pass.
* [ ] Latency and throughput benchmarks are recorded (including NVMe path vs. pure host baseline).
* [ ] Real-time scheduling parameters (priority / scheduling context) for the `hv` NVMe poller are documented and validated.

## 23. Design Rule

The implementation should remain small.

Prefer:

```text
direct memory
+ direct ring access
+ preallocated state
+ explicit ownership (seL4 capabilities)
+ minimal barriers
+ minimal MMIO
+ thin metadata translation (VirtIO ↔ NVMe)
+ bounded WCET
+ poll-mode determinism
```

over:

```text
generic abstractions
+ callbacks
+ allocation
+ locking
+ repeated translation
+ bounce buffers
+ unnecessary layers
+ ambient authority
+ unbounded work on the critical path
```

The VirtIO implementation is a performance-critical boundary, not a general-purpose device framework. For storage the real performance boundary is the host NVMe poll-mode driver running inside a capability-isolated, real-time-capable protection domain; VirtIO is only the ABI that keeps the unikernel portable.
