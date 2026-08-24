//
// Created by tobin on 2025-12-10.
//

#ifndef KERN_SCHED_H
#define KERN_SCHED_H

#include "dialect.h"
#include "kern_fs.h"

typedef struct kern_mem_region {
    u64 phys;
    u64 virt;
    u64 page_count;
    struct kern_mem_region *next;
} kern_mem_region_t;

typedef struct kern_process {
    i32 pid;
    u64 cr3; // Physical address of PML4
    u64 heap_vptr; // Current virtual address for next allocation
    struct kern_mem_region *mem_regions; // List of all allocated regions
    i32 thread_count;
    file_handle_t *fd_table[128]; // MAX_FILE_HANDLES
    // Future: WASM context, etc.
    i32 argc;
    char *argv[16];
    // Terminal session this process was created in — output from
    // screen_push_line / screen_push_linef is routed to this session's
    // scrollback (and cell buffer) instead of whatever session happens
    // to be active at the time the output is written.
    void *terminal_session;

    // Per-process cleanup hook: invoked from process_exit() *before* the
    // process struct is freed, so the owning thread (or the shim that
    // spawned it) can release resources that aren't visible to the
    // generic reaper (e.g. a WASM m3 runtime + environment).
    u0 (*cleanup_fn)(u0 *);
    u0  *cleanup_ctx;

    // IPC: pending signal bitmask (see kern_ipc.h for bit definitions).
    u32 pending_signals;

    // IPC setup: parent-to-child handoff (see kern_ipc.h::ipc_setup_send).
    bool ipc_setup_ready;       // true after ipc_setup_send() delivers data
    u64  ipc_setup_shmem_va;    // shared memory VA for this child
    i32  ipc_setup_peer_pid;    // PID of the other child in the pair
} kern_process_t;

extern kern_process_t *foreground_proc;

typedef struct kern_task {
    u64 rsp; // MUST BE FIRST
    i32 tid;
    i32 state; // 0 ready, 1 running, 2 blocked, 3 dead
    u0 *stack_base;
    kern_process_t *process;
    struct kern_task * next;        // master list (all tasks, for PID lookup & reaping)
    struct kern_task * next_ready;  // ready queue (READY/RUNNING only — O(1) dispatch)
    u32 signal_wait_mask;  // IPC: signal mask this task is blocked on
    u64 run_ticks;         // CPU accounting: 10ms quanta actually granted
                           // to this task (credited in the timer ISR).
} kern_task_t;

#define TASK_STATE_READY 0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_BLOCKED 2
#define TASK_STATE_DEAD 3

u0 sched_init();
u0 sched_yield();
u0 sched_run_next();
u0 sched_thread_exit();

// Block the calling task (remove from ready queue, set BLOCKED, yield).
// Caller must have interrupts enabled.
u0 sched_block_current(void);

// Unblock a task (insert into ready queue, set READY).
// Safe to call from any context (does its own irq management).
u0 sched_unblock(kern_task_t *task);

kern_task_t * sched_create_thread(u0(*function)(u0*), u0* arg);
kern_task_t * sched_create_process_thread(kern_process_t *proc, u0(*function)(u0*), u0* arg);

// Idle-thread wakeup: the boot/shell thread (tid 0) is skipped by the
// round-robin while other threads are READY, so compute threads (e.g. the
// Doom WASM loop) get the full CPU instead of alternating with a halting
// idle thread. The keyboard ISR calls sched_idle_wake() when input arrives;
// the main loop calls sched_idle_clear() after draining it.
u0 sched_idle_wake(void);
u0 sched_idle_clear(void);
kern_process_t * process_create();
u0 process_exit(kern_process_t *proc);

kern_task_t * sched_get_current_task();
kern_task_t *sched_get_by_pid(i32 pid);
kern_task_t * sched_get_task_list_head();

kern_process_t * sched_get_current_process();
kern_process_t * sched_get_kernel_process();
u8 sched_kill_process(i32 pi);

u0* pmalloc(u64 size);
u0 pfree(void *ptr);

#endif //KERN_SCHED_H
