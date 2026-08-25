// kern_compositor.c — WASM compositor host implementation.
//
// Provides the kernel-side of the compositor protocol:
//   display.* — framebuffer access (claimCompositor, present, claimBuffer, blitFromPid)
//   input.*   — hardware input event delivery
//   proc.*    — child process lifecycle

#include "../include/kern_compositor.h"
#include "../include/kern_screen.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_ipc.h"
#include "../include/kern_keyboard.h"
#include "../include/wasm_spawn.h"

#include "wasm3-0.5.0/source/m3_env.h"

// ── Module-level state ─────────────────────────────────────────────────────

i32 g_compositor_pid = -1;

// Compositor runtime — set by claimCompositor, used by present/blitFromPid.
static IM3Runtime g_compositor_runtime = NULL;

// Compositor's own display buffer offset (set by claimBuffer when compositor calls it).
static u32 g_compositor_buffer_offset = 0;

// ── Deferred spawn ────────────────────────────────────────────────────────
//
// proc.spawn just stores the request; proc.dequeueSpawn does the actual
// wasm_spawn(). This keeps host functions shallow (no deep call chains
// inside wasm3's interpreter recursion) and avoids stack overflow.
//
// The WM calls:
//   1. proc.spawn(path, argc, argv)  → returns 0 (accepted) or -1 (full)
//   2. proc.dequeueSpawn()           → returns PID or -1 (nothing pending)

#define SPAWN_QUEUE_SIZE 8

typedef struct {
    char path[128];
    i32  result_pid;   // set by dequeueSpawn before signaling
} spawn_request_t;

static spawn_request_t g_spawn_queue[SPAWN_QUEUE_SIZE];
static volatile u32    g_spawn_head = 0;   // WM writes here
static volatile u32    g_spawn_tail = 0;   // dequeueSpawn reads here

// Display buffer tracking: one entry per compositor child.
static compositor_child_t g_children[MAX_COMPOSITOR_WINDOWS];
static u32 g_child_count = 0;

// Input event ring buffer for the compositor.
// Each event is 4 u32s: type, data0, data1, data2.
#define EV_SLOT_TYPE  0
#define EV_SLOT_DATA0 1
#define EV_SLOT_DATA1 2
#define EV_SLOT_DATA2 3
#define EV_SLOT_SIZE  4
static u32 g_event_buf[COMPOSITOR_EVENT_QUEUE_SIZE * EV_SLOT_SIZE];
static volatile u32 g_event_read  = 0;
static volatile u32 g_event_write = 0;

// ── Helpers ────────────────────────────────────────────────────────────────

// Find child slot by PID, or -1 if not found.
static i32 compositor_find_child(i32 pid) {
    for (u32 i = 0; i < g_child_count; i++) {
        if (g_children[i].active && g_children[i].pid == pid)
            return (i32)i;
    }
    return -1;
}

// ── Public API ─────────────────────────────────────────────────────────────

void compositor_init(void) {
    g_compositor_pid = -1;
    g_compositor_runtime = NULL;
    g_compositor_buffer_offset = 0;
    g_child_count = 0;
    g_event_read = 0;
    g_event_write = 0;
    g_spawn_head = 0;
    g_spawn_tail = 0;
    mem_set((u8*)g_children, 0, sizeof(g_children));
    mem_set((u8*)g_event_buf, 0, sizeof(g_event_buf));
    mem_set((u8*)g_spawn_queue, 0, sizeof(g_spawn_queue));
}

void compositor_push_event(u8 type, u32 d0, u32 d1, u32 d2) {
    u32 next = (g_event_write + 1) % COMPOSITOR_EVENT_QUEUE_SIZE;
    if (next == g_event_read) return;  // drop on overflow
    u32 idx = g_event_write * EV_SLOT_SIZE;
    g_event_buf[idx + EV_SLOT_TYPE]  = type;
    g_event_buf[idx + EV_SLOT_DATA0] = d0;
    g_event_buf[idx + EV_SLOT_DATA1] = d1;
    g_event_buf[idx + EV_SLOT_DATA2] = d2;
    g_event_write = next;
}

void compositor_child_cleanup(i32 pid) {
    // If the compositor itself is dying, reset global state.
    if (pid == g_compositor_pid) {
        g_compositor_pid = -1;
        g_compositor_runtime = NULL;
        g_compositor_buffer_offset = 0;
        g_event_read = g_event_write;  // drain events
    }

    i32 idx = compositor_find_child(pid);
    if (idx < 0) return;
    g_children[idx].active = false;
    // Compact the array.
    for (u32 i = (u32)idx; i + 1 < g_child_count; i++) {
        g_children[i] = g_children[i + 1];
    }
    g_child_count--;
}

// ── display.claimCompositor ────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_claim_compositor) {
    m3ApiReturnType(i32)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    if (g_compositor_pid != -1 && g_compositor_pid != proc->pid) {
        restore_irq(irq);
        m3ApiReturn(-1);
    }
    g_compositor_pid = proc->pid;
    g_compositor_runtime = runtime;
    restore_irq(irq);

    m3ApiReturn(0);
}

// ── display.getResolution ──────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_get_resolution) {
    m3ApiReturnType(i32)

    display_t *disp = screen_current_display();
    if (!disp) { m3ApiReturn(0); }

    u32 packed = ((u32)disp->surface.height << 16) | (u32)disp->surface.width;
    m3ApiReturn((i32)packed);
}

// ── display.present ────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_present) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, offset)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    i32 comp_pid = g_compositor_pid;
    restore_irq(irq);

    if (comp_pid != -1 && proc->pid != comp_pid) {
        // Compositor is running, but caller isn't the compositor.
        // The compositor will read this app's buffer and flip.
        // Mark the child as dirty so the compositor knows to re-blit.
        m3ApiReturn(0);
    }

    // No compositor OR caller IS the compositor: blit to hardware.
    display_t *disp = screen_current_display();
    if (!disp || !disp->trueAddress) { m3ApiReturn(-2); }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) { m3ApiReturn(-3); }

    u32 screen_bytes = (u32)(disp->surface.pitch * disp->surface.height);
    if (offset > mem_size || mem_size - offset < screen_bytes) { m3ApiReturn(-4); }

    mem_copy(disp->trueAddress, mem + offset, screen_bytes);
    m3ApiReturn(0);
}

// ── display.claimBuffer ────────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_claim_buffer) {
    m3ApiReturnType(i32)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    display_t *disp = screen_current_display();
    if (!disp) { m3ApiReturn(-1); }

    u32 screen_bytes = (u32)(disp->surface.pitch * disp->surface.height);
    u32 pages_needed = (screen_bytes + 65535) / 65536;   // 64KB WASM pages

    // m3_GetMemorySize returns BYTES (m3_env.c line 451: length = numPageBytes)
    u32 cur_bytes = m3_GetMemorySize(runtime);
    u32 offset = cur_bytes;                     // place after existing memory
    u32 cur_pages = cur_bytes / 65536;          // for ResizeMemory (takes pages)

    M3Result r = ResizeMemory(runtime, cur_pages + pages_needed);
    if (r) {
        m3ApiReturn(-1);
    }

    // Track the buffer. Compositor gets its own offset, children get tracked
    // in g_children for blitFromPid.
    if (g_compositor_pid != -1) {
        u64 irq = save_irq_and_disable();
        if (proc->pid == g_compositor_pid) {
            // Compositor itself: record buffer offset for blitFromPid destination.
            g_compositor_buffer_offset = offset;
        } else if (g_child_count < MAX_COMPOSITOR_WINDOWS) {
            compositor_child_t *c = &g_children[g_child_count++];
            c->active = true;
            c->pid = proc->pid;
            c->buffer_offset = offset;
            c->buf_w = (u32)disp->surface.width;
            c->buf_h = (u32)disp->surface.height;
            c->runtime = runtime;
        }
        restore_irq(irq);
    }

    m3ApiReturn((i32)offset);
}

// ── display.blitFromPid ────────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_blit_from_pid) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, pid)
    m3ApiGetArg(i32, src_x)
    m3ApiGetArg(i32, src_y)
    m3ApiGetArg(i32, dst_x)
    m3ApiGetArg(i32, dst_y)
    m3ApiGetArg(i32, blit_w)
    m3ApiGetArg(i32, blit_h)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

    i32 idx = compositor_find_child(pid);
    if (idx < 0) { m3ApiReturn(-2); }

    compositor_child_t *child = &g_children[idx];
    if (!child->runtime) { m3ApiReturn(-3); }

    // Compositor's destination buffer — use the tracked offset.
    u32 compositor_offset = g_compositor_buffer_offset;
    IM3Runtime compositor_rt = g_compositor_runtime;
    if (!compositor_rt || compositor_offset == 0) { m3ApiReturn(-4); }

    u32 dst_mem_size = 0;
    u8 *dst_mem = m3_GetMemory(compositor_rt, &dst_mem_size, 0);
    if (!dst_mem) { m3ApiReturn(-5); }

    display_t *disp = screen_current_display();
    if (!disp) { m3ApiReturn(-5); }
    u32 dst_stride = (u32)disp->surface.pitch;

    u32 src_mem_size = 0;
    u8 *src_mem = m3_GetMemory(child->runtime, &src_mem_size, 0);
    if (!src_mem) { m3ApiReturn(-6); }

    u32 src_stride = child->buf_w * 4;
    u32 src_off = child->buffer_offset;

    // Clamp source rectangle to child buffer bounds.
    if (src_x < 0) src_x = 0;
    if (src_y < 0) src_y = 0;
    if (src_x + blit_w > (i32)child->buf_w) blit_w = (i32)child->buf_w - src_x;
    if (src_y + blit_h > (i32)child->buf_h) blit_h = (i32)child->buf_h - src_y;
    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

    // Clamp dest rectangle to compositor buffer bounds.
    u32 dst_max_x = dst_stride / 4;
    u32 dst_max_y = (dst_mem_size - compositor_offset) / dst_stride;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    if ((u32)(dst_x + blit_w) > dst_max_x) blit_w = (i32)dst_max_x - dst_x;
    if ((u32)(dst_y + blit_h) > dst_max_y) blit_h = (i32)dst_max_y - dst_y;
    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

    // Row-by-row copy.
    u32 row_bytes = (u32)blit_w * 4;
    for (i32 y = 0; y < blit_h; y++) {
        u32 src_row_off = src_off + ((u32)(src_y + y) * src_stride) + (u32)src_x * 4;
        u32 dst_row_off = compositor_offset + ((u32)(dst_y + y) * dst_stride) + (u32)dst_x * 4;
        if (src_row_off <= src_mem_size && src_mem_size - src_row_off >= row_bytes &&
            dst_row_off <= dst_mem_size && dst_mem_size - dst_row_off >= row_bytes) {
            mem_copy(dst_mem + dst_row_off, src_mem + src_row_off, row_bytes);
        }
    }

    m3ApiReturn(0);
}

// ── input.pollEvents ───────────────────────────────────────────────────────

m3ApiRawFunction(wasm_input_poll_events) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, buf_offset)
    m3ApiGetArg(i32, max_events)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) { m3ApiReturn(0); }

    u32 events_copied = 0;
    u64 irq = save_irq_and_disable();

    while (g_event_read != g_event_write && events_copied < (u32)max_events) {
        u32 src_idx = g_event_read * EV_SLOT_SIZE;
        u32 ev_offset = buf_offset + events_copied * 4 * EV_SLOT_SIZE;
        if (ev_offset > mem_size || mem_size - ev_offset < 16) break;

        // Copy as 4 consecutive u32s: type, data0, data1, data2
        ((u32*)(mem + ev_offset))[0] = g_event_buf[src_idx + EV_SLOT_TYPE];
        ((u32*)(mem + ev_offset))[1] = g_event_buf[src_idx + EV_SLOT_DATA0];
        ((u32*)(mem + ev_offset))[2] = g_event_buf[src_idx + EV_SLOT_DATA1];
        ((u32*)(mem + ev_offset))[3] = g_event_buf[src_idx + EV_SLOT_DATA2];

        g_event_read = (g_event_read + 1) % COMPOSITOR_EVENT_QUEUE_SIZE;
        events_copied++;
    }

    restore_irq(irq);
    m3ApiReturn((i32)events_copied);
}

// ── proc.spawn ─────────────────────────────────────────────────────────────
// Stores a spawn request in the queue. The WM must call proc.dequeueSpawn()
// to actually perform the spawn (outside the interpreter's deepest recursion).

m3ApiRawFunction(wasm_compositor_proc_spawn) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, path_offset)
    m3ApiGetArg(i32, argc)
    m3ApiGetArg(u32, argv_offset)

    (void)argc; (void)argv_offset;   // ignored for now; WM spawns hello.wasm

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    // Bounds-check: read path string from WASM linear memory.
    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || path_offset >= mem_size) { m3ApiReturn(-2); }

    // Enqueue the spawn request.
    u64 irq = save_irq_and_disable();
    u32 next = (g_spawn_head + 1) % SPAWN_QUEUE_SIZE;
    if (next == g_spawn_tail) {
        restore_irq(irq);
        m3ApiReturn(-3);  // queue full
    }
    spawn_request_t *req = &g_spawn_queue[g_spawn_head];
    // Copy path (truncate to 127 chars max + null).
    u32 path_len = 0;
    while (path_offset + path_len < mem_size && path_len < 127 && mem[path_offset + path_len])
        path_len++;
    for (u32 i = 0; i < path_len; i++)
        req->path[i] = (char)mem[path_offset + i];
    req->path[path_len] = '\0';
    req->result_pid = -1;
    g_spawn_head = next;
    restore_irq(irq);

    m3ApiReturn(0);
}

// ── proc.dequeueSpawn ─────────────────────────────────────────────────────
// Actually performs a pending spawn. Called from the WM's event loop.
// Returns the new PID, or -1 if nothing pending.

m3ApiRawFunction(wasm_compositor_proc_dequeue_spawn) {
    m3ApiReturnType(i32)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    if (g_spawn_tail == g_spawn_head) {
        restore_irq(irq);
        m3ApiReturn(-1);  // nothing pending
    }
    spawn_request_t *req = &g_spawn_queue[g_spawn_tail];
    g_spawn_tail = (g_spawn_tail + 1) % SPAWN_QUEUE_SIZE;
    restore_irq(irq);

    // Perform the actual spawn.  This is still a host function, but it's
    // called from the WM's event loop where wasm3 recursion depth is minimal.
    wasm_spawn_opts_t opts = {0};
    opts.path = req->path;
    opts.wasi_argv = false;
    opts.foreground = false;
    i32 pid = wasm_spawn(&opts);
    req->result_pid = pid;
    m3ApiReturn(pid);
}


// ── proc.signal ────────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_compositor_proc_signal) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, pid)
    m3ApiGetArg(i32, event)
    m3ApiGetArg(i32, data)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    // Pack event + data into an IPC signal.
    // Bits 0-7: event type, bits 8-31: data.
    u32 mask = ((u32)event & 0xFF) | ((u32)data << 8);
    bool ok = ipc_signal_send(pid, mask);
    m3ApiReturn(ok ? 0 : -1);
}