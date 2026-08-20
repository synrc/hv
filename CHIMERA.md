# OS.1 UNICORN: Unikernel Control Plane on Chimera Linux

**Abstract**

The evolution of high-assurance, fault-tolerant distributed systems necessitates a departure from traditional monolithic orchestration platforms. We present the architectural requirements and design for the **OS.1 Control Plane**, a lightweight, formally verifiable orchestration operating system. By leveraging Chimera Linux as the management domain, Erlang/BEAM as isolated unikernels, the seL4 microkernel as a Type-1 hypervisor, and ASN.1 DER for binary control messaging, we achieve a highly deterministic, low-latency infrastructure capable of strict hardware isolation and bounded-time execution.

## 1. Introduction

Modern cloud infrastructure often relies on complex, monolithic orchestration planes (e.g., Kubernetes on general-purpose Linux distributions like Debian or Ubuntu). While these stacks provide extensive functionality, their massive Trusted Computing Base (TCB) undermines formal security guarantees, introduces unpredictable latency spikes, and complicates capability-based isolation.

OS.1 introduces a bifurcated architecture: a rigid **System Plane** for lifecycle management and resource orchestration, and a **Application Plane** composed of isolated, purpose-built unikernels. This document outlines the architectural requirements for implementing the OS.1 System Plane using **Chimera Linux**, interfacing with **seL4** and **Erlang BEAM unikernels** via **ASN.1 DER** over VirtIO.

## 2. Architectural Principles

The orchestration plane must adhere to the following strict constraints:

1. **Hardware Ownership Invariant:** The System Plane OS must never bind its own block or network drivers to the physical devices intended for the unikernels. Physical controllers (e.g., NVMe) must remain under the exclusive capability control of the seL4 `hv` host domain.

2. **Zero Dynamic Allocation in Hot Paths:** Orchestration messaging must be deterministic, utilizing pre-allocated buffers and zero-copy protocols.

3. **Minimal TCB:** The control plane OS must be stripped of unnecessary services, utilizing a minimal libc and strict capability bounding.

## 3. Chimera Linux as the Control Plane OS

Proxmox VE traditionally utilizes Debian as its underlying orchestration OS (Dom0/Control Plane). For OS.1, **Chimera Linux** replaces Debian. Chimera is a FreeBSD-userland, musl-libc based Linux distribution that provides a drastically reduced attack surface compared to GNU/Linux systems.

### 3.1 Requirements for Chimera Integration

* **Musl-libc Foundation:** Chimera's musl-libc ensures a lightweight footprint and strict POSIX compliance without the bloat of glibc.

* **Driver Blacklisting:** Chimera must be configured to unconditionally ignore specific PCI BDFs (Bus/Device/Function) corresponding to production NVMe and NIC devices, ensuring they are passed through to the seL4 hypervisor unmodified.

* **Virtual Machine Manager (VMM) Interface:** Chimera will host the user-facing API and CLI tools, acting as the frontend for lifecycle commands (start, stop, migrate) directed at the underlying seL4 microkit system.

## 4. Hypervision: Erlang Unikernels on seL4

The payload of the OS.1 architecture consists of unmodified Ericsson BEAM virtual machines compiled as static unikernels. 

### 4.1 Execution Model

Rather than running a full guest OS for each Erlang node, the BEAM VM is compiled alongside a minimal syscall emulation layer (`synrc`). This unikernel runs directly as an seL4 Protection Domain (PD). 

* **Scheduling Contexts:** Each BEAM scheduler thread is pinned to a dedicated core alongside its corresponding VirtIO and NVMe polling queues.

* **Fault Isolation:** Erlang's actor-model fault tolerance is reinforced at the hardware level by seL4's capability-based isolation. If a BEAM unikernel crashes, the seL4 microkernel traps the fault and notifies the Chimera System Plane to trigger a restart.

## 5. Control Protocol: ASN.1 DER

Traditional orchestration systems rely heavily on JSON over HTTP (e.g., the Kubernetes API) or gRPC. These protocols are fundamentally incompatible with hard real-time systems due to string parsing overhead, unbound payload sizes, and dynamic memory allocation (garbage collection).

### 5.1 ASN.1 DER Specification

OS.1 mandates **ASN.1 DER (Distinguished Encoding Rules)** for all inter-domain control messages between the Chimera System Plane and the seL4/BEAM components.

* **Determinism:** DER guarantees a single, unique binary encoding for any given data structure.

* **Bounded Execution:** DER can be parsed in-place using statically allocated buffers. The Worst-Case Execution Time (WCET) for parsing any valid control message is mathematically bounded.

* **Erlang Synergy:** The BEAM VM features highly optimized, native ASN.1 compilation, allowing Erlang unikernels to decode orchestration messages instantly without relying on heavy manual parsing either in C99 or Erlang.

### 5.2 VirtIO Control Channel

Control messages are dispatched from Chimera to the seL4 host domain via a dedicated VirtIO console/control ring. The shared-memory rings are capability-mapped, allowing zero-copy transmission of ASN.1 DER payloads directly from the control plane to the hypervisor management domain.

## 6. Boot Process

The initialization of the OS.1 stack follows a deterministic, statically defined sequence, ensuring that the Trusted Computing Base (TCB) remains minimal and isolated from the moment of power-on:

1. **Hardware Initialization:** The platform bootloaderLimine loads the seL4 microkernel and the statically linked Microkit loader image into memory.

2. **Microkernel Bootstrap:** seL4 initializes the CPU, configures the SMMU (System MMU) to isolate DMA domains, and applies the immutable capability descriptions provided by the Microkit system definition (`hv.system`). 

3. **Control Plane Boot:** The Chimera Linux VM is launched as the privileged orchestration and driver domain. It initializes its minimal musl-libc environment and userland, specifically ignoring the isolated hardware (e.g., the NVMe controllers) reserved for the data plane.

4. **Data Plane Unikernel Boot:** The Erlang/BEAM unikernels are instantiated in their isolated Protection Domains.

5. **Control Channel Establishment:** Chimera Linux and the BEAM unikernels map their shared VirtIO rings utilizing seL4 IPC capabilities. The ASN.1 DER control channel is established, allowing the Chimera VMM to assume lifecycle management of the running BEAM instances.

## 7. Conclusion

By synthesizing Chimera Linux for management, seL4 for strict capability-based isolation, Erlang BEAM for fault-tolerant workloads, and ASN.1 DER for deterministic communication, the OS.1 System Plane achieves a profound reduction in TCB. This architecture provides a viable, high-assurance alternative to legacy monolithic orchestration platforms, paving the way for formally verifiable cloud infrastructure.
