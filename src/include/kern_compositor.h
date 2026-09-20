// kern_compositor.h — WASM compositor host interface.
//
// The compositor is a WASM program that imports "display" functions
// exclusive to it (claimCompositor, present, blitFromPid) and manages
// child windows via proc.spawn + IPC signals + shared display buffers.
//
// Non-compositor apps import only display.claimBuffer + display.present
// (simple mode: write pixels, kernel flips to hardware) or display.claimBuffer
// alone (compositor mode: compositor reads the buffer via blitFromPid).

#ifndef SANDFLEA9_KERN_COMPOSITOR_H
#define SANDFLEA9_KERN_COMPOSITOR_H

#include "dialect.h"
#include "kern_sched.h"
#include "m3_env.h"

// ── Limits & Capacities ───────────────────────────────────────────────────

#define MAX_COMPOSITOR_WINDOWS 128
#define COMPOSITOR_EVENT_QUEUE_SIZE 256
#define CLIENT_EVENT_QUEUE_SIZE 64

// ── Window Types & Flags ──────────────────────────────────────────────────

#define WIN_TYPE_TOPLEVEL 0
#define WIN_TYPE_DIALOG   1
#define WIN_TYPE_POPUP    2
#define WIN_TYPE_TOOLTIP  3

#define WIN_FLAG_NONE        0
#define WIN_FLAG_BORDERLESS  (1 << 0)
#define WIN_FLAG_FULLSCREEN  (1 << 1)
#define WIN_FLAG_FIXED_SIZE  (1 << 2)
#define WIN_FLAG_TRANSPARENT (1 << 3)

// ── Global Compositor Event Types (wm.pollEvents) ─────────────────────────

#define WM_EV_KEY_DOWN      0   // d0=keycode, d1=0, d2=0
#define WM_EV_KEY_UP        1   // d0=keycode, d1=0, d2=0
#define WM_EV_MOUSE_MOVE    2   // d0=dx, d1=dy, d2=0
#define WM_EV_MOUSE_BTN     3   // d0=btn (1=left,2=right,4=mid), d1=down(1)/up(0), d2=0
#define WM_EV_MOUSE_WHEEL   4   // d0=dx, d1=dy, d2=0
#define WM_EV_WIN_CREATED   10  // d0=handle, d1=pid, d2=(parent_handle << 16) | type
#define WM_EV_WIN_DESTROYED 11  // d0=handle, d1=pid, d2=0
#define WM_EV_WIN_COMMITTED 12  // d0=handle, d1=(dirty_x << 16) | dirty_y, d2=(dirty_w << 16) | dirty_h
#define WM_EV_WIN_TITLE     13  // d0=handle, d1=pid, d2=0
#define WM_EV_WIN_RESIZE    14  // d0=handle, d1=pid, d2=(w << 16) | h

// ── Client Window Event Types (winput.poll / window.pollEvents) ───────────

#define CLIENT_EV_KEY_DOWN    1
#define CLIENT_EV_KEY_UP      2
#define CLIENT_EV_MOUSE_MOVE  3
#define CLIENT_EV_MOUSE_DOWN  4
#define CLIENT_EV_MOUSE_UP    5
#define CLIENT_EV_MOUSE_WHEEL 6
#define CLIENT_EV_RESIZE      7
#define CLIENT_EV_FOCUS       8
#define CLIENT_EV_BLUR        9
#define CLIENT_EV_CLOSE_REQ   10

// 16-byte client event struct
typedef struct {
    u16 type;
    u16 flags;
    i32 win;
    u32 data0;
    u32 data1;
} client_event_t;

// Window Info metadata struct (written to WASM memory by wm.getWindowInfo)
typedef struct {
    i32  handle;
    i32  pid;
    i32  parent_handle;
    u32  win_type;
    u32  flags;
    u32  buf_w;
    u32  buf_h;
    u32  stride;
    char title[64];
} kern_window_info_t;

// Per-window tracking in kernel
typedef struct {
    bool           active;
    i32            handle;             // (generation << 16) | index
    i32            pid;                // owning process
    i32            parent_handle;
    u32            win_type;
    u32            flags;
    char           title[64];
    u32            buffer_offset;      // linear memory offset
    u32            buf_w, buf_h;      // buffer dimensions
    u32            stride;             // stride in bytes
    IM3Runtime     runtime;            // client WASM runtime
    
    // Per-window event queue
    client_event_t event_buf[CLIENT_EVENT_QUEUE_SIZE];
    volatile u32   event_read;
    volatile u32   event_write;
} kern_window_t;

// Backward-compat alias
typedef kern_window_t compositor_child_t;

// ── Globals ────────────────────────────────────────────────────────────────

extern i32 g_compositor_pid;
extern u32 g_compositor_session_id;

// ── Host function forward declarations ─────────────────────────────────────
// Linked into WASM modules via m3_LinkRawFunction.

// display.*
extern const void * wasm_display_claim_compositor(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_get_resolution(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_present(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_present_rect(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_claim_buffer(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_fill_rect(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_copy_buffer(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_blit_from_pid(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// input.* (global events)
extern const void * wasm_input_poll_events(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// wm.* (compositor privileged)
extern const void * wasm_wm_poll_events(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_wm_blit_surface(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_wm_route_input(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_wm_get_window_info(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// window.* (client)
extern const void * wasm_window_create(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_window_destroy(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_window_set_title(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_window_set_size(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// surface.* (client)
extern const void * wasm_surface_attach(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_surface_commit(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_surface_commit_rect(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// winput.* (client)
extern const void * wasm_winput_poll(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// proc.* (compositor / process control)
extern const void * wasm_compositor_proc_spawn(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_compositor_proc_dequeue_spawn(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_compositor_proc_signal(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// ── Kernel internal API ────────────────────────────────────────────────────

void compositor_push_event(u8 type, u32 d0, u32 d1, u32 d2);
void compositor_child_cleanup(i32 pid);
void compositor_init(void);
bool compositor_is_active(void);

#endif // SANDFLEA9_KERN_COMPOSITOR_H