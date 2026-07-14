# Framebuffer Access for WASM Applications

**Status:** Design Exploration  
**Date:** July 2026  
**Context:** Doom currently blits directly to the real framebuffer via a custom `drawFrame` host function. This doc generalizes that into a clean architecture any WASM app can use.

---

## 1. Problem Statement

sandfleaOS has one physical framebuffer (or more, via Limine multi-monitor). Currently:

- **Terminal apps** render via cell buffers → backbuffer → `screen_draw()` memcpy to real framebuffer
- **Doom** bypasses everything and writes directly to `disp->trueAddress` via a custom WASM host function
- **There is no generic mechanism** for a WASM app to say "I want to draw pixels"

We need a unified architecture where any WASM program can:

1. Declare it wants display access
2. Learn the display resolution and format
3. Write pixels (to a buffer, or directly)
4. Signal "frame ready" (present)
5. Be composed with other windows (future)
6. Do all of this without crashing the kernel or corrupting other apps' pixels

---

## 2. Design Constraints

| Constraint | Why |
|------------|-----|
| **WASM linear memory is the only memory an app sees** | No raw pointers, no `kmalloc`, no shared memory pages without a host function bridge |
| **App is preemptively scheduled** | Can't assume the app runs to completion before swapping buffers |
| **Double-buffering already exists** | `surface.address` (backbuffer) + `trueAddress` (real FB) in `display_t` |
| **Zero-copy is desirable but not required** | 8MB memcpy at 60fps is ~2% of DDR4 bandwidth — fine for now |
| **Future compositing** | Must support multiple apps' buffers being blended onto one screen |
| **No GPU** | Software rendering only — every pixel is CPU-written |

---

## 3. Architecture Overview

### 3.1 Three Tiers of Framebuffer Access

```
                     ┌─────────────────────────────────────────┐
                     │          WASM Application                │
                     │                                         │
                     │  Linear Memory: [code][data][stack]      │
                     │                    │                     │
                     │  Optional: display buffer at known offset│
                     └───────────────────┬─────────────────────┘
                                         │
                      ┌──────────────────┼──────────────────┐
                      ▼                  ▼                  ▼
              ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
              │  Tier 1      │  │  Tier 2      │  │  Tier 3      │
              │  Host Call   │  │  Shared Page │  │  Zero-Copy   │
              │  Blit        │  │  Mapping     │  │  Page Flip   │
              │              │  │              │  │              │
              │ drawFrame()  │  │ fb_phys page │  │ App owns its │
              │ copies from  │  │ mapped into  │  │ own physical │
              │ WASM memory  │  │ WASM linear  │  │ page, kernel │
              │ → backbuffer │  │ mem, kernel  │  │ page-flips   │
              │              │  │ reads it     │  │ display to it│
              └──────────────┘  └──────────────┘  └──────────────┘
```

### 3.2 Tier Comparison

| Aspect | Tier 1: Host Blit | Tier 2: Shared Page | Tier 3: Page Flip |
|--------|------------------|--------------------|--------------------|
| **How app writes** | Fills buffer in WASM linear mem, calls `drawFrame(offset)` | Writes directly to a page mapped into WASM linear mem | Same as Tier 2 |
| **How kernel reads** | Copies from WASM linear mem via host function | Reads the shared physical page at compositing time | Tells display controller to scan out the app's page |
| **Copy cost** | Full frame memcpy (8MB) | Zero (kernel reads in place) | Zero |
| **App changes needed** | Must call `drawFrame` | Just writes to a pointer | Same as Tier 2 |
| **Complexity** | ★☆☆ (trivial) | ★★☆ (VMM work) | ★★★ (display hw control) |
| **Security** | ✅ App can only write to its own WASM mem | ✅ App writes to its own page, can't touch others | ✅ Same |
| **Compositing** | Kernel composites by copying each app's buffer | Kernel reads each app's page in place | App owns the display — no compositing |
| **Ready for** | **Now** (Doom proves it) | **Next** | **Future** (QEMU virtio-gpu) |

---

## 4. Tier 1: Host Function Blit (MVP)

### 4.1 API

```c
// WASM imports:
// (import "display" "getResolution" (func (result i32 i32)))
// (import "display" "getFormat"     (func (result i32)))      
// (import "display" "drawFrame"     (func (param i32 i32 i32)))  // offset, width, height

// Host functions:
m3ApiRawFunction(wasm_display_get_resolution) {
    m3ApiReturnType(i32)  // packed: (height << 16) | width, or two separate calls
    display_t *disp = screen_current_display();
    u32 packed = (disp->surface.width << 16) | disp->surface.height;
    m3ApiReturn((i32)packed);
}

m3ApiRawFunction(wasm_display_get_format) {
    m3ApiReturnType(i32)
    // Return a format enum:
    // 0 = ARGB32 (32-bit, 4 bytes per pixel, native byte order)
    // 1 = XRGB32
    // 2 = RGB565 (future)
    m3ApiReturn(0);  // ARGB32 for now
}

m3ApiRawFunction(wasm_display_draw_frame) {
    m3ApiGetArg(u32, buffer_offset)  // offset in WASM linear memory
    m3ApiGetArg(u32, width)
    m3ApiGetArg(u32, height)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || buffer_offset + width * height * 4 > mem_size) {
        m3ApiReturn(-1);  // bounds error
    }

    display_t *disp = screen_current_display();
    if (!disp || !disp->surface.address) {
        m3ApiReturn(-2);  // no display
    }

    // Blit from WASM memory → backbuffer (centered)
    u32 *src = (u32 *)(mem + buffer_offset);
    u32 *dst = (u32 *)disp->surface.address;
    u32 dst_pitch_px = disp->surface.pitch / 4;
    u32 copy_w = min(width, disp->surface.width);
    u32 copy_h = min(height, disp->surface.height);
    u32 dst_x_off = (disp->surface.width - copy_w) / 2;
    u32 dst_y_off = (disp->surface.height - copy_h) / 2;

    for (u32 y = 0; y < copy_h; y++) {
        mem_copy(
            (u8 *)(dst + (dst_y_off + y) * dst_pitch_px + dst_x_off),
            (u8 *)(src + y * width),
            copy_w * 4
        );
    }

    // Flip: backbuffer → real framebuffer
    screen_draw();

    m3ApiReturn(0);
}
```

### 4.2 Session Integration

When a WASM app calls `drawFrame`, it's writing to the **active session's backbuffer**. This means:

- If the user switches TTYs, the app's backbuffer is saved with the session
- When switching back, the session's backbuffer is restored to the display
- Multiple apps on different TTYs each have their own backbuffer

```c
// In term_render(), when a doom-like app is active:
if (s->owns_display) {
    // App's render loop calls drawFrame → writes to backbuffer → screen_draw()
    // We don't need to do anything here — drawFrame already flipped
    return;
}
```

### 4.3 Session Flag

Add a flag to `term_session_t`:

```c
typedef struct term_session {
    // ... existing fields ...
    bool    owns_display;      // does this session's app control the full screen?
    // If true: term_render() skips cell-buffer rendering (app owns the FB)
    // If false: term_render() renders cells normally
} term_session_t;
```

Set by the WASM loading code when an app claims the display:

```c
// In wasm_spawn.c, after linking display imports:
if (module_imports_display(module)) {
    active_session->owns_display = true;
}
```

---

## 5. Tier 2: Shared Memory Page Mapping

### 5.1 Motivation

Tier 1 always copies the full frame from WASM memory → backbuffer → real FB. For a 1920×1080 display at 60fps, that's `1920×1080×4×60 = 498 MB/s` of copying. Most of this is redundant — the app only changes a small portion of the screen each frame.

Tier 2 eliminates the WASM-memory-to-backbuffer copy by **mapping a physical page directly into the app's WASM linear memory**. The app writes pixels to this page, and the kernel reads them (in place) when compositing.

### 5.2 How It Works

```
WASM Linear Memory Layout:
┌─────────────────────────────────┐
│  Code / Data / Stack             │
├─────────────────────────────────┤
│  Display Buffer (framebuffer-    │
│  sized, at a fixed offset or     │
│  obtained via host function)     │
│                                 │
│  This region is BACKED BY A     │
│  PHYSICAL PAGE that the kernel  │
│  can also read.                 │
└─────────────────────────────────┘
          ▲              ▲
          │              │
    App writes      Kernel reads
    pixels here     (compositing)
```

### 5.3 API

```c
// (import "display" "claimBuffer" (func (result i32)))  // returns linear-memory offset

m3ApiRawFunction(wasm_display_claim_buffer) {
    m3ApiReturnType(i32)

    display_t *disp = screen_current_display();
    u32 fb_size = disp->surface.pitch * disp->surface.height;

    // 1. Allocate physical page for the display buffer
    u64 fb_phys = pmm_alloc_page();  // or multiple pages for larger screens

    // 2. Map it into the current process's PML4 at a specific virtual address
    u64 fb_virt = allocate_in_process_vaddr(proc, fb_size);
    vmm_map_page_in_pml4(proc->cr3, fb_phys, fb_virt, PAGE_PRESENT | PAGE_RW | PAGE_USER);

    // 3. Map the SAME physical page into WASM linear memory
    //    (grow WASM memory if needed, then map at the new page)
    u32 wasm_offset = wasm_grow_memory(runtime, fb_size);
    map_phys_into_wasm_memory(runtime, fb_phys, wasm_offset, fb_size);

    // 4. Store mapping for compositing
    proc->display_buffer_phys = fb_phys;
    proc->display_buffer_wasm_offset = wasm_offset;

    m3ApiReturn((i32)wasm_offset);
}

// (import "display" "present" (func))  // signal "frame ready"

m3ApiRawFunction(wasm_display_present) {
    // The app has finished writing to its display buffer.
    // 1. Read from the app's physical page
    // 2. Blit to the backbuffer (or compositor)
    // 3. screen_draw() to flip

    kern_process_t *proc = sched_get_current_process();
    u64 fb_phys = proc->display_buffer_phys;
    u64 fb_virt_kernel = fb_phys + hhdm_offset;  // kernel can read it via HHDM

    display_t *disp = screen_current_display();
    u32 *src = (u32 *)fb_virt_kernel;
    u32 *dst = (u32 *)disp->surface.address;

    // memcpy from shared page → backbuffer
    u32 fb_pixels = disp->surface.pitch * disp->surface.height / 4;
    mem_copy((u8 *)dst, (u8 *)src, fb_pixels * 4);

    screen_draw();  // flip to real FB
}
```

### 5.4 Memory Management

| Phase | What Happens |
|-------|-------------|
| **App starts** | `claimBuffer` allocates a physical page, maps it into both WASM linear mem and kernel HHDM space |
| **App runs** | Writes pixels directly to the shared page at the returned WASM offset |
| **App presents** | Kernel reads the physical page (via HHDM), copies to backbuffer, flips |
| **Session switch** | Physical page stays mapped — session saves the WASM offset |
| **Switch back** | Kernel re-reads the same physical page (app may have changed it since switch) |
| **App exits** | `process_exit` walks `mem_regions`, unmaps the physical page, frees it |

### 5.5 Dirty Rectangle Tracking

To avoid copying the full 8MB frame every `present`:

```c
// App calls these to mark changed regions:
// (import "display" "markDirty" (func (param i32 i32 i32 i32)))  // x, y, w, h

// Kernel accumulates dirty rects and only copies those regions:
typedef struct {
    u32 x, y, w, h;
} dirty_rect_t;

#define MAX_DIRTY_RECTS 64

void display_mark_dirty(kern_process_t *proc, u32 x, u32 y, u32 w, u32 h) {
    if (proc->dirty_rect_count < MAX_DIRTY_RECTS) {
        proc->dirty_rects[proc->dirty_rect_count++] = (dirty_rect_t){x, y, w, h};
    } else {
        // Too many rects — just mark the whole screen dirty
        proc->dirty_rect_count = 1;
        proc->dirty_rects[0] = (dirty_rect_t){0, 0, SCREEN_W, SCREEN_H};
    }
}

void display_present(kern_process_t *proc) {
    display_t *disp = screen_current_display();
    u64 fb_phys = proc->display_buffer_phys;
    u32 *src = (u32 *)(fb_phys + hhdm_offset);
    u32 *dst = (u32 *)disp->surface.address;

    if (proc->dirty_rect_count == 1 &&
        proc->dirty_rects[0].w == disp->surface.width &&
        proc->dirty_rects[0].h == disp->surface.height) {
        // Full-screen dirty — fast path: single memcpy
        mem_copy((u8 *)dst, (u8 *)src, disp->surface.pitch * disp->surface.height);
    } else {
        // Partial dirty — copy rects
        for (u32 i = 0; i < proc->dirty_rect_count; i++) {
            dirty_rect_t *r = &proc->dirty_rects[i];
            for (u32 y = r->y; y < r->y + r->h; y++) {
                mem_copy(
                    (u8 *)(dst + y * (disp->surface.pitch / 4) + r->x),
                    (u8 *)(src + y * (disp->surface.pitch / 4) + r->x),
                    r->w * 4
                );
            }
        }
    }

    proc->dirty_rect_count = 0;
    screen_draw();
}
```

### 5.6 Composing Multiple Apps (Future)

With shared pages, compositing multiple apps is straightforward:

```
App A's display page ──┬── mapped into WASM linear mem (A writes here)
                       └── kernel reads for compositing

App B's display page ──┬── mapped into WASM linear mem (B writes here)
                       └── kernel reads for compositing

Compositor:
  1. Draw App A's page to backbuffer at position (x_a, y_a)
  2. Draw App B's page to backbuffer at position (x_b, y_b)
  3. screen_draw() → flip
```

No per-app memcpy needed — just compositing-level blits.

---

## 6. Tier 3: Zero-Copy Page Flip (Future)

### 6.1 How It Works

Instead of copying from the shared page to the backbuffer, tell the display controller to scan out from the shared page directly:

```
Normal mode:
  [Real FB] ← LCD controller DMA reads from here
       ↑
  [Backbuffer] ← kernel writes here

Zero-copy flip:
  [Real FB] ← was [Page A], now [Page B]
       ↑
  App writes to [Page B]
```

### 6.2 Requirements

- **The display controller must support changing the scanout address**
  - Limine framebuffers are fixed physical pages — we can't change where the LCD reads from
  - **QEMU virtio-gpu** and **Bochs VBE** support this via register writes
  - Real hardware varies (some Intel/AMD GPUs support flipping in their display engine)

- **The physical page must be contiguous** (not scattered like kmalloc pages)
  - `pmm_alloc_page()` gives 4KB physical pages — fine for a single page
  - For larger buffers, need contiguous physical memory allocation

### 6.3 When to Implement

When running in QEMU with virtio-gpu, or when a real GPU driver exists. Not before.

---

## 7. WASM Import Registration

### 7.1 Auto-Detection

The linker should detect whether a WASM module imports display functions and register them automatically:

```c
// In wasm_spawn.c, after linking common imports:

// Check if the module wants display access
if (module_has_import(module, "display", "getResolution")) {
    m3_LinkRawFunction(module, "display", "getResolution",
                       "i()", &wasm_display_get_resolution);
    m3_LinkRawFunction(module, "display", "getFormat",
                       "i()", &wasm_display_get_format);
    m3_LinkRawFunction(module, "display", "drawFrame",
                       "i(iii)", &wasm_display_draw_frame);

    // Mark session as display-owning
    if (active_session) {
        active_session->owns_display = true;
    }
}
```

### 7.2 Module Has Import Helper

```c
bool module_has_import(IM3Module module, const char *mod_name, const char *func_name) {
    // Iterate the module's import section
    // wasm3 doesn't expose this directly, but we can check by attempting
    // m3_LinkRawFunction and seeing if the import exists.
    // OR: parse the imports from the raw WASM binary (import section at offset 0x..)
    // For now: just check the module name string in the import list.
}
```

A simpler approach: **just try to link**. If the import doesn't exist, `m3_LinkRawFunction` returns an error (which we can ignore). If it does exist, the function is linked. This is already what wasm3 does — linking a non-existent import is a no-op that returns an error.

```c
// Safe to call even if the module doesn't import "display":
M3Result r = m3_LinkRawFunction(module, "display", "drawFrame", "i(iii)", &wasm_display_draw_frame);
if (r) {
    // Not an error — module just doesn't use display
    serial_outsf("Display functions not linked (module doesn't import them): %s\\n", r);
}
```

This means **every WASM app automatically gets display access if it asks for it**. No capability system needed for now.

---

## 8. Migration: Doom → New Architecture

### 8.1 Current Doom

Doom currently:
1. Has a custom `wasm_doom_test()` that does its own loading + game loop
2. Uses `doom_drawFrame` host function → writes to `disp->trueAddress`
3. Manually sets `doom_active = true`
4. Manually clears the framebuffer

### 8.2 New Doom

Doom should:
1. Use `wasm_spawn()` with the standard `wasm_thread_entry` (no custom entry)
2. Import the standard `display.drawFrame` instead of its custom `ui.drawFrame`
3. Set `module = "display"`, not `module = "ui"`, in the WASM source

The `wasm_spawn_opts_t.link_extra` function would add Doom-specific imports:

```c
void doom_link_extra(IM3Module module, IM3Runtime rt, void *user) {
    m3_LinkRawFunction(module, "console", "onErrorMessage", "v(ii)", &doom_onErrorMessage);
    m3_LinkRawFunction(module, "console", "onInfoMessage",  "v(ii)", &doom_onInfoMessage);
    // ...
    // Note: display.drawFrame is now linked by the common code, NOT here
}

void handle_command_doom() {
    wasm_spawn_opts_t opts = {
        .path = "doom-v0.1.0.wasm",
        .foreground = true,
        .link_extra = doom_link_extra,
        .wait = true,  // ← now works with display functions!
    };
    wasm_spawn(&opts);
}
```

### 8.3 What Changes

| Aspect | Old Doom | New Doom |
|--------|----------|----------|
| Entry point | Custom `wasm_doom_test()` | Standard `wasm_thread_entry` |
| Game loop | Custom `while(1)` in C | WASM `_start()` with its own loop |
| Draw function | `ui.drawFrame` → custom | `display.drawFrame` → standard |
| Framebuffer access | Direct to `trueAddress` | Writes to WASM buffer → host blits |
| Session management | Manual `doom_active` | Auto via `owns_display` |
| TTY switching | Manual session save/restore | Automatic (session infrastructure) |

---

## 9. Security Considerations

### 9.1 What an App Can Do

With the Tier 1 host-call approach:

- ✅ Write pixels to its own WASM linear memory (safe — wasm3 enforces bounds)
- ✅ Call `drawFrame(offset, w, h)` where `offset`, `w`, `h` are within its memory
- ❌ Write to arbitrary kernel memory (host function checks bounds)
- ❌ Read other apps' pixels (each app has its own WASM linear memory)
- ❌ Access the display after its timeslice ends (preemption is transparent)

### 9.2 Bounds Checking

The critical safety check in `drawFrame`:

```c
u32 mem_size = 0;
u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
if (!mem || buffer_offset + width * height * 4 > mem_size) {
    m3ApiReturn(-1);  // bounds error — app bug, safe to ignore
}
```

Without this, a malicious app could pass `width = 0x7FFFFFFF` and read kernel memory. With the check, the max readable region is the app's own WASM linear memory.

### 9.3 Display Capturing

An app could call `drawFrame` every frame to read the current screen contents (since it provides the source buffer and the kernel copies TO it). Actually no — `drawFrame` copies FROM WASM memory TO the display. The app can't read the current screen contents because the copy is unidirectional.

For true security (preventing screen capture), the app never receives framebuffer contents — it only pushes pixels to the display. This is intentional: a terminal app running in a window shouldn't be able to read what's in the adjacent window.

### 9.4 Rate Limiting

An app could call `drawFrame` in a tight loop (DoS by framebuffer copy). Mitigations:

1. **Scheduler**: The app is preemptively scheduled — it can't saturate CPU
2. **VSync**: `drawFrame` blocks until the next VSync (~16ms at 60fps), capping to 60fps
3. **Yield**: After `drawFrame`, the host function calls `sched_yield()` to let other tasks run

```c
m3ApiRawFunction(wasm_display_draw_frame) {
    // ... copy pixels ...

    // Rate limit: yield after drawing
    sched_yield();

    m3ApiReturn(0);
}
```

---

## 10. Implementation Plan

### Phase 1: Generalize DrawFrame (This Week)

1. **Add `display` import module** to `wasm_spawn.c`:
   - `getResolution`
   - `getFormat`
   - `drawFrame`

2. **Auto-link** if module imports `"display"`:
   - Check via `m3_LinkRawFunction` error (safe to call on non-importing modules)

3. **Add `owns_display`** field to `term_session_t`

4. **Update `term_render()`** to skip cell rendering when `owns_display = true`

5. **Keep existing Doom** working via `link_extra` (add do-nothing `ui.drawFrame` wrapper that calls the new `display.drawFrame`)

### Phase 2: Migrate Doom

1. Recompile Doom WASM to import from `"display"` instead of `"ui"`
2. Remove custom `wasm_doom_test()` entry point
3. Doom uses standard `wasm_thread_entry` with `link_extra` for game-specific imports
4. Remove manual `doom_active` — use `owns_display` instead

### Phase 3: Shared Pages (Tier 2)

1. Implement `claimBuffer` host function
2. Implement physical page sharing between WASM linear memory and kernel HHDM
3. Implement `present` host function
4. Implement dirty rectangle tracking
5. Optionally: wire into `term_render()` for compositing with cell buffer

### Phase 4: Multi-App Compositing (Future)

1. Multiple apps can each `claimBuffer`
2. Compositor (kernel or userspace) reads all app buffers, places them at x,y positions
3. Input routing: clicks determine which app gets keyboard focus

---

## 11. Open Questions

1. **Should `drawFrame` write to the backbuffer or a per-app buffer?** Currently writes to the global backbuffer (shared with terminal). For multi-app compositing, each app needs its own buffer. Phase 3 (shared pages) solves this naturally.

2. **Can a WASM module both use the terminal AND draw to the display?** Not in the current model — `owns_display` is a binary flag. A future compositing model could overlay terminal output on top of an app's frame (e.g., a HUD overlay).

3. **What about mouse cursor rendering?** The cursor should be composited by the kernel (or compositor) as a sprite overlay, not by each app. This requires a cursor plane (hardware) or software compositing in the final blit.

4. **How does an app know when the frame was actually displayed?** For simple apps, `drawFrame` returns immediately after the blit. For VSync-synchronized apps, add a `waitForVsync` host function that blocks until the next vertical blank.

5. **Should the display buffer be zero-initialized?** Yes — a freshly claimed buffer should be all-black (or all-transparent for compositing). The caller (`claimBuffer` / `drawFrame` before first use) should zero it.

---

## 12. Summary

| Layer | What | When | Lines | Complexity |
|-------|------|------|-------|------------|
| **1a** | Generalize `display.drawFrame` host function | This week | ~50 | ★☆☆ |
| **1b** | `owns_display` session flag + term_render skip | This week | ~10 | ★☆☆ |
| **1c** | Auto-link display imports in wasm_spawn | This week | ~20 | ★☆☆ |
| **2** | Migrate Doom to standard path | Next | ~100 | ★★☆ |
| **3** | `claimBuffer` + `present` (shared pages) | Near | ~150 | ★★★ |
| **4** | Dirty rectangle tracking | Near | ~80 | ★★☆ |
| **5** | Multi-app compositing | Future | ~300 | ★★★★ |
| **6** | Zero-copy page flip | Future | ~100 | ★★★★★ |

**Start with Phase 1.** It's 80 lines that give every WASM app the ability to draw pixels — the same mechanism Doom uses, but standardized. Doom can keep working via its custom path until Phase 2 migrates it.
