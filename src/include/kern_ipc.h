// kern_ipc.h — Inter-process communication: N:N shared memory + signals.
//
// Shared memory: global reference-counted regions (shm_id) attached into
// per-process handle tables. An arbitrary number of processes can attach
// to the same region, and any process can hold an arbitrary number of handles.
//
// Signals: per-process bitmasks with blocking wait and non-blocking poll.

#ifndef SANDFLEA9_KERN_IPC_H
#define SANDFLEA9_KERN_IPC_H

#include "../include/dialect.h"
#include "../include/kern_sched.h"

// ── Per-Process Shared Memory Attachment Entry ───────────────────────────────
typedef struct proc_shmem_entry {
    bool active;       // true if this slot is in use
    u32  shm_id;       // global shared memory ID
    u64  virt_addr;    // virtual address mapped into this process's PML4
    u64  page_count;   // number of pages mapped
} proc_shmem_entry_t;

// ── Global Shared Memory Object (Kernel Physical Registry) ───────────────────
typedef struct kern_shmem {
    u32                shm_id;       // unique global ID (1, 2, 3...)
    u64                phys_base;    // base physical page
    u64                page_count;   // number of physical pages
    u32                flags;        // PAGE_RW, PAGE_USER, etc.
    u32                ref_count;    // number of active process attachments
    struct kern_shmem *next;         // global registry linked list
} kern_shmem_t;

// ── Core Kernel IPC Shared Memory API ────────────────────────────────────────

// Allocates physical pages and registers a global shm_id (ref_count starts at 0).
i32 shmem_create_region(u32 page_count, u32 flags);

// Maps shm_id into proc's PML4, allocates a slot in proc->shmem_table, ref_count++.
// Returns the local handle index (>= 0), or -1 on failure.
i32 shmem_attach_proc(kern_process_t *proc, u32 shm_id);

// Unmaps the handle from proc's PML4, frees slot, decrements ref_count.
// If ref_count == 0, frees physical pages and the global shmem struct.
bool shmem_detach_proc(kern_process_t *proc, i32 handle);

// Reads bytes from shared memory (O(1) lookup via handle).
i32 shmem_read_bytes(kern_process_t *proc, i32 handle, u32 offset, void *dst, u32 len);

// Writes bytes to shared memory (O(1) lookup via handle).
i32 shmem_write_bytes(kern_process_t *proc, i32 handle, u32 offset, const void *src, u32 len);

// Writes bytes to a shared memory ring buffer (byte 0=head, byte 1=tail, bytes 2..257 data).
i32 shmem_write_ring(u32 shm_id, const void *src, u32 len);

// Called by process_exit() to detach all active shared memory handles of dying process.
void ipc_process_cleanup(kern_process_t *proc);

// ── Signals ──────────────────────────────────────────────────────────────────
#define IPC_SIG_DATA_READY   (1u << 0)   // shared memory has new data
#define IPC_SIG_TERM         (1u << 1)   // polite termination request
#define IPC_SIG_CHILD_EVENT  (1u << 2)   // a child process changed state
#define IPC_SIG_KEY          (1u << 3)   // keystroke received (bit 8 = 1<<3)
#define IPC_SIG_STDOUT       (1u << 4)   // stdout data available in stdout ring (bit 16 = 1<<4)

bool ipc_signal_send(i32 target_pid, u32 signal_mask);
u32  ipc_signal_wait(u32 mask);
u32  ipc_signal_poll(u32 mask);

#endif //SANDFLEA9_KERN_IPC_H
