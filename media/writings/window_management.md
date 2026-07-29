# Window Management for sandfleaOS

**Status:** Design Exploration  
**Date:** July 2026  
**Author:** Tobin (via Buffy)

---

## 1. Executive Summary

sandfleaOS currently has a **single-display, take-over model** — one thing owns the framebuffer at a time. The kernel's shell renders text lines into a backbuffer, Doom blits directly to the real framebuffer, and there's no way to switch between them without killing the process. This document explores:

1. When to start building a window manager
2. What the architecture should look like
3. How to achieve zero-copy window drawing
4. A concrete first step: **F-key terminal switching** (kernel-level MVP)
5. The path to an ELF-based userspace compositor

---

## 2. Current State Assessment

### What We Have Now

| Component | Description | Limitations |
|-----------|-------------|-------------|
| `screen_draw()` | memcpy from `surface.address` → `trueAddress` | Full-screen copy every frame (~8MB for 1920x1080) |
| `screen_render_shell()` | Renders text lines + status bar + input line | Hardcoded layout, no windowing |
| `owns_framebuffer` flag | Doom bypasses the shell and draws directly | Binary take-over — terminal loses control |
| `display_t` | Surface + backbuffer + trueAddress per monitor | Only one active at a time (`current_display`) |
| `foreground_proc` | Routes keyboard to the foreground WASM process | No visual indication of which window owns input |
| Keyboard queue | Single global queue + foreground process queue | Keys go to whoever is foreground — no way to switch |

### The Core Problem

There is **no concept of windows** at any level. The entire screen is a single canvas that every consumer (terminal, Doom, future apps) fights over. The `owns_framebuffer`/`!owns_framebuffer` binary in `main.c:244-287` is the architecture:

```c
if (owns_framebuffer) {
    // Doom's thread owns the screen entirely — just yield CPU
    asm volatile("hlt");
} else {
    // Render shell, handle input...
}
```

### What's Actually Working in Our Favor

- **Double-buffering already exists** (`surface.address` → `trueAddress` copy)
- **Process isolation is in place** (separate CR3 per process, `kern_mem_region_t` tracking)
- **Scheduler is functional** (cooperative + timer-driven preemption at ~100Hz)
- **Display infrastructure supports multiple monitors** (`display_t` array from Limine)
- **`foreground_proc` concept already routes keyboard** — just needs to route display too
- **SSFN font at `font_width=8`, `font_height=16`** — we know exact pixel sizes

---

## 3. When to Start a Window Manager

### Thresholds: When to Add What

| When you can... | Add this | Complexity |
|-----------------|----------|------------|
| Boot Doom or a terminal (not both) | **(current state)** | — |
| Switch between Doom and terminal via F1/F2 | **F-key terminal switching** (kernel-level) | ★☆☆ |
| Two terminal sessions running simultaneously | **Multiple virtual terminals** | ★★☆ |
| See both Doom and a terminal on screen at once | **Compositor w/ shared framebuffer pages** | ★★★ |
| A userspace program manages window positions | **ELF compositor (userspace)** | ★★★★ |
| Windows can overlap, be dragged, resized | **Full window manager** | ★★★★★ |

### Recommendation: Start NOW with F-key switching

**Don't architect a full compositor yet.** The single most impactful thing you can build this week is **F-key virtual terminal switching** at the kernel level. Here's why:

1. **It solves the Doom vs Terminal problem immediately** — swap between them at runtime
2. **Zero-copy is trivial** — each virtual terminal (VT) owns its own backbuffer; switching means `memcpy` a different backbuffer → real framebuffer
3. **Builds the infrastructure** (VT tracking, per-VT backbuffers, keybinding dispatch) that a future compositor will reuse
4. **~200 lines of C** — no ELF loader changes, no new syscalls, no IPC

Wait on a full compositor until you have **3+ things that want the screen simultaneously** (Doom + 2 terminals + a debugger, for example). At that point, the shared-framebuffer-page approach (Section 5) becomes worth the complexity.

---

## 4. Phase 1: F-Key Virtual Terminal Switching (Kernel-Level, ~200 lines)

### 4.1 Design

Each "virtual terminal" (VT) is a **self-contained display state**:

```
typedef struct virt_terminal {
    u32         id;
    char        name[32];           // "tty1", "tty2", etc.
    
    // Display state
    draw_surface_t backbuffer;      // kmalloc'd framebuffer-sized buffer
    screen_text_row_t *text_root;   // terminal text lines (moved from globals)
    screen_text_row_t *text_tail;
    i32            text_scroll;
    
    // Input routing
    u8             fg_key_queue[FG_QUEUE_SIZE];
    volatile u32   fg_queue_read_ptr;
    volatile u32   fg_queue_write_ptr;
    kern_process_t *foreground_proc; // which process gets keyboard
    
    // Active indicator
    bool visible;                   // only one VT is visible at a time
} virt_terminal_t;
```

### 4.2 VT Lifecycle

```
Boot
  │
  ├── VT0 (kernel shell) ── spawned at boot, always exists
  │
  ├── VT1 (Doom) ────────── created when "doom" command runs
  │     │                     (or lazily when F2 is first pressed)
  │     └── owns: owns_framebuffer = true, draws to VT1's backbuffer
  │
  ├── VT2 (user shell) ──── created on F3 press (new login shell)
  │
  └── VT3..n ────────────── created on demand
```

### 4.3 Switching Mechanism

When the user presses F1–F4 (scancodes 0x3B–0x3E on PS/2):

```c
// In main loop or keyboard handler:
void handle_vt_switch(u8 scancode) {
    u32 vt_id = scancode - 0x3B;  // F1=0, F2=1, etc.
    
    if (vt_id >= MAX_VIRT_TERMINALS) return;
    
    // 1. Save current VT state (backbuffer already IS the current VT's backbuffer)
    //    Nothing to save — we draw to whichever VT is current.
    
    // 2. Kill any fullscreen app on the current VT (or just "minimize" it)
    if (current_vt->foreground_proc && current_vt->foreground_is_fullscreen) {
        // Suspend the process rather than killing it
        suspend_foreground_process(current_vt);
    }
    
    // 3. Switch active VT
    current_vt = &virt_terminals[vt_id];
    
    // 4. Update globals that terminal code references
    screen_text_root = current_vt->text_root;
    screen_text_tail = current_vt->text_tail;
    screen_text_scroll = current_vt->text_scroll;
    foreground_proc = current_vt->foreground_proc;
    fg_queue_read_ptr = current_vt->fg_queue_read_ptr;
    fg_queue_write_ptr = current_vt->fg_queue_write_ptr;
    memcpy(fg_key_queue, current_vt->fg_key_queue, FG_QUEUE_SIZE);
    owns_framebuffer = current_vt->foreground_has_fullscreen;
    
    // 5. Blit the VT's backbuffer to the real framebuffer
    //    (zero-copy if we page-flip — see Section 5)
    memcpy(current_display->trueAddress, 
           current_vt->backbuffer.address,
           current_vt->backbuffer.pitch * current_vt->backbuffer.height);
    
    // 6. Re-render if it's a text VT
    if (!owns_framebuffer) screen_render_shell();
}
```

### 4.4 Changes Required

| File | Change |
|------|--------|
| New: `src/include/kern_vt.h` | `virt_terminal_t` struct, `vt_init()`, `vt_switch()` |
| New: `src/kernel/kern_vt.c` | VT array management, backbuffer allocation, F-key dispatch |
| `src/kernel/main.c` | Replace `if (owns_framebuffer) { ... } else { ... }` with VT dispatch |
| `src/kernel/kern_screen.c` | Add `screen_blit(dst_surface, src_surface)` for VT→framebuffer copy |
| `src/kernel/wasm_spawn.c` | Associate spawned processes with current VT |
| `src/include/kern_keyboard.h` | Add `KEY_F1`..`KEY_F4` constants |
| `src/kernel/kern_keyboard.c` | Add F-key scancode mappings (0x3B-0x3E) |

### 4.5 Wait... what about Doom's backbuffer?

Currently Doom draws directly to the **real** framebuffer (`display_main->trueAddress`) in `kern_tests.c`:

```c
// kern_tests.c ~line 302
u32 *dst = (u32 *) disp->surface.address;  // ← Wait, this is the BACKBUFFER
u32 *src = doom_internal_buffer;
// Blits doom_internal_buffer to disp->surface.address (backbuffer)
```

Actually, looking more carefully at `kern_tests.c`:

```c
// Blit Doom framebuffer directly to the real hardware framebuffer
u32 *dst = (u32 *) disp->surface.address;
// ...
memcpy(dst + (dst_y_off + y) * dst_pitch_px + dst_x_off,
       src + y * src_pitch_px,
       ...);
```

**This blits to `surface.address` (the backbuffer), NOT to `trueAddress`.** So Doom is already writing to our backbuffer. The shell then calls `screen_draw()` which memcpy's surface→trueAddress. But when `owns_framebuffer` is true, `screen_render_shell()` is never called, so `screen_draw()` is never called. Meaning: Doom's pixels are in the backbuffer but **never get copied to the real framebuffer** except... wait, Doom's `drawFrame` host function in `kern_tests.c` actually writes to the backbuffer, and then... there must be something else that flips.

Let me check — looking at the Doom test flow in `kern_tests.c`:

Actually the blit to `disp->surface.address` would be visible without `screen_draw()` only if Limine configured the framebuffer to be write-combining or if the blit is happening to the real address. But `disp->surface.address` is a kmalloc'd buffer initialized in `screen_init()`:

```c
// kern_screen.c line 44
v->surface.address = kmalloc(v->surface.pitch * v->surface.height);
```

So Doom writes to the backbuffer, but without `screen_draw()` the pixels never reach the real framebuffer. That's a **bug in the current code** — or there's a missing `screen_draw()` call in the Doom render path. Let me check `kern_tests.c` for a `screen_draw()` call...

If there isn't one, Doom has been rendering to the void and what you see on screen is actually just the last shell render frozen. This means:
- VT switching is **even simpler** — we just need to manage backbuffers properly
- Doom's blit to `surface.address` IS correct — it writes to the VT's backbuffer
- We need a `screen_draw()` call after Doom's drawFrame

**Correction to the plan:** Doom should call `screen_draw()` after its blit, AND/OR the VT switch should call `screen_draw()` for the active VT on every timer tick. This is actually better — it means per-VT backbuffers work perfectly with the existing double-buffer architecture.

### 4.6 Keyboard Routing Per-VT

Each VT needs its own `fg_key_queue` and `foreground_proc`. When the user presses F2 to switch to the Doom VT:

1. Save the shell VT's `foreground_proc` (NULL for shell VT)
2. Restore Doom VT's `foreground_proc` (the Doom process)
3. Restore Doom VT's `fg_key_queue` contents
4. Now keyboard input flows to Doom, not the shell

On switch back to shell VT:
1. Save Doom VT's `foreground_proc` and keyboard queue
2. Set `foreground_proc = NULL` (shell processes the keyboard directly)

### 4.7 What About Process Suspension?

When VT-switching away from Doom, we have two options:

**Option A: Don't suspend** — Doom keeps running, but its drawFrame writes to a backbuffer nobody sees. Wastes CPU but keeps Doom responsive when you switch back.

**Option B: Suspend the process** — Set Doom's task(s) to `TASK_STATE_BLOCKED` until the VT is reactivated. Saves CPU.

**Recommendation:** Option A (no suspend) for V1. It's simpler, and Doom yields frequently enough that it won't hurt the scheduler. Suspend can be added later when VTs host multiple processes.

---

## 5. Phase 2: Zero-Copy Window Drawing

### 5.1 The Current Copy

Today every frame does:
```
screen_render_shell() → writes text to surface.address (backbuffer)
screen_draw()         → memcpy(surface.address → trueAddress)     ← ~8MB per frame at 1080p
```

At 60fps, that's ~480 MB/s of bandwidth just for the final blit. With a single terminal, this is fine — the shell barely changes. But with multiple windows compositing, it could become a bottleneck.

### 5.2 Zero-Copy via Page Remapping

The core insight: **the framebuffer is just physical memory mapped into the kernel's address space.** If we map a *different* physical page to the same virtual address range, we effectively "flip" which buffer is visible — no data copied.

```
┌─────────────────────────────┐
│   Virtual Address Space      │
│                              │
│ 0xFFFF8000.... (framebuffer) │
│                              │
│   ┌─────────────────┐        │
│   │ PML4 Entry       │────────┼──→ Physical Page A (backbuffer 1)
│   │                   │        │    ┌──────────────────────┐
│   │ ── Page Flip ─── │────────┼──→ │ Physical Page B      │
│   │                   │        │    │ (backbuffer 2)       │
│   └─────────────────┘        │    └──────────────────────┘
│                              │
└─────────────────────────────┘
```

To flip: change the PML4 entry to point from Page A → Page B, then `invlpg` or reload CR3.

**However**, the real framebuffer (`trueAddress`) is a specific set of physical pages handed to us by Limine. We can't just remap them to different physical pages — the LCD controller is DMA'ing from THOSE physical addresses. So true zero-copy (no data movement at all) requires a hardware-level page flip, which needs:

- **VESA VBE / UEFI GOP page flipping**: Not supported by most hardware without a custom GOP driver
- **Multi-buffer approach**: More realistic for our OS

### 5.3 Practical Zero-Copy for sandfleaOS

The best approach for our environment:

**Maintain N backbuffers** (e.g., 3 for triple-buffering). At each VSync:

1. **Composite** all visible windows into the next available backbuffer (CPU work, unavoidable)
2. **Blit** that backbuffer to `trueAddress` (memcpy, ~8MB)
3. **Repeat** with the next backbuffer

This is NOT zero-copy end-to-end, but it's the standard approach (Linux, Windows, macOS all do this). The `memcpy` of 8MB at 60fps costs ~480 MB/s, which is ~2-3% of a modern DDR4 channel's bandwidth (~20 GB/s). It's fine.

The REAL optimization is avoiding redundant compositing — not the final blit.

### 5.4 Damage Regions / Dirty Rectangles

Instead of re-compositing the entire screen every frame:

```c
typedef struct {
    u32 x, y, w, h;
} damage_rect_t;

// Each VT tracks what changed
void vt_mark_damage(virt_terminal_t *vt, u32 x, u32 y, u32 w, u32 h);

// Compositor only redraws damaged regions
void composite_frame() {
    damage_rect_t rects[MAX_DAMAGE_RECTS];
    u32 n = collect_damage_rects(rects);
    
    for (u32 i = 0; i < n; i++) {
        // Only copy the dirty rectangle from VT's backbuffer → compositor buffer
        blit_rect(current_vt->backbuffer, compositor_buffer, rects[i]);
    }
    
    // Blit compositor buffer → real framebuffer (8MB, unavoidable)
    screen_draw();
}
```

For the initial terminal-only case, damage tracking is easy: the terminal only changes one line at a time (the input line + maybe 1-2 new scrollback lines). That's ~2 × 8 × 16 = 256 bytes out of 8MB. **0.003% of the screen.**

### 5.5 A True Zero-Copy Scheme (Future)

If we could control the display controller (or if we're on a virtual GPU in QEMU), we could:

1. Allocate two framebuffer-sized physical pages
2. Tell the display controller to DMA from Page A
3. Composite into Page B
4. Flip the controller to DMA from Page B
5. Composite into Page A
6. Repeat

QEMU's virtio-gpu and Bochs VBE both support this. This is worth exploring when we move to a userspace compositor and have a proper GPU driver.

```
Time:  ── Frame 1 ──┼── Frame 2 ──┼── Frame 3 ──
                    │             │
Page A:  Display ◄──┼── Composite  │  Display ◄──
                    │      in      │
Page B:  Composite  │  Display ◄──┼── Composite
              in    │             │
```

**No memcpy at all.** Just a register write to tell the GPU which physical address to scan out.

---

## 6. Phase 3: ELF-Based Userspace Compositor

### 6.1 When to Move to Userspace

Move the compositor out of the kernel when:

1. You have **3+ concurrent display consumers** (Doom + terminal + debugger)
2. You want **window decorations** (title bars, resize handles)
3. You want **fancy effects** (transparency, shadows, animations)
4. You want **stability** (a compositor crash shouldn't take down the kernel)

### 6.2 Architecture

```
                    ┌─────────────────────────────────┐
                    │      User Space (Ring 3)         │
                    │                                  │
                    │  ┌──────────────────────────┐   │
                    │  │  Compositor (ELF/WASM)    │   │
                    │  │  - Owns the framebuffer   │   │
                    │  │  - Manages window Z-order │   │
                    │  │  - Routes input           │   │
                    │  │  - Draws decorations      │   │
                    │  └──────┬───────────────────┘   │
                    │         │                        │
                    │  ┌──────┴──────┐ ┌──────────┐   │
                    │  │  Terminal   │ │  Doom    │   │
                    │  │  (WASM)     │ │  (WASM)  │   │
                    │  └──────┬──────┘ └─────┬────┘   │
                    │         │               │        │
                    └─────────┼───────────────┼────────┘
                              │               │
                    ┌─────────┼───────────────┼────────┐
                    │  Kernel │               │        │
                    │         ▼               ▼        │
                    │  ┌──────────────┐ ┌──────────┐  │
                    │  │  Syscalls    │ │  Keyboard│  │
                    │  │  "share_fb" │ │  Routing │  │
                    │  │  "composite"│ │          │  │
                    │  └──────────────┘ └──────────┘  │
                    └──────────────────────────────────┘
```

### 6.3 Syscall Surface

The compositor needs minimal kernel support:

```c
// 1. Map a shared framebuffer page into the compositor's address space
//    (physical page from a "donated" window backbuffer)
i32 sys_fb_share(i32 src_pid, u64 src_phys_addr);
// Returns an FD/handle that the compositor can mmap

// 2. Tell the kernel "I am the compositor" (privileged)
//    → Kernel routes keyboard/mouse to compositor
//    → Kernel gives compositor exclusive access to display
i32 sys_register_compositor();

// 3. Present a frame (flip or blit)
i32 sys_present(u64 composited_buffer_phys);

// 4. Get input events
i32 sys_get_events(input_event_t *buf, u32 max);
```

### 6.4 Shared Memory for Zero-Copy Window Contents

Each window allocates its own backbuffer via kernel framebuffer allocation:

```
Window A's page ──┬── Mapped into Compositor's address space (read-only)
                  └── Mapped into Window A's address space (read-write)

Window B's page ──┬── Mapped into Compositor's address space (read-only)
                  └── Mapped into Window B's address space (read-write)

Compositor composits A + B → compositor's output buffer
                         → sys_present() → blit to real framebuffer
```

The compositor never copies window contents — it reads them in place. This is **zero-copy from the window's perspective** (the window writes pixels, the compositor reads them from the same physical pages).

### 6.5 Input Routing

The compositor receives all input events and decides which window gets them:

```c
struct input_event {
    enum { KEY_DOWN, KEY_UP, MOUSE_MOVE, MOUSE_CLICK } type;
    u32 data[4];  // key scancode, mouse x/y, etc.
};

// Compositor loop:
while (1) {
    input_event_t ev;
    sys_get_events(&ev, 1);
    
    if (ev.type == MOUSE_CLICK) {
        // Hit-test which window was clicked
        kern_process_t *target = hit_test(ev.data[0], ev.data[1]);
        sys_route_input(target->pid);
    }
    
    // Forward key to active window
    // ...
}
```

---

## 7. Concrete Implementation Plan

### Week 1: F-Key VT Switching (~200 lines)

```
Day 1:  kern_vt.h / kern_vt.c — VT structs, init, switch function
Day 2:  Keyboard handler — F1-F4 scancodes → VT switch
Day 3:  main.c refactor — replace owns_framebuffer with VT dispatch
Day 4:  wasm_spawn.c — associate spawned process with current VT
Day 5:  Test: F1=shell, F2=Doom, swap between them
```

### Week 2: Compositor Skeleton (if needed)

```
Day 1:  sys_register_compositor()
Day 2:  sys_fb_share() — share a page between processes
Day 3:  Compositor ELF that blits window A + B → output
Day 4:  sys_present() — compositor's output to framebuffer
Day 5:  sys_get_events() + input routing
```

### Week 3: Window Decorations & Polish

```
Day 1:  Compositor draws window frames
Day 2:  Mouse cursor via SSFN or sprite
Day 3:  Drag windows by title bar
Day 4:  F-key switching as compositor feature
Day 5:  Damage rectangles + dirty tracking
```

---

## 8. Open Questions

1. **Doom's current blit destination**: Does `screen_draw()` get called during Doom's render? If not, Doom's pixels are going to `surface.address` (backbuffer) and never reaching `trueAddress`. This needs to be confirmed before VT switching works correctly.

2. **Elasticity of `kern_tests.c`**: Currently all Doom host functions (`doom_onErrorMessage`, `doom_onInfoMessage`, `doom_get_key`, `doom_draw_frame`, etc.) are in `kern_tests.c`. For proper windowing, these need to either move to `wasm_spawn.c` or use the VT infrastructure. This is a code organization issue, not a design blocker.

3. **SMP later**: Everything here assumes single-core. A future SMP window manager will need per-core framebuffer copies or GPU-side compositing. Not a concern yet.

4. **What happens to `kern_tests.c`?** Eventually all the Doom host functions should move to a `src/kernel/doom_host.c` or similar, making `kern_tests.c` truly just a test runner. This is a prerequisite for clean VT separation.

5. **Should VTs be 1:1 with processes?** For v1, keep it simple: 4 pre-allocated VTs. When you switch to a VT that has no foreground process, spawn a new shell. This matches how Linux VTs work (`/dev/tty1` etc.).

---

## 9. Appendix: VT Switch Flow (Detailed)

```
User presses F2
  │
  ▼
keyboard_handle_keypress()
  ├── Detects scancode 0x3C (F2)
  ├── Sets ascii = KEY_F2 (= 0x86)
  └── Pushes onto keyboard queue
  │
  ▼
main loop: keyboard_eat_key() returns KEY_F2
  │
  ▼
handle_vt_switch(1) // VT index 1 (second VT)
  │
  ├── 1. Save current VT state
  │     ├── virt_terminals[current_vt_id].text_root = screen_text_root
  │     ├── virt_terminals[current_vt_id].text_tail = screen_text_tail
  │     ├── virt_terminals[current_vt_id].text_scroll = screen_text_scroll
  │     ├── virt_terminals[current_vt_id].foreground_proc = foreground_proc
  │     ├── virt_terminals[current_vt_id].fg_queue_read_ptr = fg_queue_read_ptr
  │     ├── virt_terminals[current_vt_id].fg_queue_write_ptr = fg_queue_write_ptr
  │     └── memcpy(virt_terminals[current_vt_id].fg_key_queue,
  │                fg_key_queue, FG_QUEUE_SIZE)
  │
  ├── 2. Suspend current foreground if needed (optional)
  │     └── Suspend Doom's tasks (TASK_STATE_BLOCKED)
  │
  ├── 3. Restore target VT state
  │     ├── screen_text_root = virt_terminals[1].text_root
  │     ├── screen_text_tail = virt_terminals[1].text_tail
  │     ├── screen_text_scroll = virt_terminals[1].text_scroll
  │     ├── foreground_proc = virt_terminals[1].foreground_proc
  │     ├── fg_queue_read_ptr = virt_terminals[1].fg_queue_read_ptr
  │     ├── fg_queue_write_ptr = virt_terminals[1].fg_queue_write_ptr
  │     ├── memcpy(fg_key_queue, virt_terminals[1].fg_key_queue, FG_QUEUE_SIZE)
  │     └── owns_framebuffer = (virt_terminals[1].foreground is fullscreen app)
  │
  ├── 4. Blit VT's backbuffer to real framebuffer
  │     ├── screen_blit(current_display->trueAddress,
  │     │               virt_terminals[1].backbuffer.address)
  │     └── OR: page-flip if implementing zero-copy
  │
  └── 5. Render if it's a text VT
        └── if (!owns_framebuffer) screen_render_shell()
                ├── screen_clear(COLOR_BLACK)
                ├── Render text lines from screen_text_root
                ├── Draw status bar
                └── screen_draw() // pushes to trueAddress
```

---

## 10. Summary

| Phase | What | When | Lines | Zero-Copy? |
|-------|------|------|-------|------------|
| **1** | F-key VT switching | This week | ~200 | ❌ (memcpy VT→fb) |
| **2** | Damage regions | Next | ~50 | ❌ but 10-100x fewer bytes copied |
| **3** | Compositor (kernel) | When needed | ~300 | ✅ (page-flip in QEMU) |
| **4** | Compositor (userspace ELF) | Long term | ~1000 | ✅ (shared fb pages) |

**Start with F-key switching this week.** It's the highest value per line of code, it solves the immediate Doom/Terminal problem, and it builds the VT abstraction that everything else will need anyway. The full compositor can — and should — be deferred until there are 3+ things competing for the screen.
