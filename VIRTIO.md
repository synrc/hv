# VirtIO for Synrc Hypervision

## Goal

Add a minimal, low-latency VirtIO frontend to `synrc/hv` for unikernels running on QEMU/KVM/Proxmox.

The implementation must preserve the existing `hv` architecture while elevating storage to a first-class, near-native path:

```text
seL4
  │
  └── host
       ├── VirtIO transport (MMIO)
       │        │
       │        └── guest memory / queues
       │             │
       │             └── BEAM unikernel
       │                  └── VirtIO devices (net / blk / console)
       │
       └── NVMe poll-mode backend (storage only)
                │
                └── PCIe → physical NVMe
```

The design is intentionally small:

```text
host/
├── virtio/
│   ├── virtio.c
│   ├── virtqueue.c
│   └── virtio_mmio.c
│
└── nvme/
    ├── nvme.c
    ├── nvme_queue.c
    └── nvme_pci.c

guest/
└── virtio/
    ├── net.c
    ├── blk.c
    └── console.c
```

Do not introduce a generic device framework, bus framework, DMA abstraction, or driver object hierarchy.

**Key insight for storage:** VirtIO is an ABI between guest and host, not the underlying storage optimization. For local NVMe the physical path remains CPU → PCIe root complex → NVMe controller. The host/nvme layer is a thin translation between two ring-based asynchronous interfaces (VirtIO descriptor ↔ NVMe command). This is the family of techniques proven by SPDK: userspace/poll-mode drivers, lockless queues, direct PCI BAR access, zero-copy DMA, and completion-queue polling.

## 1. Scope

Initial implementation:

* VirtIO 1.x MMIO transport
* split virtqueues
* guest/host shared memory
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

## 2. Architecture

### Host

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
        │
        │ VirtIO block request
        ▼
host/virtio
        │
        │ direct metadata translation
        ▼
host/nvme
        │
        │ NVMe SQ + doorbell
        ▼
PCIe NVMe controller
```

No filesystem. No Linux block layer. No generic host block abstraction. No bounce buffers. No cross-core handoff on the fast path.

### Responsibilities

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

Keep the implementation as close to the VirtIO ring representation as possible.

Avoid function calls in per-packet/per-request paths where static inline code can provide the same operation.

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

* Map NVMe controller PCI BAR
* Create submission/completion queue pairs in host memory
* Translate VirtIO block descriptors into NVMe commands (opcode, LBA, length, PRP/SGL pointing at guest physical memory)
* Doorbell write for submission
* Poll completion queue (phase-bit)
* Translate NVMe completion into VirtIO used descriptor
* Multi-queue affinity (one NVMe queue pair per VirtIO queue / CPU)

### Guest

```text
guest/
└── virtio/
    ├── net.c
    ├── blk.c
    └── console.c
```

#### `net.c`

Provide:

* RX queue
* TX queue
* zero-copy RX
* zero-copy TX where possible
* polling
* interrupt-assisted operation
* batching
* descriptor recycling

Target path:

```text
application
    │
    ▼
virtio_net_send()
    │
    ▼
descriptor
    │
    ▼
available ring
    │
    ▼
MMIO notify
```

RX:

```text
used ring
    │
    ▼
virtio_net_poll()
    │
    ▼
guest buffer
    │
    ▼
network stack
```

Avoid copying packet payloads.

#### `blk.c`

Provide:

* synchronous block requests
* asynchronous block requests
* request submission
* completion polling
* descriptor-chain reuse
* direct guest-buffer I/O where possible

Avoid allocating request metadata for every operation.

The guest side remains pure VirtIO-blk; the host side maps it directly onto NVMe.

#### `console.c`

Provide a minimal VirtIO console path:

* TX
* RX
* polling
* optional interrupt notification

Do not introduce a generic stream abstraction.

## 3. Why NVMe + thin VirtIO is the lowest-latency storage path

VirtIO itself cannot give the absolute lowest latency to a host NVMe array. VirtIO is a virtual-device interface. When physical NVMe drives are attached to the host CPU’s PCIe root complex, the absolute-lowest-latency path is for the host to own the NVMe controller directly (SPDK-style) and expose storage to the unikernel through the thinnest possible mechanism.

### Ideal architecture

```text
                    ┌───────────────┐
                    │   NVMe SSD    │
                    └───────┬───────┘
                            PCIe
                             │
                    ┌────────▼────────┐
                    │  host CPU / RC  │
                    └────────┬────────┘
                             │
                     NVMe PCIe BAR
                             │
                    ┌────────▼────────┐
                    │ host/nvme       │
                    │ poll-mode       │
                    └────────┬────────┘
                             │
                    shared memory queues
                             │
                    ┌────────▼────────┐
                    │ guest/virtio    │
                    │ virtio-blk      │
                    └────────┬────────┘
                             │
                         BEAM / unikernel
```

Even better when hv owns the PCIe device:

```text
BEAM
 │
 │ virtio-blk
 ▼
guest virtqueue
 │
 │ shared memory
 ▼
host NVMe backend
 │
 │ NVMe SQ
 ▼
PCIe
 │
 ▼
NVMe
 │
 │ completion
 ▼
host NVMe CQ
 │
 ▼
guest used ring
 │
 ▼
BEAM
```

The goal is to make the VirtIO layer a very thin translation between two ring-based asynchronous interfaces.

### Why this is extremely fast

NVMe already has almost exactly the architecture we want:

```text
CPU memory
Submission Queue
┌─────────────────────────────┐
│ command 0                   │
│ command 1                   │
│ command 2                   │
│ ...                         │
└─────────────────────────────┘
          │
          │ doorbell MMIO
          ▼
       NVMe SSD
          │
          ▼
Completion Queue
┌─────────────────────────────┐
│ completion 0                │
│ completion 1                │
│ completion 2                │
│ ...                         │
└─────────────────────────────┘
```

This is precisely the model SPDK exploits: direct PCI BAR control, asynchronous queue pairs, interrupt avoidance via polling, and zero-copy I/O.

Therefore we must **not** insert a traditional block stack between VirtIO and NVMe.

Instead:

```text
VirtIO descriptor
        │
        │ translate metadata only
        ▼
NVMe command
```

and:

```text
NVMe completion
        │
        │ translate
        ▼
VirtIO used descriptor
```

### The two rings almost directly correspond

```text
              VirtIO                  NVMe
guest         descriptor ───────────► NVMe command
              │                        │
              │                        ▼
              │                      PCIe
              │                        │
              │                        ▼
              │                       SSD
              │                        │
              │                        ▼
              │                    completion
              │                        │
              ◄────────────────────────┘
              used descriptor
```

The backend must not copy the data; it translates metadata.

### Ideal READ path

BEAM issues:

```text
READ LBA=100000, length=4096
```

Guest constructs:

```text
VirtIO descriptor
    addr = guest_buffer
    len  = 4096
    flags = WRITE   (device writes into guest)

VirtIO block request header
    type = IN
    sector = 100000
```

Host receives the descriptor and does:

```text
VirtIO descriptor
       │
       ▼
host NVMe backend
       │
       ├── LBA = 100000
       ├── length = 4096
       └── buffer = guest physical memory
                    │
                    ▼
              NVMe command
                    │
                    ▼
                  SSD
```

If the platform’s DMA/IOMMU (SMMU) maps the NVMe controller to the guest physical memory, the controller DMA-writes **directly** into the guest buffer. Then:

```text
SSD
 ↓ DMA
guest buffer
 ↓
NVMe completion
 ↓
host virtio used ring
 ↓
guest
```

There is no data copy.

### DMA requirement (critical)

The NVMe device understands only DMA addresses / I/O virtual addresses, never guest virtual addresses.

Required mapping:

```text
guest VA
   ↓
guest PA
   ↓
IOMMU / SMMU / DMA mapping
   ↓
NVMe DMA address
```

Two possible architectures:

* **A. Bounce-buffer** — NVMe → host buffer → memcpy → guest buffer. Easy, unacceptable for latency goal. Rejected.
* **B. Direct DMA into guest memory** — NVMe DMA → guest physical memory. Required. The IOMMU/SMMU must map the NVMe controller exactly to the guest memory it is permitted to access. This is why an IOMMU-capable design is mandatory for a seL4 system that aims at lowest latency.

### Polling is the second major optimization

Avoid:

```text
NVMe → PCIe interrupt → host IRQ handler → VirtIO interrupt → guest IRQ → BEAM scheduler
```

Prefer:

```text
CPU
 │
 ├── submit NVMe
 ├── submit VirtIO
 ├── poll NVMe CQ
 ├── poll VirtIO used ring
 └── execute BEAM
```

SPDK notes that polling completion queues is cheap because the CQ lives in host memory and does not require an MMIO read for every completion.

### CPU affinity

Map:

```text
CPU N
 │
 ├── BEAM execution
 ├── VirtIO queue N
 └── NVMe queue pair N
```

rather than crossing cores. This matches NVMe’s multi-queue architecture and SPDK’s deliberate avoidance of cross-thread coordination.

Ideal topology:

```text
CPU 0 ── VirtIO queue 0 ── NVMe queue 0
CPU 1 ── VirtIO queue 1 ── NVMe queue 1
CPU 2 ── VirtIO queue 2 ── NVMe queue 2
```

### Conceptual distinction

* **Networking:** BEAM → VirtIO-net → host network backend → NIC. VirtIO is reasonable.
* **Local NVMe:** BEAM → VirtIO-blk → host/nvme → PCIe NVMe. VirtIO is only the ABI; the physical path is never virtualized.

### What “lowest latency” means

We do not claim mathematical absolute lowest latency without measuring the exact CPU, PCIe topology, NVMe controller, IOMMU configuration and workload. We do claim that the design removes the software mechanisms that normally add I/O latency: system calls, general-purpose block-stack traversal, data copies, locks, scheduler transitions, and completion interrupts. This is independently supported by SPDK architecture and measurements.

### Optimal I/O path (target)

```text
                    CPU
                     │
                 BEAM code
                     │
              virtio_blk_read()
                     │
                     ▼
              VirtIO descriptor
                     │
                     │ no copy
                     ▼
               NVMe command
                     │
                     │ one doorbell
                     ▼
                 PCIe fabric
                     │
                     ▼
                 NVMe SSD
                     │
                     │ DMA
                     ▼
              guest memory
                     │
                     ▼
             NVMe completion
                     │
                     ▼
              VirtIO used ring
                     │
                     ▼
                    BEAM
```

CPU view:

* submission: stores → ring → MMIO doorbell
* completion: load CQ phase bit → stores used ring

### Caveat specific to hv

If the current `hv` README describes an Alpine Linux guest that owns the physical NVMe controller, the direct path above is impossible. Either:

1. move NVMe ownership from Alpine to the hv host, or
2. use Alpine as the NVMe backend (adds another virtualization/IPC layer and cannot be absolute-lowest-latency).

If lowest possible local-NVMe latency is a primary goal, physical NVMe ownership by hv itself must be an explicit architectural requirement.

## 4. Memory Model

VirtIO descriptors contain guest physical addresses.

The hot path must not repeatedly perform expensive guest-address translation.

Establish the relationship between guest physical memory and host-accessible memory during initialization.

For contiguous guest memory:

```text
host_address = host_guest_base
             + (guest_address - guest_base)
```

where valid ranges have already been established.

The runtime queue path should use cached mappings or direct address calculation.

Memory ownership must be explicit.

Do not hide guest memory behind opaque allocation APIs in the hot path.

For NVMe DMA the same principle applies: the IOMMU mapping from guest PA → DMA address is established once (or per-buffer when necessary) and reused; it is never recomputed on every I/O.

## 5. Virtqueue Design

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

## 6. Hot-Path Rules

The following are design requirements.

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

For SMP, prefer multiple queues over a shared locked queue.

### No copies

Network TX/RX and block I/O should use existing guest buffers directly whenever possible.

### No dynamic dispatch

Do not use virtual tables, callback-heavy driver objects, or generic bus dispatch in the packet/request path.

### No repeated address translation

Cache guest-memory mappings and IOMMU translations.

### Minimize MMIO

MMIO accesses are significantly more expensive than normal memory operations.

Only notify the device when required. NVMe doorbells are also MMIO; batch them.

### Batch operations

Allow multiple descriptors to be submitted before notification.

Provide explicit flush semantics.

## 7. Memory Ordering

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

## 8. Notification and Interrupt Strategy

Support both:

```text
polling
interrupt-driven operation
```

Polling should be the lowest-latency path.

Interrupts should avoid unnecessary wakeups through VirtIO event suppression.

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

## 9. Network Fast Path

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

## 10. Block Fast Path (VirtIO → NVMe)

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

## 11. Console Fast Path

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

## 12. Device Tree for VirtIO devices (QEMU `virt` / AArch64)

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

Example node (generated by QEMU or written by hand):

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
* For direct DMA the SMMU stream-ID mapping between the physical NVMe and the guest memory region must be established by hv (or by a trusted SMMU configuration). This mapping is invisible to the guest DT.
* Discovery of VirtIO MMIO devices inside the BEAM unikernel (or a minimal guest runtime) is performed by walking the FDT looking for `compatible = "virtio,mmio"`.

## 13. OpenAMP Source

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

## 14. License and Provenance

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

## 15. Integration with `hv`

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

Keep the initial integration limited to QEMU AArch64. Physical NVMe ownership by hv is required for the lowest-latency block path; until that is achieved, virtio-blk can be backed by a simpler (higher-latency) host implementation for functional testing.

## 16. Initial Device

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

## 17. Implementation Phases

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
* [ ] Verify interrupt delivery.
* [ ] Verify queue reset/reinitialization.

### Phase 5 — VirtIO console

* [ ] Implement TX.
* [ ] Implement RX.
* [ ] Implement polling.
* [ ] Implement interrupt operation.
* [ ] Eliminate runtime allocations.

### Phase 6 — Host NVMe poll-mode driver

* [ ] Map NVMe PCI BAR.
* [ ] Create SQ/CQ pairs.
* [ ] Implement identify / namespace discovery.
* [ ] Implement admin and I/O command submission.
* [ ] Implement CQ phase-bit polling.
* [ ] Establish IOMMU mappings for guest memory.
* [ ] Multi-queue affinity.

### Phase 7 — VirtIO block + NVMe translation

* [ ] Implement request header.
* [ ] Translate VirtIO-blk → NVMe command (no copy).
* [ ] Translate NVMe completion → VirtIO used.
* [ ] Implement read / write (sync + async).
* [ ] Reuse request/descriptor structures.
* [ ] Benchmark latency and throughput against SPDK baseline.

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

### Phase 9 — Optimization

Measure before changing the implementation.

Optimize:

* descriptor allocation;
* ring indexing;
* cache locality;
* memory barriers;
* guest-address translation;
* IOMMU mapping reuse;
* MMIO notifications / NVMe doorbells;
* interrupt frequency;
* packet/request batching;
* buffer ownership;
* queue affinity.

Do not optimize abstractions that do not appear in measured hot paths.

## 18. API Design

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

Host NVMe API (internal):

```c
nvme_init(...)
nvme_create_qpair(...)
nvme_submit(...)
nvme_poll_cq(...)
```

Do not create:

```text
device_driver
bus
transport_object
dma_object
generic_request
generic_buffer
```

unless a measured implementation requirement demonstrates the need.

## 19. Testing

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
* interrupt handling;
* polling;
* network TX;
* network RX;
* block read (via NVMe backend);
* block write (via NVMe backend);
* console TX/RX;
* direct DMA correctness (no bounce buffers);
* multi-queue affinity.

Stress tests should intentionally exhaust descriptors and exercise ring wraparound.

## 20. Performance Criteria

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
* comparison against pure SPDK NVMe baseline (to quantify VirtIO translation overhead).

### Console

* write latency;
* sustained throughput.

Benchmarks must distinguish:

```text
guest → virtqueue
virtqueue → MMIO notification / NVMe doorbell
QEMU/host → used ring / NVMe CQ
used ring → guest
```

so that optimization targets remain identifiable.

## 21. Definition of Done

The initial VirtIO implementation is complete when:

* [ ] VirtIO MMIO works on QEMU AArch64 and is discovered via Device Tree.
* [ ] Virtqueues operate correctly under ring wraparound.
* [ ] No runtime allocation occurs on the hot path.
* [ ] No unnecessary locks occur on the single-queue path.
* [ ] Guest memory is not repeatedly translated.
* [ ] Network RX/TX supports zero-copy operation where possible.
* [ ] Block I/O supports synchronous and asynchronous operation via thin NVMe translation.
* [ ] Direct DMA into guest memory is used (no bounce buffers).
* [ ] Console operation works without a generic driver framework.
* [ ] Polling works.
* [ ] Interrupt-driven operation works.
* [ ] AArch64 memory ordering is explicitly correct.
* [ ] VirtIO source provenance and licenses are recorded.
* [ ] The implementation builds with the existing freestanding `hv` toolchain.
* [ ] QEMU integration tests pass.
* [ ] Latency and throughput benchmarks are recorded (including NVMe path vs. SPDK baseline).
* [ ] Physical NVMe ownership by hv is established for the production low-latency configuration.

## 22. Design Rule

The implementation should remain small.

Prefer:

```text
direct memory
+ direct ring access
+ preallocated state
+ explicit ownership
+ minimal barriers
+ minimal MMIO
+ thin metadata translation (VirtIO ↔ NVMe)
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
```

The VirtIO implementation is a performance-critical boundary, not a general-purpose device framework. For storage the real performance boundary is the host NVMe poll-mode driver; VirtIO is only the ABI that keeps the unikernel portable.
