// kern_ipc.c — shared memory + signal IPC implementation.
//
// Shared memory: maps the same physical page into two process PML4s
// via vmm_map_page_in_pml4().  Physical pages are owned by the kernel;
// process_exit calls ipc_process_cleanup() to unmap dying-process VAs.
//
// Signals: per-process bitmask.  ipc_signal_send() wakes a BLOCKED
// task if its wait mask overlaps with the sent signal.

#include "../include/kern_ipc.h"
#include "../include/kern_vmm.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"
#include "../include/kern_asmstubs.h"
#include "wasm3-0.5.0/source/m3_env.h"

// ── Global shared-memory region list ─────────────────────────────────────────
// Protected by irq save/restore (single-core assumption).

static kern_shmem_t *g_shmem_head = NULL;

// ── Shared memory: create ────────────────────────────────────────────────────

kern_shmem_t *shmem_create(kern_process_t *a, kern_process_t *b,
                           u32 size, u32 flags,
                           u64 *out_va_a, u64 *out_va_b) {
    if (!a || !b || size == 0 || size > PAGE_SIZE) return NULL;

    // Allocate one physical page.
    u64 phys = pmm_alloc_page();
    if (!phys) return NULL;

    // Pick virtual addresses: bump each process's heap pointer.
    u64 irq = save_irq_and_disable();
    u64 va_a = a->heap_vptr;
    a->heap_vptr += PAGE_SIZE;
    u64 va_b = b->heap_vptr;
    b->heap_vptr += PAGE_SIZE;
    restore_irq(irq);

    // Map into both PML4s.
    u32 map_flags = (flags & (PAGE_RW | PAGE_USER)) | PAGE_USER;
    vmm_map_page_in_pml4(a->cr3, phys, va_a, map_flags);
    vmm_map_page_in_pml4(b->cr3, phys, va_b, map_flags);

    // Allocate handle.
    kern_shmem_t *sh = (kern_shmem_t *)kmalloc(sizeof(kern_shmem_t));
    if (!sh) {
        // Unmap and free phys — OOM rollback.
        vmm_unmap_page_in_pml4(a->cr3, va_a);
        vmm_unmap_page_in_pml4(b->cr3, va_b);
        pmm_free(phys);
        return NULL;
    }

    sh->phys_base  = phys;
    sh->page_count = 1;
    sh->flags      = flags;
    sh->va_a       = va_a;
    sh->va_b       = va_b;
    sh->proc_a     = a;
    sh->proc_b     = b;
    sh->a_alive    = true;
    sh->b_alive    = true;

    // Prepend to global list.
    irq = save_irq_and_disable();
    sh->next = g_shmem_head;
    g_shmem_head = sh;
    restore_irq(irq);

    *out_va_a = va_a;
    *out_va_b = va_b;

    serial_outsf("IPC: shmem created — 1 page at 0x%llx (pid %d) / 0x%llx (pid %d)\n",
                 va_a, a->pid, va_b, b->pid);
    return sh;
}

// ── Shared memory: destroy ───────────────────────────────────────────────────

void shmem_destroy(kern_shmem_t *sh) {
    if (!sh) return;

    // Unmap from proc A + free the phys page (once — it's shared).
    if (sh->a_alive && sh->proc_a) {
        u64 phys = vmm_get_phys_in_pml4(sh->proc_a->cr3, sh->va_a);
        vmm_unmap_page_in_pml4(sh->proc_a->cr3, sh->va_a);
        if (phys) pmm_free(phys);
    }

    // Unmap from proc B (no double-free — phys already freed above).
    if (sh->b_alive && sh->proc_b) {
        vmm_unmap_page_in_pml4(sh->proc_b->cr3, sh->va_b);
    }

    // Remove from global list.
    u64 irq = save_irq_and_disable();
    if (g_shmem_head == sh) {
        g_shmem_head = sh->next;
    } else {
        kern_shmem_t *cur = g_shmem_head;
        while (cur && cur->next != sh) cur = cur->next;
        if (cur) cur->next = sh->next;
    }
    restore_irq(irq);

    kfree(sh);
}

// ── Process cleanup hook ─────────────────────────────────────────────────────
// Called from process_exit() BEFORE the process's cr3/mem_regions are freed.
// For each shared region the dying process participates in:
//   - unmap its VA from its PML4
//   - mark that side dead
//   - if both sides dead, free phys page + the handle

void ipc_process_cleanup(kern_process_t *proc) {
    if (!proc) return;

    u64 irq = save_irq_and_disable();
    kern_shmem_t *prev = NULL;
    kern_shmem_t *cur  = g_shmem_head;

    while (cur) {
        bool is_a = (cur->proc_a == proc);
        bool is_b = (cur->proc_b == proc);
        kern_shmem_t *next = cur->next;

        if (!is_a && !is_b) {
            prev = cur;
            cur  = next;
            continue;
        }

        // Unmap this process's VA (don't free phys — other side may still use it).
        u64 va = is_a ? cur->va_a : cur->va_b;
        vmm_unmap_page_in_pml4(proc->cr3, va);

        if (is_a) cur->a_alive = false;
        if (is_b) cur->b_alive = false;

        if (!cur->a_alive && !cur->b_alive) {
            // Both sides dead — free phys page and the handle.
            pmm_free(cur->phys_base);

            // Unlink from global list.
            if (prev) prev->next = next;
            else      g_shmem_head = next;

            kfree(cur);
            // prev stays unchanged (we removed cur, next is already captured).
        } else {
            prev = cur;
        }

        cur = next;
    }
    restore_irq(irq);
}

// ── Signals ──────────────────────────────────────────────────────────────────
// Lightweight per-process notification bits.  No signal handlers — just a
// pending bitmask and a blocking-wait mechanism integrated with the scheduler.

bool ipc_signal_send(i32 target_pid, u32 signal_mask) {
    if (target_pid < 0 || signal_mask == 0) return false;

    u64 irq = save_irq_and_disable();

    kern_task_t *task = sched_get_by_pid(target_pid);
    if (!task || !task->process) {
        restore_irq(irq);
        return false;
    }

    kern_process_t *proc = task->process;
    proc->pending_signals |= signal_mask;

    // Wake any BLOCKED task whose wait mask overlaps.
    // Walk the master list (not the ready queue — BLOCKED tasks aren't there).
    kern_task_t *head = sched_get_task_list_head();
    bool woken = false;
    if (head) {
        kern_task_t *cur = head;
        do {
            if (cur->process == proc &&
                cur->state == TASK_STATE_BLOCKED &&
                (cur->signal_wait_mask & signal_mask)) {
                sched_unblock(cur);
                woken = true;
            }
            cur = cur->next;
        } while (cur != head);
    }

    restore_irq(irq);
    return woken;
}

u32 ipc_signal_wait(u32 mask) {
    if (mask == 0) return 0;

    kern_process_t *proc = sched_get_current_process();
    kern_task_t    *task = sched_get_current_task();
    if (!proc || !task) return 0;

    u64 irq = save_irq_and_disable();

    // Fast path: signal already pending.
    u32 pending = proc->pending_signals & mask;
    if (pending) {
        proc->pending_signals &= ~pending;
        restore_irq(irq);
        return pending;
    }

    // Block until a matching signal arrives.
    task->signal_wait_mask = mask;
    restore_irq(irq);

    sched_block_current();  // removes us from ready queue, sets BLOCKED, yields

    // We're back — consume pending signals.
    irq = save_irq_and_disable();
    pending = proc->pending_signals & mask;
    proc->pending_signals &= ~pending;
    task->signal_wait_mask = 0;
    restore_irq(irq);
    return pending;
}

u32 ipc_signal_poll(u32 mask) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return 0;

    u64 irq = save_irq_and_disable();
    u32 pending = proc->pending_signals & mask;
    proc->pending_signals &= ~pending;
    restore_irq(irq);
    return pending;
}

// ── Parent-to-child setup handoff ────────────────────────────────────────────

void ipc_setup_send(i32 child_pid, u64 shmem_va, i32 peer_pid) {
    u64 irq = save_irq_and_disable();
    kern_task_t *task = sched_get_by_pid(child_pid);
    if (!task || !task->process) { restore_irq(irq); return; }

    kern_process_t *proc = task->process;
    proc->ipc_setup_shmem_va = shmem_va;
    proc->ipc_setup_peer_pid = peer_pid;
    proc->ipc_setup_ready     = true;

    // Wake the child if it's BLOCKED in ipc_setup_wait.
    if (task->state == TASK_STATE_BLOCKED)
        sched_unblock(task);
    restore_irq(irq);
}

// ── WASM host functions (linked under "env") ─────────────────────────────────

m3ApiRawFunction(wasm_ipc_get_pid) {
    m3ApiReturnType(i32)
    kern_process_t *proc = sched_get_current_process();
    m3ApiReturn(proc ? proc->pid : -1);
}

m3ApiRawFunction(wasm_ipc_setup_wait) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, shmem_va_ptr)    // out: shmem VA (u64 written here)
    m3ApiGetArg(u32, peer_pid_ptr)    // out: peer PID  (i32 written here)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || shmem_va_ptr + 8 > mem_size || peer_pid_ptr + 4 > mem_size) {
        m3ApiReturn(-1);
    }

    kern_process_t *proc = sched_get_current_process();
    kern_task_t    *task = sched_get_current_task();
    if (!proc || !task) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    if (!proc->ipc_setup_ready) {
        // Block until parent calls ipc_setup_send().
        restore_irq(irq);
        sched_block_current();
        // Woken up — re-check.
        irq = save_irq_and_disable();
    }

    if (!proc->ipc_setup_ready) {
        restore_irq(irq);
        m3ApiReturn(-1);
    }

    *(u64*)(mem + shmem_va_ptr) = proc->ipc_setup_shmem_va;
    *(i32*)(mem + peer_pid_ptr) = proc->ipc_setup_peer_pid;
    restore_irq(irq);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasm_ipc_signal_send) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, target_pid)
    m3ApiGetArg(i32, mask)
    bool ok = ipc_signal_send(target_pid, (u32)mask);
    m3ApiReturn(ok ? 1 : 0);
}

m3ApiRawFunction(wasm_ipc_signal_wait) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, mask)
    u32 got = ipc_signal_wait((u32)mask);
    m3ApiReturn((i32)got);
}
