// kern_compositor.c — WASM compositor & Window Manager host implementation.
//
// Provides the kernel-side of the window manager & compositor protocol:
//   display.* — hardware framebuffer presentation & legacy buffer claims
//   input.*   — hardware input event delivery (legacy alias)
//   wm.*      — privileged window manager controls (event poll, surface blit, input routing)
//   window.*  — client window lifecycle (create, destroy, setTitle, setSize)
//   surface.* — client frame presentation & dirty rectangle commits
//   winput.*  — client input event queue consumption
//   proc.*    — child process lifecycle & spawning

#include "../include/kern_compositor.h"
#include "../include/kern_screen.h"
#include "../include/kern_terminal.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_ipc.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_mouse.h"
#include "../include/wasm_spawn.h"

#include "wasm3-0.5.0/source/m3_env.h"

// ── Module-level state ─────────────────────────────────────────────────────

i32 g_compositor_pid = -1;
u32 g_compositor_session_id = 0;

bool compositor_is_active(void) {
    if (g_compositor_pid == -1) return false;
    if (!active_session) return true;
    return (active_session->id == g_compositor_session_id);
}

// Compositor runtime — set by claimCompositor, used by present/blit.
static IM3Runtime g_compositor_runtime = NULL;

// Compositor's own display buffer offset (set by claimBuffer when compositor calls it).
static u32 g_compositor_buffer_offset = 0;

// ── Window Management Table ────────────────────────────────────────────────

static kern_window_t g_windows[MAX_COMPOSITOR_WINDOWS];
static u16 g_window_generation = 1;

static i32 make_window_handle(u16 index) {
    if (++g_window_generation == 0) g_window_generation = 1;
    return (i32)(((u32)g_window_generation << 16) | (u32)(index & 0xFFFF));
}

static i32 compositor_find_window_by_handle(i32 handle) {
    if (handle <= 0) return -1;
    u16 idx = (u16)(handle & 0xFFFF);
    if (idx >= MAX_COMPOSITOR_WINDOWS) return -1;
    if (!g_windows[idx].active || g_windows[idx].handle != handle) return -1;
    return (i32)idx;
}

static i32 compositor_find_free_window_slot(void) {
    for (u32 i = 0; i < MAX_COMPOSITOR_WINDOWS; i++) {
        if (!g_windows[i].active) return (i32)i;
    }
    return -1;
}

static i32 compositor_find_window_by_pid(i32 pid) {
    for (u32 i = 0; i < MAX_COMPOSITOR_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].pid == pid)
            return (i32)i;
    }
    return -1;
}

static void comp_str_copy(char *dst, const char *src, u32 max_len) {
    if (!dst || max_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    u32 i = 0;
    while (i + 1 < max_len && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// ── Deferred spawn ────────────────────────────────────────────────────────

#define SPAWN_QUEUE_SIZE 8

typedef struct {
    char path[128];
    i32  arg0;
    i32  result_pid;
} spawn_request_t;

static spawn_request_t g_spawn_queue[SPAWN_QUEUE_SIZE];
static volatile u32    g_spawn_head = 0;
static volatile u32    g_spawn_tail = 0;

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

// ── Public API ─────────────────────────────────────────────────────────────

void compositor_init(void) {
    g_compositor_pid = -1;
    g_compositor_session_id = 0;
    g_compositor_runtime = NULL;
    g_compositor_buffer_offset = 0;
    g_event_read = 0;
    g_event_write = 0;
    g_spawn_head = 0;
    g_spawn_tail = 0;
    g_window_generation = 1;
    mem_set((u8*)g_windows, 0, sizeof(g_windows));
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
        g_compositor_session_id = 0;
        g_compositor_runtime = NULL;
        g_compositor_buffer_offset = 0;
        g_event_read = g_event_write;  // drain events
    }

    // Cleanup all windows registered to this PID and notify WM
    for (u32 i = 0; i < MAX_COMPOSITOR_WINDOWS; i++) {
        if (g_windows[i].active && g_windows[i].pid == pid) {
            i32 handle = g_windows[i].handle;
            g_windows[i].active = false;
            if (g_compositor_pid != -1 && pid != g_compositor_pid) {
                compositor_push_event(WM_EV_WIN_DESTROYED, (u32)handle, (u32)pid, 0);
            }
        }
    }
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
    g_compositor_session_id = proc->terminal_session ? ((term_session_t*)proc->terminal_session)->id : (active_session ? active_session->id : 0);
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
        // Find window for this process
        i32 win_idx = compositor_find_window_by_pid(proc->pid);
        i32 handle = (win_idx >= 0) ? g_windows[win_idx].handle : proc->pid;
        compositor_push_event(WM_EV_WIN_COMMITTED, (u32)handle, (u32)proc->pid, 0);
        m3ApiReturn(0);
    }

    if (comp_pid != -1 && proc->pid == comp_pid && !compositor_is_active()) {
        m3ApiReturn(0);
    }

    // No compositor OR caller IS active compositor: blit to hardware.
    display_t *disp = screen_current_display();
    if (!disp || !disp->trueAddress) { m3ApiReturn(-2); }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) { m3ApiReturn(-3); }

    u32 screen_w = (u32)disp->surface.width;
    u32 screen_h = (u32)disp->surface.height;
    u32 pitch    = (u32)disp->surface.pitch;
    u32 row_bytes = screen_w * 4;

    u8 *dst_base = (u8 *)disp->trueAddress;
    u8 *src_base = mem + offset;

    if (pitch == row_bytes) {
        u32 screen_bytes = pitch * screen_h;
        if (offset > mem_size || mem_size - offset < screen_bytes) { m3ApiReturn(-4); }
        mem_copy(dst_base, src_base, screen_bytes);
    } else {
        for (u32 r = 0; r < screen_h; r++) {
            u32 dst_off = r * pitch;
            u32 src_off = r * row_bytes;
            if (offset + src_off + row_bytes <= mem_size) {
                mem_copy(dst_base + dst_off, src_base + src_off, row_bytes);
            }
        }
    }

    m3ApiReturn(0);
}

// ── display.presentRect ────────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_present_rect) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, offset)
    m3ApiGetArg(i32, x)
    m3ApiGetArg(i32, y)
    m3ApiGetArg(i32, w)
    m3ApiGetArg(i32, h)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    i32 comp_pid = g_compositor_pid;
    restore_irq(irq);

    if (comp_pid != -1 && proc->pid != comp_pid) {
        i32 win_idx = compositor_find_window_by_pid(proc->pid);
        i32 handle = (win_idx >= 0) ? g_windows[win_idx].handle : proc->pid;
        u32 d1 = ((u32)(x & 0xFFFF) << 16) | (u32)(y & 0xFFFF);
        u32 d2 = ((u32)(w & 0xFFFF) << 16) | (u32)(h & 0xFFFF);
        compositor_push_event(WM_EV_WIN_COMMITTED, (u32)handle, d1, d2);
        m3ApiReturn(0);
    }

    if (comp_pid != -1 && proc->pid == comp_pid && !compositor_is_active()) {
        m3ApiReturn(0);
    }

    display_t *disp = screen_current_display();
    if (!disp || !disp->trueAddress) { m3ApiReturn(-2); }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) { m3ApiReturn(-3); }

    u32 screen_w = (u32)disp->surface.width;
    u32 screen_h = (u32)disp->surface.height;
    u32 pitch    = (u32)disp->surface.pitch;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (i32)screen_w) w = (i32)screen_w - x;
    if (y + h > (i32)screen_h) h = (i32)screen_h - y;
    if (w <= 0 || h <= 0) { m3ApiReturn(0); }

    u32 row_bytes = (u32)w * 4;
    u8 *dst_base = (u8 *)disp->trueAddress;
    u8 *src_base = mem + offset;

    for (i32 r = 0; r < h; r++) {
        u32 row_y = (u32)(y + r);
        u32 dst_off = row_y * pitch + (u32)x * 4;
        u32 src_off = row_y * (screen_w * 4) + (u32)x * 4;
        if (offset + src_off + row_bytes <= mem_size) {
            mem_copy(dst_base + dst_off, src_base + src_off, row_bytes);
        }
    }

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
    u32 pages_needed = (screen_bytes + 65535) / 65536;

    u32 cur_bytes = m3_GetMemorySize(runtime);
    u32 offset = cur_bytes;
    u32 cur_pages = cur_bytes / 65536;

    M3Result r = ResizeMemory(runtime, cur_pages + pages_needed);
    if (r) {
        m3ApiReturn(-1);
    }

    if (g_compositor_pid != -1) {
        u64 irq = save_irq_and_disable();
        if (proc->pid == g_compositor_pid) {
            g_compositor_buffer_offset = offset;
        } else {
            i32 idx = compositor_find_window_by_pid(proc->pid);
            if (idx < 0) {
                idx = compositor_find_free_window_slot();
                if (idx >= 0) {
                    kern_window_t *w = &g_windows[idx];
                    w->active = true;
                    w->handle = make_window_handle((u16)idx);
                    w->pid = proc->pid;
                    w->parent_handle = 0;
                    w->win_type = WIN_TYPE_TOPLEVEL;
                    w->flags = WIN_FLAG_NONE;
                    w->buffer_offset = offset;
                    w->buf_w = (u32)disp->surface.width;
                    w->buf_h = (u32)disp->surface.height;
                    w->stride = (u32)disp->surface.width * 4;
                    w->runtime = runtime;
                    w->event_read = 0;
                    w->event_write = 0;
                    comp_str_copy(w->title, "App Window", sizeof(w->title));
                    compositor_push_event(WM_EV_WIN_CREATED, (u32)w->handle, (u32)proc->pid, (u32)WIN_TYPE_TOPLEVEL);
                }
            } else {
                g_windows[idx].buffer_offset = offset;
                g_windows[idx].runtime = runtime;
                compositor_push_event(WM_EV_WIN_COMMITTED, (u32)g_windows[idx].handle, 0, 0);
            }
        }
        restore_irq(irq);
    }

    m3ApiReturn((i32)offset);
}

// ── display.fillRect ───────────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_fill_rect) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, offset)
    m3ApiGetArg(i32, x)
    m3ApiGetArg(i32, y)
    m3ApiGetArg(i32, w)
    m3ApiGetArg(i32, h)
    m3ApiGetArg(u32, color)

    display_t *disp = screen_current_display();
    if (!disp) m3ApiReturn(-1);

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) m3ApiReturn(-1);

    i32 screen_w = (i32)disp->surface.width;
    i32 screen_h = (i32)disp->surface.height;

    int x1 = x + w; if (x1 > screen_w) x1 = screen_w;
    int y1 = y + h; if (y1 > screen_h) y1 = screen_h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 <= x || y1 <= y) m3ApiReturn(0);

    u32 *fb = (u32*)(mem + offset);
    int span = x1 - x;
    u32 row_words = (u32)screen_w;

    u64 last_pixel_off = (u64)(y1 - 1) * row_words + (x1 - 1);
    if (offset + (last_pixel_off + 1) * 4 > mem_size) m3ApiReturn(-1);

    u64 col64 = ((u64)color << 32) | (u64)color;

    for (int py = y; py < y1; py++) {
        u32 *row = &fb[py * row_words + x];
        int px = 0;
        while (px + 2 <= span) {
            *(u64*)(row + px) = col64;
            px += 2;
        }
        if (px < span) {
            row[px] = color;
        }
    }

    m3ApiReturn(0);
}

// ── display.copyBuffer ────────────────────────────────────────────────────

m3ApiRawFunction(wasm_display_copy_buffer) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, dst_offset)
    m3ApiGetArg(u32, src_offset)

    display_t *disp = screen_current_display();
    if (!disp) m3ApiReturn(-1);

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) m3ApiReturn(-1);

    u32 total_bytes = (u32)(disp->surface.width * disp->surface.height * 4);
    if (dst_offset + total_bytes > mem_size || src_offset + total_bytes > mem_size) {
        m3ApiReturn(-1);
    }

    mem_copy(mem + dst_offset, mem + src_offset, total_bytes);
    m3ApiReturn(0);
}

// ── display.blitFromPid (Legacy compatibility) ─────────────────────────────

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

    i32 idx = compositor_find_window_by_pid(pid);
    if (idx < 0) { m3ApiReturn(-2); }

    kern_window_t *child = &g_windows[idx];
    if (!child->runtime) { m3ApiReturn(-3); }

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

    u32 src_stride = child->stride ? child->stride : (child->buf_w * 4);
    u32 src_off = child->buffer_offset;

    if (src_x < 0) src_x = 0;
    if (src_y < 0) src_y = 0;
    if (src_x + blit_w > (i32)child->buf_w) blit_w = (i32)child->buf_w - src_x;
    if (src_y + blit_h > (i32)child->buf_h) blit_h = (i32)child->buf_h - src_y;
    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

    u32 dst_max_x = dst_stride / 4;
    u32 dst_max_y = (dst_mem_size - compositor_offset) / dst_stride;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    if ((u32)(dst_x + blit_w) > dst_max_x) blit_w = (i32)dst_max_x - dst_x;
    if ((u32)(dst_y + blit_h) > dst_max_y) blit_h = (i32)dst_max_y - dst_y;
    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

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

// ── wm.pollEvents / input.pollEvents ───────────────────────────────────────

static i32 poll_events_internal(IM3Runtime runtime, u32 buf_offset, i32 max_events) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { return -1; }

    if (compositor_is_active()) {
        mouse_event_t mev;
        while (mouse_eat_event(&mev)) {
            compositor_push_event(mev.type, (u32)(i32)mev.dx, (u32)(i32)mev.dy, 0);
        }

        u8 k = 0;
        while ((k = keyboard_eat_key())) {
            if (k >= KEY_F1 && k <= KEY_F4) {
                u32 target = k - KEY_F1;
                if (target < MAX_SESSIONS && (!active_session || target != active_session->id)) {
                    session_switch(target);
                    sched_idle_wake();
                }
                continue;
            }
            compositor_push_event(WM_EV_KEY_DOWN, k, 0, 0);
        }
    }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) { return 0; }

    u32 events_copied = 0;
    u64 irq = save_irq_and_disable();

    while (g_event_read != g_event_write && events_copied < (u32)max_events) {
        u32 src_idx = g_event_read * EV_SLOT_SIZE;
        u32 ev_offset = buf_offset + events_copied * 4 * EV_SLOT_SIZE;
        if (ev_offset > mem_size || mem_size - ev_offset < 16) break;

        ((u32*)(mem + ev_offset))[0] = g_event_buf[src_idx + EV_SLOT_TYPE];
        ((u32*)(mem + ev_offset))[1] = g_event_buf[src_idx + EV_SLOT_DATA0];
        ((u32*)(mem + ev_offset))[2] = g_event_buf[src_idx + EV_SLOT_DATA1];
        ((u32*)(mem + ev_offset))[3] = g_event_buf[src_idx + EV_SLOT_DATA2];

        g_event_read = (g_event_read + 1) % COMPOSITOR_EVENT_QUEUE_SIZE;
        events_copied++;
    }

    restore_irq(irq);

    if (events_copied == 0) {
        sched_idle_wake();
        sched_yield();
    }

    return (i32)events_copied;
}

m3ApiRawFunction(wasm_input_poll_events) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, buf_offset)
    m3ApiGetArg(i32, max_events)

    i32 count = poll_events_internal(runtime, buf_offset, max_events);
    m3ApiReturn(count);
}

m3ApiRawFunction(wasm_wm_poll_events) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, buf_offset)
    m3ApiGetArg(i32, max_events)

    i32 count = poll_events_internal(runtime, buf_offset, max_events);
    m3ApiReturn(count);
}

// ── wm.blitSurface ─────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_wm_blit_surface) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, win_handle)
    m3ApiGetArg(i32, src_x)
    m3ApiGetArg(i32, src_y)
    m3ApiGetArg(i32, dst_x)
    m3ApiGetArg(i32, dst_y)
    m3ApiGetArg(i32, blit_w)
    m3ApiGetArg(i32, blit_h)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }
    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

    i32 idx = compositor_find_window_by_handle(win_handle);
    if (idx < 0) { m3ApiReturn(-2); }

    kern_window_t *child = &g_windows[idx];
    if (!child->runtime) { m3ApiReturn(-3); }

    u32 compositor_offset = g_compositor_buffer_offset;
    IM3Runtime compositor_rt = g_compositor_runtime ? g_compositor_runtime : runtime;
    if (!compositor_rt) { m3ApiReturn(-4); }

    u32 dst_mem_size = 0;
    u8 *dst_mem = m3_GetMemory(compositor_rt, &dst_mem_size, 0);
    if (!dst_mem) { m3ApiReturn(-5); }

    display_t *disp = screen_current_display();
    if (!disp) { m3ApiReturn(-5); }
    u32 dst_stride = (u32)disp->surface.pitch;

    u32 src_mem_size = 0;
    u8 *src_mem = m3_GetMemory(child->runtime, &src_mem_size, 0);
    if (!src_mem) { m3ApiReturn(-6); }

    u32 src_stride = child->stride ? child->stride : (child->buf_w * 4);
    u32 src_off = child->buffer_offset;

    if (src_x < 0) src_x = 0;
    if (src_y < 0) src_y = 0;
    if (src_x + blit_w > (i32)child->buf_w) blit_w = (i32)child->buf_w - src_x;
    if (src_y + blit_h > (i32)child->buf_h) blit_h = (i32)child->buf_h - src_y;
    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

    u32 dst_max_x = dst_stride / 4;
    u32 dst_max_y = (dst_mem_size - compositor_offset) / dst_stride;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    if ((u32)(dst_x + blit_w) > dst_max_x) blit_w = (i32)dst_max_x - dst_x;
    if ((u32)(dst_y + blit_h) > dst_max_y) blit_h = (i32)dst_max_y - dst_y;
    if (blit_w <= 0 || blit_h <= 0) { m3ApiReturn(0); }

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

// ── wm.routeInput ──────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_wm_route_input) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, win_handle)
    m3ApiGetArg(u32, event_type)
    m3ApiGetArg(u32, flags)
    m3ApiGetArg(u32, data0)
    m3ApiGetArg(u32, data1)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    i32 idx = compositor_find_window_by_handle(win_handle);
    if (idx < 0) { m3ApiReturn(-2); }

    kern_window_t *w = &g_windows[idx];
    u64 irq = save_irq_and_disable();

    u32 next = (w->event_write + 1) % CLIENT_EVENT_QUEUE_SIZE;
    if (next == w->event_read) {
        restore_irq(irq);
        m3ApiReturn(-3); // queue full
    }

    client_event_t *ev = &w->event_buf[w->event_write];
    ev->type = (u16)event_type;
    ev->flags = (u16)flags;
    ev->win = win_handle;
    ev->data0 = data0;
    ev->data1 = data1;
    w->event_write = next;

    restore_irq(irq);
    m3ApiReturn(0);
}

// ── wm.getWindowInfo ───────────────────────────────────────────────────────

m3ApiRawFunction(wasm_wm_get_window_info) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, win_handle)
    m3ApiGetArg(u32, out_info_ptr)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    i32 idx = compositor_find_window_by_handle(win_handle);
    if (idx < 0) { m3ApiReturn(-2); }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || out_info_ptr + sizeof(kern_window_info_t) > mem_size) {
        m3ApiReturn(-3);
    }

    kern_window_t *w = &g_windows[idx];
    kern_window_info_t *info = (kern_window_info_t*)(mem + out_info_ptr);
    info->handle = w->handle;
    info->pid = w->pid;
    info->parent_handle = w->parent_handle;
    info->win_type = w->win_type;
    info->flags = w->flags;
    info->buf_w = w->buf_w;
    info->buf_h = w->buf_h;
    info->stride = w->stride;
    comp_str_copy(info->title, w->title, sizeof(info->title));

    m3ApiReturn(0);
}

// ── window.create ──────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_window_create) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, parent_handle)
    m3ApiGetArg(u32, win_type)
    m3ApiGetArg(u32, title_ptr)
    m3ApiGetArg(u32, title_len)
    m3ApiGetArg(u32, w)
    m3ApiGetArg(u32, h)
    m3ApiGetArg(u32, flags)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    i32 idx = compositor_find_free_window_slot();
    if (idx < 0) {
        restore_irq(irq);
        m3ApiReturn(-2); // out of window handles
    }

    kern_window_t *win = &g_windows[idx];
    i32 handle = make_window_handle((u16)idx);
    win->active = true;
    win->handle = handle;
    win->pid = proc->pid;
    win->parent_handle = parent_handle;
    win->win_type = win_type;
    win->flags = flags;
    win->buf_w = w;
    win->buf_h = h;
    win->stride = w * 4;
    win->buffer_offset = 0;
    win->runtime = runtime;
    win->event_read = 0;
    win->event_write = 0;

    // Read title from linear memory
    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (mem && title_ptr < mem_size && title_len > 0) {
        u32 copy_len = (title_len < 63) ? title_len : 63;
        if (title_ptr + copy_len > mem_size) copy_len = mem_size - title_ptr;
        for (u32 i = 0; i < copy_len; i++) win->title[i] = (char)mem[title_ptr + i];
        win->title[copy_len] = '\0';
    } else {
        comp_str_copy(win->title, "Window", sizeof(win->title));
    }

    if (g_compositor_pid != -1) {
        u32 d2 = ((u32)(parent_handle & 0xFFFF) << 16) | (u32)(win_type & 0xFFFF);
        compositor_push_event(WM_EV_WIN_CREATED, (u32)handle, (u32)proc->pid, d2);
    }
    restore_irq(irq);

    m3ApiReturn(handle);
}

// ── window.destroy ─────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_window_destroy) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    i32 idx = compositor_find_window_by_handle(handle);
    if (idx < 0) {
        restore_irq(irq);
        m3ApiReturn(-2);
    }

    kern_window_t *win = &g_windows[idx];
    if (win->pid != proc->pid && proc->pid != g_compositor_pid) {
        restore_irq(irq);
        m3ApiReturn(-3); // not owner
    }

    win->active = false;
    if (g_compositor_pid != -1) {
        compositor_push_event(WM_EV_WIN_DESTROYED, (u32)handle, (u32)win->pid, 0);
    }
    restore_irq(irq);

    m3ApiReturn(0);
}

// ── window.setTitle ────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_window_set_title) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(u32, title_ptr)
    m3ApiGetArg(u32, title_len)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    i32 idx = compositor_find_window_by_handle(handle);
    if (idx < 0) { m3ApiReturn(-2); }

    kern_window_t *win = &g_windows[idx];
    if (win->pid != proc->pid && proc->pid != g_compositor_pid) { m3ApiReturn(-3); }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (mem && title_ptr < mem_size && title_len > 0) {
        u32 copy_len = (title_len < 63) ? title_len : 63;
        if (title_ptr + copy_len > mem_size) copy_len = mem_size - title_ptr;
        for (u32 i = 0; i < copy_len; i++) win->title[i] = (char)mem[title_ptr + i];
        win->title[copy_len] = '\0';
    }

    if (g_compositor_pid != -1) {
        compositor_push_event(WM_EV_WIN_TITLE, (u32)handle, (u32)proc->pid, 0);
    }

    m3ApiReturn(0);
}

// ── window.setSize ─────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_window_set_size) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(u32, w)
    m3ApiGetArg(u32, h)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    i32 idx = compositor_find_window_by_handle(handle);
    if (idx < 0) { m3ApiReturn(-2); }

    kern_window_t *win = &g_windows[idx];
    if (win->pid != proc->pid && proc->pid != g_compositor_pid) { m3ApiReturn(-3); }

    win->buf_w = w;
    win->buf_h = h;
    win->stride = w * 4;

    if (g_compositor_pid != -1) {
        u32 d2 = ((w & 0xFFFF) << 16) | (h & 0xFFFF);
        compositor_push_event(WM_EV_WIN_RESIZE, (u32)handle, (u32)proc->pid, d2);
    }

    m3ApiReturn(0);
}

// ── surface.attach ─────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_surface_attach) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(u32, buf_offset)
    m3ApiGetArg(u32, w)
    m3ApiGetArg(u32, h)
    m3ApiGetArg(u32, stride)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    i32 idx = compositor_find_window_by_handle(handle);
    if (idx < 0) { m3ApiReturn(-2); }

    kern_window_t *win = &g_windows[idx];
    if (win->pid != proc->pid && proc->pid != g_compositor_pid) { m3ApiReturn(-3); }

    win->buffer_offset = buf_offset;
    win->buf_w = w;
    win->buf_h = h;
    win->stride = (stride > 0) ? stride : (w * 4);
    win->runtime = runtime;

    m3ApiReturn(0);
}

// ── surface.commit ─────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_surface_commit) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    i32 idx = compositor_find_window_by_handle(handle);
    if (idx < 0) { m3ApiReturn(-2); }

    kern_window_t *win = &g_windows[idx];
    if (g_compositor_pid != -1) {
        compositor_push_event(WM_EV_WIN_COMMITTED, (u32)handle, 0, 0);
    }

    m3ApiReturn(0);
}

// ── surface.commitRect ─────────────────────────────────────────────────────

m3ApiRawFunction(wasm_surface_commit_rect) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, handle)
    m3ApiGetArg(i32, x)
    m3ApiGetArg(i32, y)
    m3ApiGetArg(i32, w)
    m3ApiGetArg(i32, h)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    i32 idx = compositor_find_window_by_handle(handle);
    if (idx < 0) { m3ApiReturn(-2); }

    if (g_compositor_pid != -1) {
        u32 d1 = ((u32)(x & 0xFFFF) << 16) | (u32)(y & 0xFFFF);
        u32 d2 = ((u32)(w & 0xFFFF) << 16) | (u32)(h & 0xFFFF);
        compositor_push_event(WM_EV_WIN_COMMITTED, (u32)handle, d1, d2);
    }

    m3ApiReturn(0);
}

// ── winput.poll ────────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_winput_poll) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, win_filter)
    m3ApiGetArg(u32, buf_offset)
    m3ApiGetArg(i32, max_events)

    kern_process_t *proc = sched_get_current_process();
    if (!proc) { m3ApiReturn(-1); }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || max_events <= 0) { m3ApiReturn(0); }

    u32 events_copied = 0;
    u64 irq = save_irq_and_disable();

    for (u32 i = 0; i < MAX_COMPOSITOR_WINDOWS && events_copied < (u32)max_events; i++) {
        kern_window_t *w = &g_windows[i];
        if (!w->active || w->pid != proc->pid) continue;
        if (win_filter > 0 && w->handle != win_filter) continue;

        while (w->event_read != w->event_write && events_copied < (u32)max_events) {
            u32 ev_off = buf_offset + events_copied * sizeof(client_event_t);
            if (ev_off + sizeof(client_event_t) > mem_size) break;

            client_event_t *dst = (client_event_t*)(mem + ev_off);
            client_event_t *src = &w->event_buf[w->event_read];
            *dst = *src;

            w->event_read = (w->event_read + 1) % CLIENT_EVENT_QUEUE_SIZE;
            events_copied++;
        }
    }

    restore_irq(irq);

    if (events_copied == 0) {
        sched_idle_wake();
        sched_yield();
    }

    m3ApiReturn((i32)events_copied);
}

// ── proc.spawn ─────────────────────────────────────────────────────────────

m3ApiRawFunction(wasm_compositor_proc_spawn) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, path_offset)
    m3ApiGetArg(i32, argc)
    m3ApiGetArg(u32, argv_offset)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    i32 arg0 = 0;
    if (argc > 0 && argv_offset != 0) {
        u32 mem_size = 0;
        u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
        if (mem && argv_offset + 4 <= mem_size) {
            u32 raw = 0;
            mem_copy((u8*)&raw, mem + argv_offset, 4);
            arg0 = (i32)raw;
        }
    }

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || path_offset >= mem_size) { m3ApiReturn(-2); }

    u64 irq = save_irq_and_disable();
    u32 next = (g_spawn_head + 1) % SPAWN_QUEUE_SIZE;
    if (next == g_spawn_tail) {
        restore_irq(irq);
        m3ApiReturn(-3);  // queue full
    }
    spawn_request_t *req = &g_spawn_queue[g_spawn_head];
    u32 path_len = 0;
    while (path_offset + path_len < mem_size && path_len < 127 && mem[path_offset + path_len])
        path_len++;
    for (u32 i = 0; i < path_len; i++)
        req->path[i] = (char)mem[path_offset + i];
    req->path[path_len] = '\0';
    req->arg0 = arg0;
    req->result_pid = -1;
    g_spawn_head = next;
    restore_irq(irq);

    m3ApiReturn(0);
}

// ── proc.dequeueSpawn ─────────────────────────────────────────────────────

m3ApiRawFunction(wasm_compositor_proc_dequeue_spawn) {
    m3ApiReturnType(i32)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || proc->pid != g_compositor_pid) { m3ApiReturn(-1); }

    u64 irq = save_irq_and_disable();
    if (g_spawn_tail == g_spawn_head) {
        restore_irq(irq);
        m3ApiReturn(-1);
    }
    spawn_request_t *req = &g_spawn_queue[g_spawn_tail];
    g_spawn_tail = (g_spawn_tail + 1) % SPAWN_QUEUE_SIZE;
    restore_irq(irq);

    wasm_spawn_opts_t opts = {0};
    opts.path = req->path;
    opts.wasi_argv = false;
    opts.foreground = false;

    char argbuf[16];
    char *argv[1] = { argbuf };
    if (req->arg0 > 0) {
        int v = req->arg0, n = 0;
        while (v > 0) { argbuf[n++] = '0' + (v % 10); v /= 10; }
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            char t = argbuf[i]; argbuf[i] = argbuf[j]; argbuf[j] = t;
        }
        argbuf[n] = 0;
        opts.argc = 1;
        opts.argv = argv;
    }
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

    u32 mask = ((u32)event & 0xFF) | ((u32)data << 8);
    bool ok = ipc_signal_send(pid, mask);
    m3ApiReturn(ok ? 0 : -1);
}