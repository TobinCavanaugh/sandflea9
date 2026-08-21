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

    current_task = root_task;
    task_list_head = root_task;
    PROFILE_AUTO_END;
}

i32 task_id_c = 0;
i32 process_id_c = 0;

u0 sched_thread_exit() {
    u64 irq = save_irq_and_disable();
    current_task->state = TASK_STATE_DEAD;
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
            found = 1;
        }

        cur = cur->next;
    } while (cur != head);

    restore_irq(irq);
    return found;
}

u0 sched_sleep(u64 ms) {
    if (!current_task) return;
    if (ms == 0) {
        sched_yield();
        return;
    }
    extern volatile u64 sw;
    u64 irq = save_irq_and_disable();
    current_task->wake_at_tick = sw + ms;
    current_task->state = TASK_STATE_SLEEPING;
    restore_irq(irq);

    sched_run_next();
}

u0 sched_run_next() {
    if (!current_task) return;

    extern volatile u64 sw;
    u64 irq = save_irq_and_disable();

    // Save the original task — we need it to detect wrap-around.
    kern_task_t *start = current_task;
    kern_task_t *prev = current_task;
    kern_task_t *next = current_task->next;

    while (next != current_task) {
        if (next->state == TASK_STATE_DEAD && next->tid != 0) {
            kern_task_t *dead_task = next;
            prev->next = next->next;

            if (dead_task == task_list_head) {
                task_list_head = prev;
            }

            dead_task->process->thread_count--;
            if (dead_task->process->thread_count == 0) {
                process_exit(dead_task->process);
            }

            if (dead_task->stack_base) {
                kfree(dead_task->stack_base);
            }

            next = prev->next;
            kfree(dead_task);
        } else if (next->state == TASK_STATE_SLEEPING) {
            if (sw >= next->wake_at_tick) {
                // Wake-up time reached!
                next->state = TASK_STATE_READY;
                break;
            }
            // Still sleeping — skip immediately in O(1) without context switch
            prev = next;
            next = next->next;
        } else if (next->state == TASK_STATE_BLOCKED) {
            // Blocked on I/O or signal — skip
            prev = next;
            next = next->next;
        } else {
            // Found a READY or RUNNING task
            break;
        }
    }

    // If we wrapped all the way around back to ourselves:
    if (next == start) {
        if ((current_task->state == TASK_STATE_SLEEPING && sw < current_task->wake_at_tick) ||
            current_task->state == TASK_STATE_BLOCKED) {
            restore_irq(irq);
            asm volatile("hlt");
            return;
        }
        restore_irq(irq);
        return;
    }

    current_task = next;

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
    restore_irq(irq);

    sched_run_next();
}

u0 sched_unblock(kern_task_t *task) {
    if (!task) return;
    u64 irq = save_irq_and_disable();
    if (task->state == TASK_STATE_BLOCKED || task->state == TASK_STATE_SLEEPING) {
        task->state = TASK_STATE_READY;
    }
    restore_irq(irq);
}
