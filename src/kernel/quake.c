// src/kernel/quake.c
// Quake command launcher and custom game-thread entry.

#include "../include/dialect.h"
#include "../include/kern_screen.h"
#include "../include/kern_serial.h"
#include "../include/kern_mem.h"
#include "../include/kern_fs.h"
#include "../include/kern_sched.h"
#include "../include/wasm_spawn.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_mouse.h"
#include "../include/kern_profile.h"
#include "../include/kern_terminal.h"

#ifndef PROFILE_ENABLED
#define PROFILE_ENABLED 0
#endif

#include "wasm3-0.5.0/source/m3_env.h"
#include "wasm3-0.5.0/source/m3_api_libc.h"

static void quake_wasm_set_memory(void *memory) {
    (void)memory;
}


// Quake-specific host ABI. These imports are distinct from the generic
// env.fd_* imports used by other WASM programs.
m3ApiRawFunction(quake_wasm_fd_open_host) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, path_offset)
    m3ApiGetArg(i32, flags)
    u32 size = 0;
    u8 *mem = m3_GetMemory(runtime, &size, 0);
    if (!mem || path_offset >= size) m3ApiReturn(-1);
    m3ApiReturn(fs_open((const char *)(mem + path_offset)));
}

m3ApiRawFunction(quake_wasm_fd_read_host) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, buf_offset)
    m3ApiGetArg(i32, length)
    u32 size = 0;
    u8 *mem = m3_GetMemory(runtime, &size, 0);
    if (!mem || length < 0 || buf_offset > size || (u32)length > size - buf_offset) m3ApiReturn(-1);
    m3ApiReturn(fs_read(fd, mem + buf_offset, (u32)length));
}

m3ApiRawFunction(quake_wasm_fd_write_host) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, buf_offset)
    m3ApiGetArg(i32, length)
    u32 size = 0;
    u8 *mem = m3_GetMemory(runtime, &size, 0);
    if (!mem || length < 0 || buf_offset > size || (u32)length > size - buf_offset) m3ApiReturn(-1);
    if (fd == 1 || fd == 2) { term_write((const char *)(mem + buf_offset), (u32)length); m3ApiReturn(length); }
    m3ApiReturn(fs_write(fd, mem + buf_offset, (u32)length));
}

m3ApiRawFunction(quake_wasm_fd_seek_host) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(i32, offset)
    m3ApiGetArg(i32, whence)
    m3ApiReturn(fs_seek(fd, offset, whence));
}

m3ApiRawFunction(quake_wasm_fd_close_host) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiReturn(fs_close(fd));
}

m3ApiRawFunction(quake_wasm_fd_size_host) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiReturn((i32)fs_size(fd));
}

m3ApiRawFunction(quake_wasm_log_host) {
    m3ApiGetArg(i32, error)
    m3ApiGetArg(u32, message_offset)
    u32 size = 0;
    u8 *mem = m3_GetMemory(runtime, &size, 0);
    if (mem && message_offset < size) {
        serial_outsf("QUAKE[%d]: %s\n", error, (char *)(mem + message_offset));
    }
    m3ApiSuccess();
}

m3ApiRawFunction(quake_wasm_time_host) {
    m3ApiReturnType(i64)
    static u64 ticks;
    ticks += 16;
    m3ApiReturn(ticks);
}

m3ApiRawFunction(quake_wasm_draw_frame_host) {
    m3ApiGetArg(u32, pixels)
    m3ApiGetArg(i32, palette)
    m3ApiGetArg(i32, width)
    m3ApiGetArg(i32, height)
    m3ApiGetArg(i32, stride)
    (void)pixels; (void)palette; (void)width; (void)height; (void)stride;
    m3ApiSuccess();
}

static i32 quake_call_i32(IM3Runtime runtime, const char *name) {
    IM3Function fn = null;
    if (m3_FindFunction(&fn, runtime, name)) return -1;
    if (m3_Call(fn, 0, NULL)) return -1;
    u32 value = 0;
    if (m3_GetResultsV(fn, &value)) return -1;
    return (i32)value;
}

static void quake_blit(IM3Runtime runtime, u32 offset, u32 width, u32 height, u32 stride) {
    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    display_t *disp = screen_current_display();
    u64 bytes = (u64)stride * height;
    if (!mem || !disp || !disp->trueAddress || width == 0 || height == 0 ||
        stride < width * 4u || offset > memory_size || bytes > memory_size - offset) return;

    u32 scale_x = disp->surface.width / width;
    u32 scale_y = disp->surface.height / height;
    u32 scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1) scale = 1;

    u32 start_x = (disp->surface.width - width * scale) / 2;
    u32 start_y = (disp->surface.height - height * scale) / 2;

    u32 *src = (u32 *)(mem + offset);
    u32 *dst = (u32 *)disp->trueAddress;
    u32 pitch = disp->surface.pitch / 4;
    u32 src_stride_pixels = stride / 4;

    if (scale == 1) {
        for (u32 y = 0; y < height; y++) {
            mem_copy((u8 *)(dst + (start_y + y) * pitch + start_x),
                     (u8 *)(src + y * src_stride_pixels), width * 4u);
        }
    } else if (scale == 2) {
        for (u32 y = 0; y < height; y++) {
            u32 *srow = src + y * src_stride_pixels;
            u64 *drow1_64 = (u64 *)(dst + (start_y + y * 2) * pitch + start_x);
            for (u32 x = 0; x < width; x += 2) {
                u64 p0 = (u64)srow[x];
                u64 p1 = (u64)srow[x + 1];
                drow1_64[x]     = (p0 << 32) | p0;
                drow1_64[x + 1] = (p1 << 32) | p1;
            }
            u8 *drow2 = (u8 *)(dst + (start_y + y * 2 + 1) * pitch + start_x);
            mem_copy(drow2, (u8 *)drow1_64, width * 8u);
        }
    } else {
        for (u32 y = 0; y < height; y++) {
            u32 *srow = src + y * src_stride_pixels;
            for (u32 sy = 0; sy < scale; sy++) {
                u32 *drow = dst + (start_y + y * scale + sy) * pitch + start_x;
                for (u32 x = 0; x < width; x++) {
                    u32 p = srow[x];
                    for (u32 sx = 0; sx < scale; sx++) {
                        drow[x * scale + sx] = p;
                    }
                }
            }
        }
    }
}

// This is a thread entry, not a launcher: it must never call wasm_spawn().
u0 wasm_quake_game(void *arg) {
    const char *path = arg ? (const char *)arg : "quake.wasm";
    PROFILE_SCOPE("quake:game");
    serial_outsf("WASM QUAKE: loading %s\n", path);

    PROFILE_BEGIN("quake:fs_open_wasm");
    i32 fd = fs_open(path);
    PROFILE_END("quake:fs_open_wasm");
    if (fd < 0) {
        screen_push_linef("WASM QUAKE: could not open %s", path);
        return;
    }
    u32 size = fs_size(fd);
    u8 *data = kmalloc(size);
    if (!data) {
        fs_close(fd);
        screen_push_line("WASM QUAKE: file buffer OOM");
        return;
    }
    PROFILE_BEGIN("quake:fs_read_wasm");
    fs_read(fd, data, size);
    PROFILE_END("quake:fs_read_wasm");
    fs_close(fd);

    IM3Environment env = m3_NewEnvironment();
    // Keep the runtime's allocation limit above Quake's 64 MiB initial module memory.
    // wasm3 rounds its backing allocation to the module's initial memory size.
    IM3Runtime runtime = env ? m3_NewRuntime(env, 512 * 1024, NULL) : null;
    if (!runtime) {
        screen_push_line("WASM QUAKE: runtime creation failed");
        kfree(data);
        if (env) m3_FreeEnvironment(env);
        return;
    }

    IM3Module module = null;
    M3Result result = m3_ParseModule(env, &module, data, size);
    if (result) {
        screen_push_linef("WASM QUAKE: parse error: %s", result);
        m3_FreeRuntime(runtime); m3_FreeEnvironment(env); kfree(data);
        return;
    }
    result = m3_LoadModule(runtime, module);
    if (result) {
        screen_push_linef("WASM QUAKE: load error: %s", result);
        m3_FreeRuntime(runtime); m3_FreeEnvironment(env); kfree(data);
        return;
    }

    serial_outsl("WASM QUAKE: linking WASI");
    kern_link_wasi(module, sched_get_current_process());
    serial_outsl("WASM QUAKE: linking LibC");
    result = m3_LinkLibC(module);
    if (result) {
        serial_outsf("WASM QUAKE: libc link error: %s\n", result);
        screen_push_linef("WASM QUAKE: libc link error: %s", result);
        m3_FreeRuntime(runtime); m3_FreeEnvironment(env); kfree(data);
        return;
    }
    serial_outsl("WASM QUAKE: imports linked");

    // Link Quake's dedicated host ABI after loading. The module contains weak
    // fallbacks, so these imports are normally internal unless explicitly bound.
    m3_LinkRawFunction(module, "env", "quake_wasm_fd_open", "i(ii)", &quake_wasm_fd_open_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_fd_read", "i(iii)", &quake_wasm_fd_read_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_fd_write", "i(iii)", &quake_wasm_fd_write_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_fd_seek", "i(iii)", &quake_wasm_fd_seek_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_fd_close", "i(i)", &quake_wasm_fd_close_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_fd_size", "i(i)", &quake_wasm_fd_size_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_log", "v(ii)", &quake_wasm_log_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_time_milliseconds", "I()", &quake_wasm_time_host);
    m3_LinkRawFunction(module, "env", "quake_wasm_draw_frame", "v(iiiii)", &quake_wasm_draw_frame_host);
    m3_CompileModule(module);

    IM3Function init = null, tick = null;
    IM3Function key_down = null, key_up = null;
    IM3Function mouse_move = null, mouse_button = null, shutdown = null;
    if (m3_FindFunction(&init, runtime, "initGame") ||
        m3_FindFunction(&tick, runtime, "tickGame") ||
        m3_FindFunction(&key_down, runtime, "reportKeyDown") ||
        m3_FindFunction(&key_up, runtime, "reportKeyUp") ||
        m3_FindFunction(&mouse_move, runtime, "reportMouseMove") ||
        m3_FindFunction(&mouse_button, runtime, "reportMouseButton") ||
        m3_FindFunction(&shutdown, runtime, "shutdownGame")) {
        screen_push_line("WASM QUAKE: required export missing");
        m3_FreeRuntime(runtime); m3_FreeEnvironment(env); kfree(data);
        return;
    }

    IM3Function wasm_init = null;
    if (!m3_FindFunction(&wasm_init, runtime, "_initialize")) {
        serial_outsl("WASM QUAKE: calling _initialize");
        m3_Call(wasm_init, 0, NULL);
    }

    u32 quake_memory_size = 0;
    void *quake_memory = m3_GetMemory(runtime, &quake_memory_size, 0);
    /* The Quake module owns its own linear-memory address space; no native
     * pointer can be passed across this ABI. */
    (void)quake_memory;
    (void)quake_memory_size;
    serial_outsl("WASM QUAKE: calling initGame");
    serial_outsl("WASM QUAKE: note: initGame uses internal COM_InitArgv state");
    u32 mem_before_init = 0;
    m3_GetMemory(runtime, &mem_before_init, 0);
    serial_outsf("WASM QUAKE: memory before init: %u bytes\n", mem_before_init);
    PROFILE_BEGIN("quake:initGame");
    result = m3_Call(init, 0, NULL);
    PROFILE_END("quake:initGame");
    if (result) {
        // Include the module's memory bounds in the failure report; this
        // distinguishes a stale/undersized module from an invalid host pointer.
        serial_outsf("WASM QUAKE: initGame trap with memory=%u bytes\n",
                     m3_GetMemorySize(runtime));
        serial_outsf("WASM QUAKE: initGame error: %s\n", result);
        screen_push_linef("WASM QUAKE: initGame error: %s", result);
        m3_FreeRuntime(runtime); m3_FreeEnvironment(env); kfree(data);
        return;
    }
    serial_outsl("WASM QUAKE: initGame complete");
    serial_outsl("WASM QUAKE: entering game loop");

    u32 mem_after_init = 0;
    m3_GetMemory(runtime, &mem_after_init, 0);
    serial_outsf("WASM QUAKE: memory after init: %u bytes\n", mem_after_init);
    u32 width = (u32)max(1, quake_call_i32(runtime, "getFrameWidth"));
    u32 height = (u32)max(1, quake_call_i32(runtime, "getFrameHeight"));
    i32 frame = quake_call_i32(runtime, "getFrameBuffer");
    u32 stride = (u32)max(1, quake_call_i32(runtime, "getFrameStride"));
    serial_outsf("WASM QUAKE: initial frame sample offset=%d value=%u\n",
                 frame, frame >= 0 ? ((u8 *)m3_GetMemory(runtime, &mem_after_init, 0))[frame] : 0);
    serial_outsf("WASM QUAKE: framebuffer offset=%d width=%u height=%u stride=%u\n",
                 frame, width, height, stride);
    screen_push_line("QUAKE: Running! Press Ctrl+C to exit...");

    typedef struct {
        u8 scancode;
        i32 quake_key;
        bool was_down;
    } quake_key_map_t;

    static quake_key_map_t key_map[] = {
        { 0x01, 27, false },   // ESC (K_ESCAPE)
        { 0x1C, 13, false },   // Enter (K_ENTER)
        { 0x39, 32, false },   // Space (K_SPACE)
        { 0x0F, 9,  false },   // Tab (K_TAB)
        { 0x0E, 127, false },  // Backspace (K_BACKSPACE)
        { 0x1D, 133, false },  // Ctrl (K_CTRL)
        { 0x38, 132, false },  // Alt (K_ALT)
        { 0x2A, 134, false },  // LShift (K_SHIFT)
        { 0x36, 134, false },  // RShift (K_SHIFT)
        { 0x48, 128, false },  // Up (K_UPARROW)
        { 0x50, 129, false },  // Down (K_DOWNARROW)
        { 0x4B, 130, false },  // Left (K_LEFTARROW)
        { 0x4D, 131, false },  // Right (K_RIGHTARROW)
        { 0x29, '`', false },  // Console tilde / backtick
        { 0x3B, 135, false },  // F1
        { 0x3C, 136, false },  // F2
        { 0x3D, 137, false },  // F3
        { 0x3E, 138, false },  // F4
        { 0x3F, 139, false },  // F5
        { 0x40, 140, false },  // F6
        { 0x41, 141, false },  // F7
        { 0x42, 142, false },  // F8
        { 0x43, 143, false },  // F9
        { 0x44, 144, false },  // F10
        { 0x57, 145, false },  // F11
        { 0x58, 146, false },  // F12
        { 0x11, 'w', false },
        { 0x1F, 's', false },
        { 0x1E, 'a', false },
        { 0x20, 'd', false },
        { 0x12, 'e', false },
        { 0x10, 'q', false },
        { 0x13, 'r', false },
        { 0x14, 't', false },
        { 0x15, 'y', false },
        { 0x16, 'u', false },
        { 0x17, 'i', false },
        { 0x18, 'o', false },
        { 0x19, 'p', false },
        { 0x21, 'f', false },
        { 0x22, 'g', false },
        { 0x23, 'h', false },
        { 0x24, 'j', false },
        { 0x25, 'k', false },
        { 0x26, 'l', false },
        { 0x2C, 'z', false },
        { 0x2D, 'x', false },
        { 0x2E, 'c', false },
        { 0x2F, 'v', false },
        { 0x30, 'b', false },
        { 0x31, 'n', false },
        { 0x32, 'm', false },
        { 0x02, '1', false },
        { 0x03, '2', false },
        { 0x04, '3', false },
        { 0x05, '4', false },
        { 0x06, '5', false },
        { 0x07, '6', false },
        { 0x08, '7', false },
        { 0x09, '8', false },
        { 0x0A, '9', false },
        { 0x0B, '0', false },
        { 0x0C, '-', false },
        { 0x0D, '=', false },
        { 0x1A, '[', false },
        { 0x1B, ']', false },
        { 0x27, ';', false },
        { 0x28, '\'', false },
        { 0x33, ',', false },
        { 0x34, '.', false },
        { 0x35, '/', false },
    };
    const int key_map_count = (int)(sizeof(key_map) / sizeof(key_map[0]));

    u32 frame_counter = 0;
    while (true) {
        // --- Keyboard input ---
        for (int i = 0; i < key_map_count; i++) {
            quake_key_map_t *km = &key_map[i];
            bool is_down = keyboard_scancode_is_pressed(km->scancode);

            if (is_down && !km->was_down) {
                km->was_down = true;
                if (key_down) {
                    const void *k_args[1] = { &km->quake_key };
                    m3_Call(key_down, 1, k_args);
                }
            } else if (!is_down && km->was_down) {
                km->was_down = false;
                if (key_up) {
                    const void *k_args[1] = { &km->quake_key };
                    m3_Call(key_up, 1, k_args);
                }
            }
        }

        u8 fg_k = keyboard_fg_eat();
        if (fg_k == KEY_CTRL_C) {
            serial_outsl("WASM QUAKE: Ctrl+C received, exiting");
            break;
        }

        mouse_event_t event;
        while (mouse_eat_event(&event)) {
            if (event.type == MOUSE_EVENT_MOVE) {
                i32 m_dx = (i32)event.dx;
                i32 m_dy = (i32)event.dy;
                const void *m_args[2] = { &m_dx, &m_dy };
                result = m3_Call(mouse_move, 2, m_args);
            } else {
                i32 btn = (event.dx == 2) ? 1 : ((event.dx == 4) ? 2 : 0);
                i32 down = event.dy ? 1 : 0;
                const void *b_args[2] = { &btn, &down };
                result = m3_Call(mouse_button, 2, b_args);
            }
            if (result) goto quake_exit;
        }

        i32 dt = 16;
        const void *dt_args[1] = { &dt };
        PROFILE_BEGIN("quake:tickGame");
        result = m3_Call(tick, 1, dt_args);
        PROFILE_END("quake:tickGame");
        if (result) {
            serial_outsf("WASM QUAKE: tick error: %s\n", result);
            break;
        }

        kern_process_t *cur_proc = sched_get_current_process();
        bool is_fg_session = (!cur_proc || !cur_proc->terminal_session || cur_proc->terminal_session == active_session);
        if (frame >= 0 && is_fg_session) {
            quake_blit(runtime, (u32)frame, width, height, stride);
        }
        if ((frame_counter++ % 60u) == 0u) {
            u32 sample_size = 0;
            u8 *sample_mem = m3_GetMemory(runtime, &sample_size, 0);
            if (sample_mem && (u32)frame < sample_size)
                serial_outsf("WASM QUAKE: frame %u sample=%u\n", frame_counter, sample_mem[frame]);
        }
        sched_yield();
    }

quake_exit:
    if (active_session) {
        active_session->owns_framebuffer = false;
    }
    m3_Call(shutdown, 0, NULL);
    screen_push_linef("WASM QUAKE: game loop stopped: %s", result ? result : "unknown");
    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    kfree(data);

    kern_process_t *proc = sched_get_current_process();
    if (proc) {
        process_exit(proc);
    }
}

void wasm_quake_launch(void *arg) {
    const char *path = arg ? (const char *)arg : "quake.wasm";
    wasm_spawn_opts_t opts = {
        .path = path,
        .foreground = true,
        .wait = false,
        .stack_kb = 512,
        .thread_entry = wasm_quake_game,
        .custom_arg = (void *)path,
    };
    i32 pid = wasm_spawn(&opts);
    if (pid < 0) {
        screen_push_linef("WASM QUAKE: Could not start %s", path);
        serial_outsf("WASM QUAKE: spawn failed for %s\n", path);
    } else {
        if (active_session) {
            active_session->owns_framebuffer = true;
        }
        screen_push_linef("QUAKE: Starting (PID %d)...", pid);
        serial_outsf("WASM QUAKE: started PID %d\n", pid);
    }
}
