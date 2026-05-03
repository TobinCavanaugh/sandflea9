# Inter-Process Communication (IPC) in sandfleaOS

This document explores various methods of Inter-Process Communication (IPC) suitable for sandfleaOS, analyzing their strengths, weaknesses, and implementation strategies within our kernel architecture.

## Overview

In sandfleaOS, processes are isolated from each other using x86_64 paging (specifically the PML4/CR3 register). This isolation is critical for system stability and security but necessitates explicit mechanisms for processes to share data or synchronize actions.

---

## 1. Shared Memory

Shared memory allows multiple processes to access the same physical memory regions.

### Implementation
In sandfleaOS, this can be implemented by using `vmm_map_page_in_pml4` to map a common set of physical pages (allocated via `pmm_alloc_page`) into the virtual address space of two or more processes.

### Upsides
- **Highest Performance:** No data copying is required. Once mapped, communication happens at memory speeds.
- **Large Payloads:** Suitable for sharing massive datasets (e.g., framebuffers, large databases).

### Downsides
- **Complexity:** Requires explicit synchronization (e.g., spinlocks, semaphores) to prevent race conditions.
- **Security Risks:** Buggy or malicious processes can corrupt shared state.

---

## 2. Message Passing (Asynchronous)

Asynchronous message passing involves a kernel-managed queue where processes can post "messages."

### Implementation
The kernel maintains a linked list or circular buffer of messages. 
- `sys_send(target_pid, buffer, size)`: Kernel copies data from the sender's memory into a kernel-allocated buffer.
- `sys_recv(buffer, size)`: Kernel copies data from its buffer into the receiver's memory.

### Upsides
- **Decoupling:** Senders can continue working without waiting for the receiver to process the message.
- **Safety:** Processes remain isolated; data is copied rather than shared directly.

### Downsides
- **Overhead:** Data must be copied twice (User A -> Kernel -> User B).
- **Memory Pressure:** The kernel must manage buffers, which can consume significant memory if queues grow large.

---

## 3. Message Passing (Synchronous / Rendezvous)

In synchronous messaging, the sender blocks until the receiver is ready to receive, or vice versa.

### Implementation
This relies on the `TASK_STATE_BLOCKED` state in `kern_sched.h`.
- When Process A sends to B, it is marked `BLOCKED` and added to a "waiting to send" list for B.
- When Process B calls receive, the kernel identifies A, copies the data directly from A's virtual memory to B's virtual memory (using the HHDM to access A's pages), and unblocks A.

### Upsides
- **Minimal Buffering:** No intermediate kernel buffer is needed.
- **Deterministic:** The sender knows exactly when the receiver has the data.

### Downsides
- **Tight Coupling:** If one process hangs, the other may block indefinitely.
- **Complexity:** Harder to implement safely than buffered queues.

---

## 4. Signals and Events

Signals are lightweight notifications sent to a process to indicate a specific event occurred.

### Implementation
Each `kern_task_t` or `kern_process_t` could have a bitmask of pending signals. When the scheduler switches to a task, it checks the mask and, if a signal is pending, redirects execution to a signal handler.

### Upsides
- **Extremely Fast:** Minimal overhead for small notifications.
- **Interruptive:** Can wake up a blocked process immediately.

### Downsides
- **No Payload:** Standard signals carry very little information (usually just the signal ID).
- **Asynchronous Complexity:** Writing signal-safe code is notoriously difficult for userspace developers.

---

## 5. Pipes

Pipes provide a unidirectional byte stream, typically following a First-In-First-Out (FIFO) model.

### Implementation
Pipes can be integrated into the VFS (`kern_fs.h`). A pipe is a kernel-side circular buffer with two file descriptors: one for reading and one for writing.
- `write` blocks if the buffer is full.
- `read` blocks if the buffer is empty.

### Upsides
- **Familiar Abstraction:** Uses standard `read`/`write` syscalls.
- **Streaming:** Excellent for processing data in stages (e.g., `grep | sort`).

### Downsides
- **Unidirectional:** Requires two pipes for full-duplex communication.
- **Copying:** Similar overhead to asynchronous message passing.

---

## Recommendation for sandfleaOS

For the initial IPC implementation in sandfleaOS, I recommend starting with **Shared Memory** for high-performance needs and **Asynchronous Message Queues** for general-purpose communication. 

1. **Shared Memory** leverages our existing VMM code efficiently.
2. **Message Queues** provide a safer, easier-to-use API for system services (like a window manager or filesystem server).

Subsequent development should focus on integrating **Pipes** into the VFS to support standard shell-like functionality.
