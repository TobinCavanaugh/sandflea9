//
// Created by tobin on 2025-12-10.
//

#ifndef KERN_SCHED_H
#define KERN_SCHED_H

#include "dialect.h"

typedef struct kern_task {
    u64 rsp; // MUST BE FIRST
    i32 pid;
    i32 state; // 0 ready, 1 running, 2 blocked, 3 dead
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

#endif //KERN_SCHED_H
