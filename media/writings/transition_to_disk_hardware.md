# Transitioning to Modern Disk Hardware and Async I/O

This document outlines the architectural shift required to move sandfleaOS from its current synchronous IDE implementation to a modern, asynchronous AHCI (SATA) driver capable of running on real UEFI-only hardware.

## 1. The Hardware Gap: IDE vs. AHCI

The current `kern_ide.c` uses Port I/O and polling to communicate with the disk. This works perfectly in QEMU but will fail on any laptop manufactured in the last 15+ years.

| Feature | Legacy IDE (Current) | AHCI / SATA (Target) |
| :--- | :--- | :--- |
| **Interface** | Port I/O (`inb`/`outb`) | Memory Mapped I/O (MMIO) |
| **Data Transfer** | Programmed I/O (CPU moves bytes) | Direct Memory Access (DMA) |
| **Concurrency** | Single command at a time (Blocking) | 32 Command Slots (Asynchronous) |
| **Real Hardware** | Disappeared ~2010 | Standard on M.2/SATA SSDs |

## 2. Asynchronous I/O Architecture

To support the 32 command slots of AHCI and provide a modern `io_uring`-like interface for WASM modules, we will move to a **Request-Response** model for the VFS.

### The Request Structure
Every I/O operation becomes a persistent object in kernel memory:

```c
typedef struct io_request {
    i32 fd;
    u64 lba_start;      // Physical disk location
    u32 sector_count;
    void* buffer;       // Target RAM
    bool is_write;
    
    // Status
    volatile u8 status; // 0: Pending, 1: Active, 2: Completed, 3: Error
    
    struct io_request* next;
} io_request_t;
```

### The Async Workflow
1.  **Submission:** The FS Layer adds a request to the `storage_queue`.
2.  **Dispatch:** The AHCI driver finds an empty slot (0-31) in the hardware and assigns the request to it.
3.  **Execution:** The CPU continues running other threads (or WASM modules). The disk controller moves data directly into the provided `buffer` via DMA.
4.  **Completion:** The disk controller triggers an **Interrupt**. The ISR identifies which command slot finished, marks the `io_request_t` as `Completed`, and wakes up any threads waiting on that specific job.

## 3. Implementation Roadmap

### Phase 1: PCI & MMIO Setup
- Use existing `pci_init_system` to find the SATA Controller (Class 01, Subclass 06).
- Enable "Bus Mastering" in the PCI Command register.
- Map the Controller's BAR 5 (the AHCI Base Address) into the kernel's virtual memory.

### Phase 2: The Command List
- AHCI requires a "Command List" and "Command Tables" to be stored in physical memory.
- We will use `pmm_alloc_page()` to create these tables.
- These tables tell the drive: "Read 10 sectors from X and put them in RAM address Y."

### Phase 3: The Interrupt Handler
- Register a new ISR for the SATA controller's IRQ.
- This handler will be responsible for calling the callbacks or unblocking threads when a disk operation finishes.

## 4. Why this matters for WASM
By making the I/O non-blocking at the kernel level, we can run a "WASM Executor" that never pauses. While the disk is slowly fetching data for one WASM module, the CPU can be fully utilized running the game logic or UI code of another module.

## 5. Persistence and PERSIST (Persistent Storage)
Once AHCI is working, we will flip the `ext2_read_only` flag to `false`. 
- **Persistence:** Because we will no longer use `-snapshot` in QEMU, every `write()` call from your WASM code will permanently modify the `disk.img` file.
- **Safety:** We will implement a "Write Barrier" in the AHCI driver to ensure metadata (bitmaps) are written before data blocks, preventing filesystem corruption on crash.
