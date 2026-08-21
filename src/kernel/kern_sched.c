//
// Created by tobin on 2025-12-10.
//

#include "../include/kern_sched.h"

#include "../include/kern_vmm.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_serial.h"
#include "../include/kern_fs.h"
#include "../include/kern_terminal.h"
#include "../include/kern_profile.h"
#include "../include/kern_ipc.h"

extern u0 task_switch_asm(kern_task_t *current, kern_task_t *next);

kern_task_t *current_task = null;
kern_task_t *task_list_head = null;
static kern_task_t *ready_head = NULL;   // circular list: READY/RUNNING tasks only
kern_process_t *kernel_process = null;
kern_process_t *foreground_proc = null;

u64 read_cr3();

u0 sched_init() {
    PROFILE_AUTO_BEGIN;
    kernel_process = kmalloc(sizeof(kern_process_t));
    kernel_process->pid = 0;
    kernel_process->cr3 = read_cr3();
    kernel_process->heap_vptr = 0x1000000; // Arbitrary start for user-space heap
    kernel_process->mem_regions = null;
    kernel_process->thread_count = 1;
    for (int i = 0; i < MAX_FILE_HANDLES; i++) kernel_process->fd_table[i] = null;

    kern_task_t *root_task = kmalloc(sizeof(kern_task_t));
    root_task->tid = 0;
    root_task->state = TASK_STATE_RUNNING;
    root_task->process = kernel_process;
    root_task->stack_base = null; // We don't know the root stack base easily
    root_task->next = root_task;
    root_task->next_ready = root_task;

    current_task = root_task;
    task_list_head = root_task;
    ready_head = root_task;
    PROFILE_AUTO_END;
}

// ── Ready-list helpers (internal) ────────────────────────────────────────

// Remove t from the ready queue. Walk to find predecessor (ready list is
// small — typically 1-5 entries — so O(n) is fine).  Assumes t is in the list.
static void ready_list_remove(kern_task_t *t) {
    if (!ready_head) return;

    if (ready_head == t && ready_head->next_ready == t) {
        ready_head = NULL;             // only entry
        return;
    }

    if (ready_head == t)
        ready_head = t->next_ready;    // bump head before unlinking

    kern_task_t *cur = ready_head;
    while (cur->next_ready != t)
        cur = cur->next_ready;
    cur->next_ready = t->next_ready;
}

// Insert t into the ready queue (after ready_head).
static void ready_list_insert(kern_task_t *t) {
    if (!ready_head) {
        ready_head = t;
        t->next_ready = t;
    } else {
        t->next_ready = ready_head->next_ready;
        ready_head->next_ready = t;
    }
}

// Reap DEAD tasks from task_list_head.  Called from sched_run_next() so
// dead threads don't accumulate.  Extracted from the old O(n) dispatch loop.
static void sched_reap_dead(void) {
    kern_task_t *head = task_list_head;
    if (!head) return;

    u64 irq = save_irq_and_disable();
    kern_task_t *prev = head;
    kern_task_t *cur  = head->next;

    while (cur != head && task_list_head) {
        if (cur->state == TASK_STATE_DEAD && cur->tid != 0) {
            kern_task_t *dead = cur;
            prev->next = cur->next;

            if (dead == task_list_head)
                task_list_head = (prev == dead) ? NULL : prev;

            dead->process->thread_count--;
            if (dead->process->thread_count == 0)
                process_exit(dead->process);
            if (dead->stack_base)
                kfree(dead->stack_base);

            cur = prev->next;
            kfree(dead);

            if (!task_list_head) { restore_irq(irq); return; }
            head = task_list_head;
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
    restore_irq(irq);
}

i32 task_id_c = 0;
i32 process_id_c = 0;

u0 sched_thread_exit() {
    u64 irq = save_irq_and_disable();
    current_task->state = TASK_STATE_DEAD;
    ready_list_remove(current_task);
    restore_irq(irq);

    while (1) {
        sched_yield();
    }
}

kern_process_t *process_create() {
    kern_process_t *proc = kmalloc(sizeof(kern_process_t));
    if (!proc) return null;

    proc->pid = ++process_id_c;
    proc->heap_vptr = 0x1000000;
    proc->mem_regions = null;
    proc->thread_count = 0;
    for (int i = 0; i < MAX_FILE_HANDLES; i++) proc->fd_table[i] = null;
    proc->argc = 0;
    for (int i = 0; i < 16; i++) proc->argv[i] = null;
    proc->cleanup_fn  = null;
    proc->cleanup_ctx = null;
    proc->pending_signals = 0;
    proc->ipc_setup_ready = false;
    proc->ipc_setup_shmem_va = 0;
    proc->ipc_setup_peer_pid = 0;
    // Associate with the terminal session that was active when the
    // process was created, so screen_push_line routes output to the
    // correct TTY even after the user switches sessions.
    proc->terminal_session = active_session;

    // Create a new PML4
    u64 pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0) {
        kfree(proc);
        return null;
    }
    proc->cr3 = pml4_phys;

    u64 *new_pml4 = (u64 *) (pml4_phys + vmm_get_hhdm());
    u64 *old_pml4 = (u64 *) (read_cr3() + vmm_get_hhdm());

    // Copy kernel mapping (top 256 entries of PML4 for 64-bit)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = old_pml4[i];
    }

    return proc;
}

u0 process_exit(kern_process_t *proc) {
    if (proc == kernel_process) return; // Never exit kernel process
    if (proc->pid == -1) return; // Already reaped

    serial_outsf("Reaping process PID %d (CR3: %llx)\n", proc->pid, proc->cr3);
    i32 old_pid = proc->pid;
    proc->pid = -1; // Mark as reaped immediately

    if (proc == foreground_proc) {
        foreground_proc = null;
    }
    // Also clear the per-session foreground_proc so the main loop
    // stops forwarding keys to a freed process.
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].foreground_proc == (void*)proc) {
            sessions[i].foreground_proc = NULL;
        }
    }

    // IPC: clean up shared memory regions this process participates in.
    // Must run BEFORE mem_regions teardown so the PML4 is still valid.
    ipc_process_cleanup(proc);

    // Per-process cleanup hook. Runs while the process is still alive enough
    // for the hook to dereference `proc`; we invoke it BEFORE we start
    // freeing fields, and BEFORE the fd_table / mem_regions / proc are
    // released. The hook is responsible for releasing any module-specific
    // resources (e.g. a WASM m3 runtime + environment).
    if (proc->cleanup_fn) {
        u0 (*fn)(u0 *) = proc->cleanup_fn;
        u0 *ctx = proc->cleanup_ctx;
        proc->cleanup_fn  = null;
        proc->cleanup_ctx = null;
        fn(ctx);
    }

    for (int i = 0; i < proc->argc; i++) {
        if (proc->argv[i]) {
            kfree(proc->argv[i]);
            proc->argv[i] = null;
        }
    }

    // Close all open files
    for (int i = 0; i < MAX_FILE_HANDLES; i++) {
        if (proc->fd_table[i]) {
            // We need a way to close this without relying on 'current_task'
            // For now, let's just free the handle memory if fs_close is too tied to current_task
            // Better: modify fs_close to be more flexible.
            // For now, let's just use kfree if it was kmalloc'd in fs_open.
            kfree(proc->fd_table[i]);
            proc->fd_table[i] = null;
        }
    }

    // Free all memory regions
    kern_mem_region_t *region = proc->mem_regions;
    while (region) {
        kern_mem_region_t *next = region->next;

        for (u64 i = 0; i < region->page_count; i++) {
            u64 vaddr = region->virt + (i * PAGE_SIZE);
            u64 phys = vmm_get_phys_in_pml4(proc->cr3, vaddr);
            if (phys) {
                pmm_free(phys);
                vmm_unmap_page_in_pml4(proc->cr3, vaddr);
            }
        }

        kfree(region);
        region = next;
    }

    // TODO: Free PML4 structure (requires recursive walk)
    pmm_free(proc->cr3);
    kfree(proc);
    serial_outsf("Process PID %d resources freed\n", old_pid);
}

static kern_task_t *task_create_internal(kern_process_t *proc, u0 (*function)(u0 *), u0 *arg) {
    if (!proc && !kernel_process) return null; // Should not happen after sched_init

    kern_task_t *task = kmalloc(sizeof(kern_task_t));
    if (!task) return null;

    task->tid = ++task_id_c;
    task->state = TASK_STATE_READY;
    task->process = proc ? proc : kernel_process;
    task->signal_wait_mask = 0;

    u64 stack_size = 128 * 1024; // 128KiB
    u0 *stack_ptr_base = kmalloc(stack_size);
    if (!stack_ptr_base) {
        kfree(task);
        return null;
    }
    task->stack_base = stack_ptr_base;

    u64 stack_top = (u64) stack_ptr_base + stack_size;
    u64 *stack_ptr = (u64 *) stack_top;

    *(--stack_ptr) = (u64) sched_thread_exit;
    *(--stack_ptr) = (u64) function;
    *(--stack_ptr) = 0x202; // flags

    *(--stack_ptr) = 0; // rbx
    *(--stack_ptr) = 0; // rbp
    *(--stack_ptr) = (u64) arg; // rdi
    *(--stack_ptr) = 0; // rsi
    *(--stack_ptr) = 0; // r12
    *(--stack_ptr) = 0; // r13
    *(--stack_ptr) = 0; // r14
    *(--stack_ptr) = 0; // r15

    task->rsp = (u64) stack_ptr;

    u64 irq = save_irq_and_disable();
    task->process->thread_count++;
    serial_outsf("Thread created: TID %d in PID %d (new count: %d)\n", task->tid, task->process->pid,
                 task->process->thread_count);
    
    if (task_list_head) {
        task->next = task_list_head->next;
        task_list_head->next = task;
    } else {
        task->next = task;
        task_list_head = task;
    }

    // Also insert into ready queue.
    ready_list_insert(task);

    restore_irq(irq);

    return task;
}

kern_task_t *sched_create_thread(u0 (*function)(u0 *), u0 *arg) {
    return task_create_internal(kernel_process, function, arg);
}

kern_task_t *sched_create_process_thread(kern_process_t *proc, u0 (*function)(u0 *), u0 *arg) {
    return task_create_internal(proc, function, arg);
}

kern_task_t *sched_get_current_task() {
    return current_task;
}

kern_process_t *sched_get_current_process() {
    if (!current_task) return null;
    return current_task->process;
}

kern_task_t *sched_get_task_list_head() {
    return task_list_head;
}

kern_process_t *sched_get_kernel_process() {
    return kernel_process;
}


kern_task_t *sched_get_by_pid(i32 pid) {
    kern_task_t *head = sched_get_task_list_head();
    if (!head) return null;

    kern_task_t *cur = head;
    do {
        if (cur->process && cur->process->pid == pid) return cur;
        cur = cur->next;
    } while (cur != head);

    return null;
}


u8 sched_kill_process(i32 pid) {
    if (pid < 0) return 0;

    u64 irq = save_irq_and_disable();
    kern_task_t *head = task_list_head;
    if (!head) {
        restore_irq(irq);
        return 0;
    }

    kern_task_t *cur = head;
    u8 found = 0;
    do {
        if (cur->process->pid == pid) {
            cur->state = TASK_STATE_DEAD;
            ready_list_remove(cur);     // out of the dispatch queue
            found = 1;
        }

        cur = cur->next;
    } while (cur != head);

    restore_irq(irq);
    return found;
}

// ── Dispatch: O(1) pick-next from the ready queue ────────────────────────

u0 sched_run_next() {
    PROFILE_SCOPE("sched:run_next");

    // Phase 1: reap any dead tasks from the master list.
    sched_reap_dead();

    // Phase 2: pick next task from the ready queue.
    // No list walk — just one pointer advance.  Idle detection:
    // if only one task is runnable, don't context-switch.
    if (!ready_head) return;
    if (ready_head == ready_head->next_ready) return;

    u64 irq = save_irq_and_disable();
    kern_task_t *prev = ready_head;
    ready_head = ready_head->next_ready;
    current_task = ready_head;
    task_switch_asm(prev, current_task);
    restore_irq(irq);
}

u0 sched_yield() {
    sched_run_next();
}

// ── Block / unblock ──────────────────────────────────────────────────────

u0 sched_block_current(void) {
    u64 irq = save_irq_and_disable();
    current_task->state = TASK_STATE_BLOCKED;
    ready_list_remove(current_task);
    restore_irq(irq);

    // Edge case: nothing left to run.  Spin with interrupts enabled
    // until an unblock arrives (should never happen — kernel main loop
    // is always ready).
    while (!ready_head)
        asm volatile("hlt");

    sched_run_next();
}

u0 sched_unblock(kern_task_t *task) {
    if (!task) return;
    u64 irq = save_irq_and_disable();
    if (task->state == TASK_STATE_BLOCKED) {
        task->state = TASK_STATE_READY;
        ready_list_insert(task);
    }
    restore_irq(irq);
}
