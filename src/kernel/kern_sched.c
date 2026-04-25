//
// Created by tobin on 2025-12-10.
//

#include "../include/kern_sched.h"

#include "../include/kern_vmm.h"

extern u0 task_switch_asm(kern_task_t *current, kern_task_t *next);

kern_task_t *current_task = null;
kern_task_t *task_list_head = null;

u0 sched_init() {
    kern_task_t *root_task = kmalloc(sizeof(kern_task_t));
    root_task->pid = 0;
    root_task->state = 1;
    root_task->next = root_task;

    current_task = root_task;
    task_list_head = root_task;
}

i32 task_id_c = 0;

u0 sched_thread_exit() {
    current_task->state = TASK_STATE_DEAD;
    while (1) {
        sched_yield();
    }
}

kern_task_t *sched_create_thread(u0 (*function)(u0 *), u0 *arg) {
    kern_task_t *task = kmalloc(sizeof(kern_task_t));
    task->pid = ++task_id_c;
    task->state = TASK_STATE_READY;

    u64 stack_phys = pmm_alloc_page();
    u64 stack_virt = stack_phys + vmm_get_hhdm();

    // Create the stack and set its top value to be the function address
    u64 stack_top = (stack_virt + PAGE_SIZE);
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

    task->next = task_list_head->next;
    task_list_head->next = task;

    return task;
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
