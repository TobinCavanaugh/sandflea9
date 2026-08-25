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

// ── Display buffer tracking (per-process) ──────────────────────────────────

#define MAX_COMPOSITOR_WINDOWS 64
#define COMPOSITOR_EVENT_QUEUE_SIZE 128

// Per-process display state for compositor-managed windows.
typedef struct {
    bool  active;                     // slot in use
    i32   pid;                        // process owning this buffer
    u32   buffer_offset;              // offset in process's WASM linear memory
    u32   buf_w, buf_h;              // buffer dimensions
    IM3Runtime runtime;              // for accessing WASM memory
} compositor_child_t;

// ── Globals ────────────────────────────────────────────────────────────────

// Set to the PID of the compositor process (or -1 if none).
extern i32 g_compositor_pid;

// ── Host function forward declarations ─────────────────────────────────────
// Signature: const void * name(IM3Runtime, IM3ImportContext, uint64_t*, void*)
// These are linked into WASM modules via m3_LinkRawFunction.

extern const void * wasm_display_claim_compositor(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_get_resolution(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_present(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_present_rect(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_claim_buffer(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_display_blit_from_pid(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_input_poll_events(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_compositor_proc_spawn(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_compositor_proc_dequeue_spawn(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);
extern const void * wasm_compositor_proc_signal(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem);

// ── Kernel internal API ────────────────────────────────────────────────────

// Called by main loop to push events into the compositor's queue.
void compositor_push_event(u8 type, u32 d0, u32 d1, u32 d2);

// Called by process_exit to cleanup compositor child state.
void compositor_child_cleanup(i32 pid);

// Initialize compositor subsystem at boot.
void compositor_init(void);

#endif // SANDFLEA9_KERN_COMPOSITOR_H