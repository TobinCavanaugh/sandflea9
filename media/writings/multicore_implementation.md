# Symmetric Multiprocessing (SMP) Implementation Plan

This document outlines the roadmap for introducing Symmetric Multiprocessing (multicore support) to sandfleaOS.

## 1. Core Scheduling Strategies

We have three primary models for distributing work across multiple CPU cores.

### Approach A: Strict Process Affinity (Core-per-Process)
In this model, when a process is created, it is assigned to a specific core. All threads belonging to that process run on that core only.

*   **Upsides:**
    *   **Cache Locality:** Excellent performance as process data stays in the core's L1/L2 caches.
    *   **No Address Space Switches:** The core rarely needs to reload CR3, minimizing TLB flushes.
    *   **Simpler Isolation:** Easy to reason about process boundaries.
*   **Downsides:**
    *   **Core Underutilization:** If a process is idle, its core sits idle while other cores might be overloaded.
    *   **Limited Parallelism:** A single process cannot utilize more than one core, even if it has many threads.

### Approach B: Global Runqueue (Shared Tasks)
A single, global linked list of READY tasks is shared by all cores. When a core's current task finishes or yields, it grabs the next available task from the head of the list.

*   **Upsides:**
    *   **Perfect Load Balancing:** All cores stay busy as long as there is work to do.
    *   **Maximum Thread Parallelism:** A single multi-threaded process (like a WASM module) can run across all cores simultaneously.
*   **Downsides:**
    *   **Lock Contention:** All cores must acquire a global spinlock to access the runqueue, creating a bottleneck as core counts increase.
    *   **Poor Cache Locality:** A thread might run on Core 0, yield, and then resume on Core 3, forcing a massive cache miss.

### Approach C: Per-Core Runqueues with Load Balancing
Each core has its own private runqueue. A "Balancer" thread occasionally moves tasks from overloaded cores to underloaded ones.

*   **Upsides:**
    *   **Scalability:** No global lock bottleneck.
    *   **High Cache Locality:** Threads stay on their "home" core by default.
*   **Downsides:**
    *   **High Complexity:** Requires sophisticated algorithms to prevent "starvation" and handle thread migration safely.

---

## 2. Implementation Roadmap

To support any of the above, sandfleaOS needs several architectural upgrades.

### Phase 1: ACPI and MADT Parsing
Currently, we use hardcoded addresses for the IOAPIC. We must parse the ACPI tables (specifically the **MADT - Multiple APIC Description Table**) to:
1.  Discover how many CPUs are actually present.
2.  Get the Local APIC ID for each core.
3.  Get the physical address of the IOAPIC.

### Phase 2: The AP Trampoline
The Bootstrap Processor (BSP) is the only core running at boot. To wake up the Application Processors (APs), we must:
1.  Write a small piece of 16-bit real-mode assembly (the **Trampoline**).
2.  Copy it to a low-memory address (e.g., `0x8000`).
3.  Send a "Startup IPI" (Inter-Processor Interrupt) via the Local APIC to the target cores.
4.  The APs start in 16-bit mode, transition to 32-bit protected mode, then 64-bit long mode, and finally jump into a kernel C entry point (`ap_kernel_entry`).

### Phase 3: Spinlocks and Atomicity
The current kernel uses `save_irq_and_disable()` for synchronization. This **will not work** on multicore systems, as disabling interrupts on Core 0 does not stop Core 1 from accessing the same memory.
-   We must implement **Spinlocks** using atomic instructions (e.g., `lock bts` or `xchg`).
-   Critical sections (scheduler, heap, VFS) must be protected by these locks.

### Phase 4: Inter-Processor Interrupts (IPIs)
Cores need a way to talk to each other.
-   **TLB Shootdown:** When Core 0 unmaps a page, it must send an IPI to all other cores to force them to invalidate their translation lookaside buffers.
-   **Scheduler Preemption:** When a high-priority task becomes READY, the BSP might need to interrupt an AP to force it to reschedule.

## Recommendation for sandfleaOS

I recommend starting with **Approach B (Global Runqueue)** for simplicity, but with a **Process-Preferred Core** optimization. 

1.  Tasks are stored in a global list.
2.  When an AP looks for work, it first looks for a task that "belongs" to its assigned PID (if any).
3.  If none are found, it takes the first available task.

This provides a balance between ease of implementation and basic performance optimization.
