# Scheduler Optimization: Ready Queue & Wake Path Improvements

This document analyzes the current scheduler, identifies real vs. perceived bottlenecks, and evaluates approaches to improve throughput and wake latency. The focus is on **perf per implementation cost** — we want the biggest wins for the smallest delta.

---

## 1. Current Architecture

### 1.1 Data Structures

```
task_list_head ──┐
                 ▼
  ┌──────┐    ┌──────┐    ┌──────┐    ┌──────┐
  │ TID 0│───▶│ TID 1│───▶│ TID 2│───▶│ TID 3│
  │KERNEL│    │ READY│    │BLOCKED│   │ READY│
  └──────┘    └──────┘    └──────┘    └──────┘
       ▲                                      │
       └──────────────────────────────────────┘
```

- **One circular linked list** (`task_list_head`) contains every task regardless of state.
- `kern_task_t.state`: READY (0), RUNNING (1), BLOCKED (2), DEAD (3).
- `current_task` is the running task; it stays in the list.
- `sched_run_next()` walks the list clockwise from `current_task`, skipping BLOCKED/DEAD, and picks the first READY task.
- Dead task reaping is done **inline** during the walk — the walker unlinks and frees DEAD tasks it encounters.

### 1.2 Scheduler Invocation Points

| Call site | Trigger | Frequency |
|---|---|---|
| `timer_handler` (main.c) | APIC timer IRQ 32 | **100 Hz** (every 10ms) |
| `sched_yield()` | Voluntary yield from WASM, IPI wait, thread exit | Sporadic |
| `ipc_signal_wait()` | WASM process blocks on signal | Per-IPC message |
| `ipc_setup_wait()` | WASM child waits for IPC handoff | Once per IPC spawn pair |

### 1.3 Current `sched_run_next()` Logic (abridged)

```c
void sched_run_next() {
    kern_task_t *start = current_task;
    kern_task_t *prev  = current_task;
    kern_task_t *next  = current_task->next;

    while (next != current_task) {
        if (next->state == DEAD && next->tid != 0) {
            // reap: unlink, process_exit if thread_count==0, kfree stack, kfree task
            next = prev->next;
            continue;
        }
        if (next->state > 1) {       // BLOCKED (2) or DEAD (3) — skip
            prev = next;
            next = next->next;
            continue;
        }
        // Found READY (0) or RUNNING (1)
        break;
    }

    if (next == start) return;  // no one else runnable
    current_task = next;
    task_switch_asm(prev, current_task);
}
```

**Worst case**: every non-current task is BLOCKED. The loop visits every node in the ring before wrapping back to itself. Cost: O(n).

### 1.4 IPC Wake Paths (also O(n))

**`ipc_signal_send()`** — walks the full task list to find BLOCKED tasks whose wait mask matches:

```c
kern_task_t *head = sched_get_task_list_head();
kern_task_t *cur = head;
do {
    if (cur->process == proc &&
        cur->state == TASK_STATE_BLOCKED &&
        (cur->signal_wait_mask & signal_mask)) {
        cur->state = TASK_STATE_READY;
        woken = true;
    }
    cur = cur->next;
} while (cur != head);
```

**`sched_get_by_pid()`** — also walks the full list (used by `ipc_signal_send`, `ipc_setup_send`, `wasm_spawn` wait loop).

---

## 2. What's Actually Slow?

### 2.1 The CPU-Math Reality

At current scale (~6–20 tasks), O(n) list walks are **not measurable** in wall-clock time:

- Walking 20 nodes: ~200 CPU cycles (memory loads in L1/L2).
- At 2 GHz: **100 nanoseconds**.
- At 100 Hz: **10 microseconds per second** total (0.001% of one core).

The list walk is **not a CPU bottleneck** at this scale.

### 2.2 The Real Problems

| Problem | Why it matters |
|---|---|
| **Mixed concerns** | Dead reaping, task selection, and blocked skipping all share one loop. Hard to reason about, easy to introduce bugs. |
| **Wake latency** | `ipc_signal_send` scans the full task list. For N=20 it's trivial; for N=200 it would be 2μs per wake. Not urgent today but the IPC subsystem was *just built* — now is the time to get the wake path right before it accrues callers. |
| **Multicore readiness** | A single `task_list_head` protected by `save_irq_and_disable` is fundamentally single-core. The multicore writeup already identifies this. Separating ready/blocked now makes per-core ready queues trivial later. |
| **Correctness fragility** | The current wrap-around logic (line 330-341 of kern_sched.c) has subtle comments about why `prev == current_task` is the wrong check. This is a smell — the state transitions should be explicit, not emergent from list geometry. |

---

## 3. Approaches

### Approach A: Simple Ready Queue (Recommended)

Add a **second circular list** for READY/RUNNING tasks only. The master `task_list_head` stays for accounting (PID lookup, kill, reaping), but the scheduler never walks it for task selection.

```
        task_list_head (accounting)        ready_head (dispatch)
        ┌──────┐  ┌──────┐  ┌──────┐       ┌──────┐  ┌──────┐
        │TID 0 │─▶│TID 1 │─▶│TID 2 │       │TID 0 │─▶│TID 3 │
        │KERNEL│  │ READY│  │BLOCKED│       │KERNEL│  │ READY│
        └──────┘  └──────┘  └──────┘       └──────┘  └──────┘
             ▲                    │              ▲           │
             └────────────────────┘              └───────────┘
```

**Key invariants:**
- A task is in `ready_head` ↔ `state ∈ {READY, RUNNING}`
- `sched_run_next()`: advance `ready_head = ready_head->next`, context switch. **O(1)**.
- To block: remove from `ready_head`, set state = BLOCKED.
- To unblock: insert into `ready_head`, set state = READY.
- Dead reaping: done in a separate `sched_reap_dead()` called from `sched_run_next()` or a periodic sweep. Walk `task_list_head` for DEAD tasks (rare path, not on the timer hot path).

**LOC change**: ~80 lines added, ~30 lines removed/modified in `kern_sched.c`. Light IPC changes (~20 lines).

**Pros:**
- O(1) task selection on every timer tick.
- Clear separation: "am I runnable?" = "am I in the ready list?"
- Removes the fragile wrap-around detection.
- Multicore: each core gets its own `ready_head` and the accounting list stays global.

**Cons:**
- Two lists to maintain (insert/remove on every state transition).
- Slightly more code than current.

### Approach B: Ready Queue + Per-Process Blocked Pointer

Same as A, plus a `kern_task_t *blocked_task` field on `kern_process_t` that points to the task (if any) currently blocked in `ipc_signal_wait()`.

**Why:** Today, `ipc_signal_send()` does `sched_get_by_pid()` (O(n)) then walks the task list again to find blocked tasks in that process. With a blocked pointer, the wake is **O(1)** — just check `proc->blocked_task` and unblock it.

**IPC wake goes from:**
```
sched_get_by_pid (walk N tasks) + walk N tasks looking for BLOCKED
                                  ↓
proc->blocked_task (single pointer deref + unblock)
```

**LOC change**: ~10 lines on top of Approach A.

**Caveat:** Assumes one blocked task per process. Today this is true — `wasm3` is single-threaded, and `ipc_signal_wait` blocks the calling task. If we ever have multi-threaded WASM, we'd need a list of blocked tasks per process. That's a problem for future-us; the blocked pointer can become a list head trivially.

### Approach C: Priority Bitmap (Linux O(1)-style)

An array of `kern_task_t *queue[32]` indexed by priority level, plus a `u32 bitmap` where bit `i` is set if queue `i` is non-empty. `sched_run_next()` uses `bsf` (bit scan forward) to find the highest-priority non-empty queue.

**Pros:** O(1) pick, built-in priority support, proven design.

**Cons:**
- We don't have task priorities today. Adding them is a separate design decision (what are the priority levels? who sets them? does the kernel preempt?).
- ~150+ LOC of new infrastructure.
- Overengineered for a hobby kernel with < 50 tasks.

**Verdict:** Premature. Revisit when/if priority scheduling is needed.

### Approach D: Intrusive Ring Buffer of Task Pointers

Replace the circular linked list with a **fixed-size array** of task pointers (`kern_task_t *slots[256]`) and a `read_idx` / `write_idx`. Ready tasks are enqueued at `write_idx`, dequeued at `read_idx`. Blocked tasks are simply not in the array.

**Pros:** O(1) enqueue/dequeue, cache-friendly (array is contiguous), no linked-list pointer chasing.

**Cons:**
- Fixed maximum (256 is fine for now, but hard to tune).
- Removing an arbitrary element (kill, process exit) requires scanning the array or leaving a tombstone.
- The array doesn't help with PID lookup or dead reaping — still need a master list.

**Verdict:** The array is nice for cache locality but the linked-list ready queue (Approach A) is simpler and has no size limit. At current scale, cache effects are irrelevant.

---

## 4. Recommendation: Approach B

**Phase 1 (Ready Queue):** Separate `ready_head` circular list. O(1) dispatch. Clean up the reaping path.

**Phase 2 (Blocked Pointer):** Add `proc->blocked_task`. O(1) IPC wake.

This is the sweet spot:
- **~90 LOC net change** across `kern_sched.c`, `kern_sched.h`, and `kern_ipc.c`.
- **No API breakage** — all existing callers of `sched_yield()`, `sched_run_next()`, `sched_get_current_task()`, etc. continue to work.
- **Multicore-friendly** — per-core ready queues slot right in.
- **IPC wake** becomes direct instead of scan-based, which matters if we ever run IPC-heavy workloads (pipes, window manager messages, etc.).

---

## 5. Implementation Plan

### Phase 1: Ready Queue (~70 LOC)

**Files touched:** `kern_sched.h`, `kern_sched.c`, `kern_ipc.c`, `kern_tests.c` (debug output only).

1. **Add `ready_head`** to `kern_sched.c`:
   ```c
   static kern_task_t *ready_head = NULL;  // circular list of READY/RUNNING tasks
   ```

2. **Add list helpers** (or inline):
   ```c
   // Remove a task from the ready list.
   static void ready_list_remove(kern_task_t *t);
   // Insert a task into the ready list (after ready_head).
   static void ready_list_insert(kern_task_t *t);
   ```

3. **Modify `task_create_internal`**: insert new task into both `task_list_head` and `ready_head`.

4. **Add `sched_block_current()`** (replaces inline `task->state = BLOCKED; sched_yield()`):
   ```c
   void sched_block_current() {
       kern_task_t *task = current_task;
       task->state = TASK_STATE_BLOCKED;
       ready_list_remove(task);
       sched_run_next();  // O(1): picks ready_head->next
   }
   ```

5. **Add `sched_unblock(task)`**:
   ```c
   void sched_unblock(kern_task_t *task) {
       task->state = TASK_STATE_READY;
       ready_list_insert(task);
   }
   ```

6. **Rewrite `sched_run_next()`**:
   ```c
   void sched_run_next() {
       if (!ready_head) return;
       if (ready_head == ready_head->next) {
           // Only one ready task — no switch needed unless it's not us.
           if (ready_head != current_task) { /* switch */ }
           return;
       }
       kern_task_t *prev = ready_head;
       ready_head = ready_head->next;
       current_task = ready_head;
       task_switch_asm(prev, ready_head);
   }
   ```
   Note: this needs care to handle `current_task` being the one that just blocked (it's no longer in ready_head).

7. **Extract dead reaping** into `sched_reap_dead()` called from `sched_run_next()` or a periodic timer:
   ```c
   static void sched_reap_dead() {
       // Walk task_list_head, unlink & free any DEAD tasks.
       // Runs infrequently, not on the hot path.
   }
   ```

8. **Update `sched_thread_exit()`**: set state to DEAD, call `ready_list_remove`, call `sched_yield()`.

9. **Update `sched_kill_process()`**: set state to DEAD, call `ready_list_remove` for each killed task.

### Phase 2: Per-Process Blocked Pointer (~20 LOC)

1. **Add `blocked_task` to `kern_process_t`**:
   ```c
   kern_task_t *blocked_task;  // task blocking in ipc_signal_wait, or NULL
   ```

2. **Update `ipc_signal_wait()`**: set `proc->blocked_task = task` before yielding, clear on wake.

3. **Rewrite `ipc_signal_send()`**:
   ```c
   bool ipc_signal_send(i32 target_pid, u32 signal_mask) {
       // ... get proc by PID (still O(n) via sched_get_by_pid — fine for now)
       proc->pending_signals |= signal_mask;
       kern_task_t *t = proc->blocked_task;
       if (t && t->state == TASK_STATE_BLOCKED && (t->signal_wait_mask & signal_mask)) {
           sched_unblock(t);
           proc->blocked_task = NULL;
           return true;
       }
       return false;
   }
   ```
   No more full-list scan for wake.

4. **Update `ipc_setup_send()`**: same pattern — unblock via `sched_unblock()`.

5. **`process_create()`**: initialize `blocked_task = NULL`.

### Phase 3 (Future): Revisit PID Lookup

`sched_get_by_pid()` is still O(n). If PID lookup becomes a bottleneck (unlikely before 100+ processes), add a simple hash table or array indexed by PID. Not urgent.

---

## 6. What Doesn't Change

- **`task_switch_asm`**: Identical. The context switch is unchanged.
- **CR3 switching logic**: Still in assembly, still skipped when same-process.
- **`process_exit()` / `ipc_process_cleanup()`**: Unchanged.
- **`sched_get_current_task()` / `sched_get_current_process()`**: Unchanged.
- **All WASM spawn paths**: `wasm_spawn.c` doesn't touch the ready list directly.
- **Timer handler**: Still calls `sched_run_next()` — it's just O(1) now.
- **`sched_get_task_list_head()`**: Still exists (used by `sched_get_by_pid` and the `procs` debug command).

---

## 7. Correctness Check: IPC Interactions

The current IPC code has two blocking patterns:

**Pattern 1 — Signal wait (ipc_signal_wait):**
```c
task->state = TASK_STATE_BLOCKED;
task->signal_wait_mask = mask;
sched_yield();                          // → sched_run_next()
// ... resumed after ipc_signal_send wakes us
```

**Pattern 2 — Setup wait (ipc_setup_wait):**
```c
task->state = TASK_STATE_BLOCKED;
sched_yield();                          // → sched_run_next()
// ... resumed after ipc_setup_send wakes us
```

Both set state manually then yield. With the ready queue:
- `sched_block_current()` would remove the task from `ready_head`, set BLOCKED, then call `sched_run_next()`.
- The waker calls `sched_unblock(task)` which inserts back into `ready_head`.
- This is strictly safer than the current pattern because the blocked task is **not in the ready list** — `sched_run_next()` can't accidentally stumble on it.

**Pattern 1 updated:**
```c
task->signal_wait_mask = mask;
proc->blocked_task = task;
sched_block_current();     // removes from ready_head, sets BLOCKED, yields
// ... resumed
proc->blocked_task = NULL;
```

---

## 8. Risk Assessment

| Risk | Likelihood | Mitigation |
|---|---|---|
| Ready list invariant broken (task in neither list) | Medium | Add `assert(task->state != READY || in_ready_list(task))` in debug builds |
| Race condition in list removal | Low | Single-core; we already use `save_irq_and_disable()` around list ops |
| Root task (TID 0) accidentally removed from ready list | Low | Root task never blocks; `sched_init()` inserts it into `ready_head` |
| `current_task` is blocked and we try to remove it from ready list (it's not there) | Low | Check `state` before removal; blocked tasks are already out |

---

## 9. Summary

| | Current | After Phase 1 | After Phase 2 |
|---|---|---|---|
| `sched_run_next()` | O(n) list walk | O(1) pointer advance | O(1) |
| `ipc_signal_send()` | O(n) list scan | O(n) PID lookup + O(n) scan | O(n) PID lookup + O(1) wake |
| Dead reaping | Inline during dispatch | Separate infrequent sweep | Same |
| Code complexity | Single list, mixed concerns | Two lists, clear separation | +1 pointer per process |
| LOC delta | — | ~+50 net | ~+70 net total |

The move to a ready queue is a **low-risk, high-clarity** improvement. It doesn't solve a pressing performance problem (there isn't one at current scale), but it **prevents future problems** and makes the scheduler significantly easier to reason about. The blocked pointer is a trivial addition that makes IPC wake O(1), which matters for any future IPC-heavy workload (pipes, window manager events, device driver notifications).
