//
// Created by tobin on 2025-12-10.
//

#ifndef KERN_SCHED_H
#define KERN_SCHED_H

#include "dialect.h"

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
    kern_mem_region_t *mem_regions; // List of all allocated regions
    // Future: WASM context, file descriptors, etc.
} kern_process_t;

typedef struct kern_task {
    u64 rsp; // MUST BE FIRST
    i32 tid;
    i32 state; // 0 ready, 1 running, 2 blocked, 3 dead
    kern_process_t *process;
    struct kern_task * next;
} kern_task_t;

#define TASK_STATE_READY 0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_BLOCKED 2
#define TASK_STATE_DEAD 3

u0 sched_init();
u0 sched_yield();
u0 sched_run_next();
u0 sched_thread_exit();

kern_task_t * sched_create_thread(u0(*function)(u0*), u0* arg);
kern_task_t * sched_create_process_thread(kern_process_t *proc, u0(*function)(u0*), u0* arg);
kern_process_t * process_create();
u0 process_exit(kern_process_t *proc);

kern_task_t * sched_get_current_task();
kern_process_t * sched_get_current_process();

u0* pmalloc(u64 size);
u0 pfree(void *ptr);

#endif //KERN_SCHED_H
