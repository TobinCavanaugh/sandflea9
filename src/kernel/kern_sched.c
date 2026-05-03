//
// Created by tobin on 2025-12-10.
//

#include "../include/kern_sched.h"

#include "../include/kern_vmm.h"

extern u0 task_switch_asm(kern_task_t *current, kern_task_t *next);

kern_task_t *current_task = null;
kern_task_t *task_list_head = null;
kern_process_t *kernel_process = null;

u64 read_cr3();

u0 sched_init() {
    kernel_process = kmalloc(sizeof(kern_process_t));
    kernel_process->pid = 0;
    kernel_process->cr3 = read_cr3();
    kernel_process->heap_vptr = 0x1000000; // Arbitrary start for user-space heap
    kernel_process->mem_regions = null;

    kern_task_t *root_task = kmalloc(sizeof(kern_task_t));
    root_task->tid = 0;
    root_task->state = TASK_STATE_RUNNING;
    root_task->process = kernel_process;
    root_task->next = root_task;

    current_task = root_task;
    task_list_head = root_task;
}

i32 task_id_c = 0;
i32 process_id_c = 0;

u0 sched_thread_exit() {
    current_task->state = TASK_STATE_DEAD;
    
    // Check if this was the last thread in the process
    bool other_threads = false;
    kern_task_t *curr = current_task->next;
    while (curr != current_task) {
        if (curr->process == current_task->process && curr->state != TASK_STATE_DEAD) {
            other_threads = true;
            break;
        }
        curr = curr->next;
    }

    if (!other_threads) {
        process_exit(current_task->process);
    }

    while (1) {
        sched_yield();
    }
}

kern_process_t *process_create() {
    kern_process_t *proc = kmalloc(sizeof(kern_process_t));
    proc->pid = ++process_id_c;
    proc->heap_vptr = 0x1000000;
    proc->mem_regions = null;

    // Create a new PML4
    u64 pml4_phys = pmm_alloc_page();
    u64 *new_pml4 = (u64 *) (pml4_phys + vmm_get_hhdm());
    u64 *old_pml4 = (u64 *) (read_cr3() + vmm_get_hhdm());

    // Copy kernel mapping (top 256 entries of PML4 for 64-bit)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = old_pml4[i];
    }

    proc->cr3 = pml4_phys;
    return proc;
}

u0 process_exit(kern_process_t *proc) {
    if (proc == kernel_process) return; // Never exit kernel process

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
    // kfree(proc); 
}

kern_task_t *sched_create_thread(u0 (*function)(u0 *), u0 *arg) {
    kern_task_t *task = kmalloc(sizeof(kern_task_t));
    task->tid = ++task_id_c;
    task->state = TASK_STATE_READY;
    task->process = kernel_process; // Default to kernel process

    u64 stack_size = 128 * 1024;
    u0 *stack_ptr_base = kmalloc(stack_size);
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
    task->next = task_list_head->next;
    task_list_head->next = task;
    restore_irq(irq);

    return task;
}

kern_task_t *sched_create_process_thread(kern_process_t *proc, u0 (*function)(u0 *), u0 *arg) {
    kern_task_t *task = sched_create_thread(function, arg);
    task->process = proc;
    return task;
}

kern_task_t *sched_get_current_task() {
    return current_task;
}

kern_process_t *sched_get_current_process() {
    if (!current_task) return null;
    return current_task->process;
}

u0 sched_run_next() {
    if (!current_task) return; // This would be bad lol

    kern_task_t *last = current_task;
    kern_task_t *next = current_task->next;

    while (next->state > 1 && next != last) {
        next = next->next;
    }

    current_task = next;

    if (last == current_task) return;
    task_switch_asm(last, current_task);
}

u0 sched_yield() {
    kern_task_t *last = current_task;
    kern_task_t *next = current_task->next;

    while (next->state > 1 && next != last) {
        next = next->next;
    }

    current_task = next;
    task_switch_asm(last, current_task);
}
