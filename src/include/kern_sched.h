//
// Created by tobin on 2025-12-10.
//

#ifndef KERN_SCHED_H
#define KERN_SCHED_H

#include "dialect.h"

typedef struct kern_task {
    u64 rsp; // MUST BE FIRST
    i32 pid;
    i32 state; // 0 ready, 1 running, 2 blocked
    struct kern_task * next;
} kern_task_t;

u0 sched_init();
u0 sched_yield();
u0 sched_run_next();

kern_task_t * sched_create_thread(u0(*function)(u0));

#endif //KERN_SCHED_H
