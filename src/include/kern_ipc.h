// kern_ipc.h — Inter-process communication: shared memory + signals.
//
// Shared memory maps the same physical pages into two process PML4s.
// Each side gets a (possibly different) virtual address.  The kernel
// owns the physical pages until both processes exit or the region is
// explicitly destroyed.
//
// Signals are lightweight per-process notification bits.  A process
// can block waiting for a mask of signals; the sender sets a bit and
// the scheduler wakes the blocked task.

#ifndef SANDFLEA9_KERN_IPC_H
#define SANDFLEA9_KERN_IPC_H

#include "../include/dialect.h"
#include "../include/kern_sched.h"

// ── Shared memory ────────────────────────────────────────────────────────────

// Opaque handle returned by shmem_create().  The caller stores it and
// passes it to shmem_destroy() to tear down the region.
typedef struct kern_shmem {
    u64             phys_base;      // first physical page
    u64             page_count;
    u32             flags;           // PAGE_RW, PAGE_USER, etc.
    u64             va_a;            // virtual addr in proc_a
    u64             va_b;            // virtual addr in proc_b
    kern_process_t *proc_a;
    kern_process_t *proc_b;
    bool            a_alive;         // false once proc_a exits
    bool            b_alive;
    struct kern_shmem *next;         // global linked list
} kern_shmem_t;

// Create a shared memory region visible to both processes.
//
//   size  — bytes (rounded up to PAGE_SIZE; MVP caps at PAGE_SIZE)
//   flags — VMM page flags: PAGE_RW | PAGE_USER for writable, or
//           just PAGE_USER for read-only
//   out_va_a, out_va_b — virtual addresses the region was mapped at
//                        in each process (may differ).
//
// Returns NULL on failure (size==0, OOM, either proc invalid).
// The caller is responsible for communicating the returned VA to
// each process (e.g. via argv at spawn time).
kern_shmem_t *shmem_create(kern_process_t *a, kern_process_t *b,
                           u32 size, u32 flags,
                           u64 *out_va_a, u64 *out_va_b);

// Tear down a shared memory region immediately.
// Unmaps from both PML4s, frees physical pages, removes from global
// list, and frees the handle.  Safe to call even if one process has
// already exited (the handle tracks liveness per-process).
void shmem_destroy(kern_shmem_t *sh);

// Called by process_exit() to clean up any shared regions the dying
// process participates in.  For each region: unmaps the process's VA,
// marks that side dead, and frees physical pages if both sides are dead.
void ipc_process_cleanup(kern_process_t *proc);

// ── Signals (lightweight per-process notification) ───────────────────────────

// Signal bits (extensible — add more as needed).
// Bit 0 is reserved (no signal).
#define IPC_SIG_DATA_READY   (1u << 0)   // shared memory has new data
#define IPC_SIG_TERM         (1u << 1)   // polite termination request
#define IPC_SIG_CHILD_EVENT  (1u << 2)   // a child process changed state

// Send a signal to a process.  If any task in that process is BLOCKED
// in ipc_signal_wait() and the signal matches its wait mask, the task
// is transitioned to READY so the scheduler will resume it.
//
// Returns true if a task was woken, false if no matching waiter existed.
bool ipc_signal_send(i32 target_pid, u32 signal_mask);

// Block the calling task until at least one signal in `mask` arrives.
// Returns the set of pending signals that matched (may include multiple
// bits if several arrived simultaneously).  Non-matched signals remain
// pending.
//
// Must be called from the task that should block (uses current_task).
u32 ipc_signal_wait(u32 mask);

// Non-blocking poll: returns currently-pending signals matching `mask`
// and atomically clears them.  Returns 0 if none.
u32 ipc_signal_poll(u32 mask);

// ── Parent-to-child setup handoff ────────────────────────────────────────────

// Called by the parent process AFTER shmem_create(). Delivers the shared
// memory VA and peer PID to a child, then unblocks the child (the child
// must be blocked in ipc_setup_wait).
void ipc_setup_send(i32 child_pid, u64 shmem_va, i32 peer_pid);

// ── WASM-exported IPC host functions ─────────────────────────────────────────
// Declared in wasm_spawn.c (which includes m3_env.h for the IM3Runtime/
// IM3ImportContext types).  This header only declares the non-WASM API.

#endif //SANDFLEA9_KERN_IPC_H
