# OS.1 OCI: A Unified Container and Unikernel Orchestration Plane

**Abstract**

Traditional cloud orchestration platforms, such as Kubernetes, impose an immense Trusted Computing Base (TCB) built upon monolithic Linux kernels and complex container runtimes. This paper introduces **synrc-kube**, a reimagined orchestration architecture where the core cluster services (Key-Value Store, Name Service, Certificate Authority, and Identity) are implemented as formally isolated Erlang/BEAM unikernels running directly on the seL4 microkernel. Furthermore, we redefine the Open Container Initiative (OCI) specification not as a mechanism for launching Linux namespaces, but as a standardized interface for spawning strictly isolated seL4 hypervisor guests.

## 1. Introduction

The proliferation of microservices has cemented Kubernetes as the industry standard for container orchestration. However, the architecture of standard Kubernetes nodes involves a deep, heavily layered stack: a host Linux kernel, `systemd`, `kubelet`, `containerd` or `crio`, `runc`, and complex virtual networking interfaces (CNI). Each layer introduces potential security vulnerabilities, unbounded latency spikes due to garbage collection and JSON parsing, and unpredictable system state.

`synrc-kube` proposes a radical departure: discarding the Linux host OS for the control plane entirely. By executing the orchestration logic as bare-metal unikernels on seL4, we achieve a system with mathematical isolation guarantees, zero dynamic allocation in hot paths (via ASN.1 DER), and a drastically reduced attack surface.

## 2. Micro-Unikernel Architecture: Distinct Protection Domains

In a standard Kubernetes master node, components like `etcd`, `kube-apiserver`, and `CoreDNS` run as separate processes on a shared Linux kernel, vulnerable to local privilege escalation exploits. `synrc-kube` decomposes these services into independent, hardware-isolated seL4 Protection Domains (PDs).

Each core service is compiled as a `synrc-beam` unikernel—an unmodified Erlang virtual machine bundled with a minimal syscall emulation layer:

1. **Key-Value Store (KVS) PD:** Replaces `etcd`. This unikernel runs Mnesia, Erlang's distributed database, providing transactional state management natively.

2. **Name Service (NS) PD:** Replaces `CoreDNS`. Dedicated to cluster service discovery, resolving names to VirtIO ring endpoints.

3. **Certificate Authority (CA) PD:** Replaces `cert-manager`. An isolated cryptographic enclave executing Erlang's `public_key` module to manage mTLS certificates. Its memory is strictly separated from the rest of the network.

4. **Identity & Directory (LDAP) PD:** Replaces `Dex` and K8s RBAC. Manages authentication for operators and inter-unikernel communication.

Communication between these distinct PDs occurs exclusively through statically defined, capability-mapped VirtIO shared memory rings using binary ASN.1 DER, ensuring deterministic, bounded-time execution.

## 3. Unified OCI Interface

A key innovation of `synrc-kube` is its treatment of the Open Container Initiative (OCI) specification. In standard environments, OCI images are unpacked and executed as Linux containers (via `cgroups` and `namespaces`). 

In the `synrc-kube` architecture, **OCI is repurposed strictly as an interface for spawning seL4 guests.**

### 3.1 Unikernels

A minimal Chimera or Alpine Linux Virtual Machine Manager (VMM) runs as a separate, unprivileged seL4 Protection Domain. Its sole responsibility is to act as the translation layer between the `synrc-kube` KVS and the hypervisor:

1. The VMM listens to a dedicated VirtIO control ring connected to the KVS unikernel.

2. When the orchestrator schedules a new workload, the VMM pulls the specified OCI image from a registry.

3. Instead of passing the image to `runc`, the VMM extracts the root filesystem (e.g., `otp-rootfs.cpio`) and instructs the seL4 hypervisor to spawn a new, fully isolated guest VM utilizing that image.

This approach preserves the universal packaging and deployment ecosystem of OCI while enforcing the strict, hardware-level isolation of a Type-1 hypervisor.

### 3.2 Containers

While strict seL4 isolation is the primary goal, `synrc-kube` recognizes the need for flexible workload environments. To this end, the VMM Shim retains the capability to launch payloads as true OS-level containers. 

When a workload profile dictates, the VMM can bypass the seL4 guest spawning routine and instead utilize standard Linux `cgroups` and `chroot` namespaces within its own Alpine/Chimera domain. This allows `synrc-kube` to seamlessly orchestrate traditional lightweight containers—such as **Alpine Linux** or **NetBSD rump** compatibility environments—directly alongside the hardware-isolated seL4 VMM guests.

## 4. Conclusion

By fracturing the monolithic orchestration control plane into distinct seL4 Protection Domains and repurposing OCI as a hypervisor spawning interface, `synrc-kube` provides a viable path to high-assurance cloud infrastructure. The inherent fault-tolerance of Erlang's OTP supervisors, combined with the formal verification of seL4, yields an orchestration platform uniquely suited for defense, aerospace, and hard real-time environments where security and determinism are paramount.
