#include "../include/kern_tests.h"
#include "../include/kern_mem.h"
#include "../include/kern_terminal.h"
#include "../include/kern_serial.h"
#include "../include/util_cmd.h"
#include "../util/util_str.h"
#include "../include/stbsupport.h"
#include "../include/kern_ext2.h"
#include "../include/ssfn.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_vmm.h"
#include "../include/kern_fs.h"
#include "../include/kern_sched.h"
#include "../include/wasm_spawn.h"
#include "../include/kern_profile.h"
#include "../util/str_slice.h"

#define DoM3Logging 0


#include "wasm3-0.5.0/source/m3_env.h"
#include "wasm3-0.5.0/source/m3_api_libc.h"
#include "../include/kern_ide.h"
#include "../include/kern_xhci.h"
#include "../include/kern_ipc.h"
#include "../include/kern_simd.h"

// TODO Running kmalloc2 and then kmalloc causes early page fault? I think

extern volatile u64 sw;

cmd_word_t *word;

// Doom globals
static u32 doom_frame_width = 0;
static u32 doom_frame_height = 0;
static volatile u64 doom_last_draw_time = 0;  // watchdog: tracks last drawFrame call

// Captured session reference: set by wasm_doom_test at thread start,
// checked by doom_drawFrame to skip drawing when Doom's session isn't
// the active one (user switched to another TTY via F-key).
static term_session_t *doom_session_ref = NULL;

// --- Doom profiling ---
// NOTE: sw timer has 10ms granularity — short ops (<10ms) will report as 0
#define PROFILE_EVERY_N 30  // dump stats every N frames
static u64   prof_kbd_time    = 0;  // keyboard polling (sw ticks)
static u64   prof_tick_time   = 0;  // tickGame execution (sw ticks)
static u64   prof_blit_time   = 0;  // drawFrame (sw ticks)
static u64   prof_loop_total  = 0;  // wall time per iteration (sw ticks)
static u64   prof_cpu_ticks   = 0;  // CPU actually granted to doom (run_ticks)
static u64   prof_blit_us     = 0;  // blit wall time in µs (rdtsc)
static u64   fps_cpu_ms       = 0;  // on-screen: CPU ms/frame (last 30-frame block)
static u64   fps_blit_ms      = 0;  // on-screen: blit ms/frame (last 30-frame block)

// --- Doom FPS counter (SSFN overlay, top-left) ---
// sw is in milliseconds (timer increments by 10 per 10ms tick).
#define FPS_UPDATE_MS 500   // recompute the FPS number this often
static u64 fps_frame_count = 0;
static u64 fps_last_time    = 0;
static u32 fps_current      = 0;

u0 test_lsr(char *path_arg) {
    char *path = path_arg ? path_arg : "/";

    u32 inode_no = 2;
    ext2_inode_t *start_inode = ext2_find_path(path, &inode_no);

    if (start_inode) {
        ext2_explorer_t exp;
        ext2_explorer_init(&exp, inode_no);
        ext2_explore_result_t res;

        while (ext2_explorer_next(&exp, &res)) {
            screen_push_linef("%*s|-- %s", (int) res.depth * 2, "", res.name);
        }
        ext2_explorer_deinit(&exp);
        kfree(start_inode);
    } else {
        screen_push_linef("lsr: Path not found: %s", path);
    }

    if (path_arg) kfree(path_arg);
}

extern u0 delay(u64 ms);

u0 test_ext2(u0 *arg) {
    char *path = (char *) arg;
    if (!path) return;

    i32 i = fs_open(path);

    const i32 bufSize = 4096;
    char *buf = pmallocz(bufSize + 1);

    if (i >= 0) {
        while (true) {
            i32 amt = fs_read(i, buf, bufSize);
            if (amt <= 0) break;
            buf[amt] = 0;
            screen_push_line(buf);
        }
        fs_close(i);
    } else {
        screen_push_linef("Unable to open file at path `%s`", path);
    }

    pfree(buf);
    kfree(path); // This was kmalloc'd in handle_command
}

u0 dotest(u0 *arg) {

    i32 tasks = (i32) (u64) arg;

    kern_process_t *proc = sched_get_current_process();
    kern_task_t *task = sched_get_current_task();

    i64 iterations = (i64) arg;
    if (iterations <= 0) iterations = 5;

    for (int i = 0; i < iterations; i++) {
        serial_outsf("[PID %d | TID %d] Heartbeat %d\n", proc->pid, task->tid, i);
        screen_push_linef("[PID %d | TID %d] Heartbeat %d", proc->pid, task->tid, i);
        delay(1000);
        pmalloc(4096 * 1024);
    }

    serial_outsf("[PID %d | TID %d] Exiting\n", proc->pid, task->tid);
    screen_push_linef("[PID %d | TID %d] Exiting", proc->pid, task->tid);
}

// Host imports (wasm_fd_*, wasm_lsr, wasm_get_arg_*) and wasm_test() moved to src/kernel/wasm_spawn.c.

m3ApiRawFunction(doom_onErrorMessage) {
    m3ApiGetArg(u32, msg_offset)
    m3ApiGetArg(u32, msg_len)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (mem && msg_offset + msg_len <= memory_size) {
        char *msg = kmalloc(msg_len + 1);
        if (msg) {
            mem_copy((u8 *)msg, mem + msg_offset, msg_len);
            msg[msg_len] = '\0';
            serial_outsf("DOOM ERROR: %s\n", msg);
            screen_push_linef("DOOM ERROR: %s", msg);
            kfree(msg);
        }
    }
    m3ApiSuccess();
}

m3ApiRawFunction(doom_onInfoMessage) {
    m3ApiGetArg(u32, msg_offset)
    m3ApiGetArg(u32, msg_len)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (mem && msg_offset + msg_len <= memory_size) {
        char *msg = kmalloc(msg_len + 1);
        if (msg) {
            mem_copy((u8 *)msg, mem + msg_offset, msg_len);
            msg[msg_len] = '\0';
            serial_outsf("DOOM INFO: %s\n", msg);
            screen_push_linef("DOOM INFO: %s", msg);
            kfree(msg);
        }
    }
    m3ApiSuccess();
}

m3ApiRawFunction(doom_readSaveGame) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, offset)
    m3ApiGetArg(u32, len)
    m3ApiReturn(0);
}

m3ApiRawFunction(doom_sizeOfSaveGame) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, num)
    m3ApiReturn(0);
}

m3ApiRawFunction(doom_writeSaveGame) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, offset)
    m3ApiGetArg(u32, data_offset)
    m3ApiGetArg(u32, len)
    m3ApiReturn(-1);
}

m3ApiRawFunction(doom_onGameInit) {
    m3ApiGetArg(u32, width)
    m3ApiGetArg(u32, height)
    doom_frame_width = width;
    doom_frame_height = height;
    serial_outsf("DOOM Init Game: screen size %d x %d\n", width, height);
    screen_push_linef("DOOM Init: screen size %d x %d", width, height);
    m3ApiSuccess();
}

m3ApiRawFunction(doom_readWads) {
    m3ApiGetArg(u32, wad_data_destination_offset)
    m3ApiGetArg(u32, byte_length_of_each_wad_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);

    screen_push_linef("readWads: Mem size %d, Dest %X", memory_size, wad_data_destination_offset);
    serial_outsf("DOOM: readWads called. Memory size: %d, Dest offset: %08x, Len offset: %08x\n",
                 memory_size, wad_data_destination_offset, byte_length_of_each_wad_offset);

    if (mem) {
        // Try multiple common WAD filenames
        static const char *wad_names[] = { "DOOM2.WAD", "DOOM.WAD", "DOOM1.WAD", "doom2.wad", "doom.wad", "doom1.wad" };
        i32 fd = -1;
        for (int wi = 0; wi < 6; wi++) {
            fd = fs_open(wad_names[wi]);
            if (fd >= 0) {
                serial_outsf("DOOM: Found WAD: %s\n", wad_names[wi]);
                break;
            }
        }

        if (fd >= 0) {
            u32 size = fs_size(fd);
            screen_push_linef("readWads: Opened WAD, size: %d", size);
            serial_outsf("DOOM: readWads opened WAD, size: %d bytes\n", size);

            if (wad_data_destination_offset + size <= memory_size &&
                byte_length_of_each_wad_offset + sizeof(u32) <= memory_size) {

                u64 t_wad_read_start = sw;
                serial_outsf("DOOM: Loading WAD (%d bytes)...\n", size);
                i32 bytes_read = fs_read(fd, mem + wad_data_destination_offset, size);
                serial_outsf("DOOM: WAD load took %lld ticks (~%lldms)\n", sw - t_wad_read_start, (sw - t_wad_read_start) * 10);
                *(u32*)(mem + byte_length_of_each_wad_offset) = size;

                screen_push_linef("readWads: Read %d of %d bytes", bytes_read, size);
                serial_outsf("DOOM: readWads loaded %d of %d bytes to offset %08x\n",
                             bytes_read, size, wad_data_destination_offset);
                if (bytes_read != (i32)size) {
                    screen_push_linef("WARNING: read only %d bytes", bytes_read);
                    serial_outsf("DOOM WARNING: fs_read returned %d instead of %d!\n", bytes_read, size);
                }
            } else {
                screen_push_linef("ERR: Bounds check failed!");
                serial_outsf("DOOM ERROR: readWads memory check FAILED! Needs %d bytes, memory has %d bytes (offset %d)\n",
                             size, memory_size, wad_data_destination_offset);
            }
            fs_close(fd);
        } else {
            screen_push_line("readWads: Could not open WAD");
            serial_outsl("DOOM: readWads could not open DOOM1.WAD");
        }
    }
    m3ApiSuccess();
}

m3ApiRawFunction(doom_wadSizes) {
    m3ApiGetArg(u32, number_of_wads_offset)
    m3ApiGetArg(u32, number_of_total_bytes_in_all_wads_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);

    screen_push_linef("wadSizes: Mem size %d", memory_size);
    serial_outsf("DOOM: wadSizes called. Memory size: %d\n", memory_size);

    if (mem) {
        // Try multiple common WAD filenames
        static const char *wad_names[] = { "DOOM2.WAD", "DOOM.WAD", "DOOM1.WAD", "doom2.wad", "doom.wad", "doom1.wad" };
        i32 fd = -1;
        for (int wi = 0; wi < 6; wi++) {
            fd = fs_open(wad_names[wi]);
            if (fd >= 0) {
                serial_outsf("DOOM: Found WAD: %s\n", wad_names[wi]);
                break;
            }
        }

        if (fd >= 0) {
            u32 size = fs_size(fd);
            fs_close(fd);

            if (number_of_wads_offset + sizeof(u32) <= memory_size &&
                number_of_total_bytes_in_all_wads_offset + sizeof(u32) <= memory_size) {
                *(u32*)(mem + number_of_wads_offset) = 1;
                *(u32*)(mem + number_of_total_bytes_in_all_wads_offset) = size;
                screen_push_linef("wadSizes: size %d", size);
                serial_outsf("DOOM: wadSizes reported 1 WAD, size %d bytes\n", size);
            } else {
                screen_push_line("ERR: wadSizes bounds fail");
                serial_outsl("DOOM ERROR: wadSizes memory check FAILED!");
            }
        } else {
            if (number_of_wads_offset + sizeof(u32) <= memory_size &&
                number_of_total_bytes_in_all_wads_offset + sizeof(u32) <= memory_size) {
                *(u32*)(mem + number_of_wads_offset) = 0;
                *(u32*)(mem + number_of_total_bytes_in_all_wads_offset) = 0;
            }
            screen_push_line("wadSizes: WAD not found");
            serial_outsl("DOOM: DOOM1.WAD not found, reporting 0 custom WADs");
        }
    }
    m3ApiSuccess();
}

m3ApiRawFunction(doom_timeInMilliseconds) {
    m3ApiReturnType(i64)
    m3ApiReturn((i64)sw);
}

m3ApiRawFunction(doom_drawFrame) {
    m3ApiGetArg(u32, buffer_offset)

    // Always update the watchdog, even when skipping the blit, so Doom
    // doesn't self-terminate when the user switches to another TTY.
    doom_last_draw_time = sw;

    // If Doom's session isn't the active one (user switched TTYs), skip
    // the blit entirely — don't scribble over someone else's terminal.
    if (doom_session_ref && doom_session_ref != active_session) {
        m3ApiSuccess();
    }

    // Blit Doom framebuffer directly to the real hardware framebuffer with
    // aspect-correct integer scaling for high-resolution displays.
    if (doom_frame_width > 0 && doom_frame_height > 0) {
        u64 t_blit_start = sw;
        u64 t_blit_us_start = profile_now_us();
        u32 memory_size = 0;
        u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
        if (mem) {
            display_t *disp = screen_current_display();
            if (disp && disp->trueAddress) {
                u32 *src = (u32 *)(mem + buffer_offset);
                u32 *dst = (u32 *)disp->trueAddress;
                u32 dst_pitch_px = disp->surface.pitch / 4;
                u32 src_pitch_px = doom_frame_width;
                u32 copy_h = doom_frame_height;
                u32 copy_w = doom_frame_width;

                // Calculate integer scale factor (aspect-ratio preserving)
                u32 scale_x = disp->surface.width / copy_w;
                u32 scale_y = disp->surface.height / copy_h;
                u32 scale = (scale_x < scale_y) ? scale_x : scale_y;
                if (scale < 1) scale = 1;

                u32 scaled_w = copy_w * scale;
                u32 scaled_h = copy_h * scale;

                // Center the scaled Doom frame on screen
                u32 dst_x_off = (disp->surface.width - scaled_w) / 2;
                u32 dst_y_off = (disp->surface.height - scaled_h) / 2;

                if (scale == 1) {
                    for (u32 y = 0; y < copy_h; y++) {
                        mem_copy(
                            (u8 *)(dst + (dst_y_off + y) * dst_pitch_px + dst_x_off),
                            (u8 *)(src + y * src_pitch_px),
                            copy_w * 4
                        );
                    }
                } else if (scale == 2) {
                    for (u32 y = 0; y < copy_h; y++) {
                        u32 *src_row = src + y * src_pitch_px;
                        u32 *dst_row0 = dst + (dst_y_off + y * 2) * dst_pitch_px + dst_x_off;
                        u32 *dst_row1 = dst_row0 + dst_pitch_px;
                        simd_scale_row_2x(dst_row0, dst_row1, src_row, copy_w);
                    }
                } else {
                    for (u32 y = 0; y < copy_h; y++) {
                        u32 *src_row = src + y * src_pitch_px;
                        for (u32 sy = 0; sy < scale; sy++) {
                            u32 *dst_row = dst + (dst_y_off + y * scale + sy) * dst_pitch_px + dst_x_off;
                            for (u32 x = 0; x < copy_w; x++) {
                                u32 p = src_row[x];
                                for (u32 sx = 0; sx < scale; sx++) {
                                    dst_row[x * scale + sx] = p;
                                }
                            }
                        }
                    }
                }

                prof_blit_time += (sw - t_blit_start);
                prof_blit_us += profile_now_us() - t_blit_us_start;

                // --- FPS counter overlay (top-left, SSFN) ---
                // Every drawFrame wipes the whole screen (full-frame blit),
                // so redraw the counter every frame; only recompute the
                // number every FPS_UPDATE_MS.
                fps_frame_count++;
                if (fps_last_time == 0) fps_last_time = sw;
                if (sw - fps_last_time >= FPS_UPDATE_MS) {
                    fps_current = (u32)((fps_frame_count * 1000) / (sw - fps_last_time));
                    fps_frame_count = 0;
                    fps_last_time = sw;
                }

                // Temporarily point SSFN at the real framebuffer (Doom owns
                // it directly), draw the counter, then restore the terminal's
                // backbuffer destination.
                ssfn_buf_t saved_dst = ssfn_dst;
                ssfn_dst.ptr = (u8 *)disp->trueAddress;
                ssfn_dst.w   = (i16)disp->surface.width;
                ssfn_dst.h   = (i16)disp->surface.height;
                ssfn_dst.p   = (u16)disp->surface.pitch;
                ssfn_dst.x   = 4;   // small margin from the top-left corner
                ssfn_dst.y   = 4;
                ssfn_dst.fg  = 0xFFFFFFFF;  // white text
                ssfn_dst.bg  = COLOR_BLACK; // solid box behind each glyph
                // C = CPU ms/frame actually granted by the scheduler,
                // B = blit ms/frame. If C is ~half of 1000/fps, the
                // round-robin is wasting CPU on the halting idle thread.
                char fps_buf[32];
                stbsp_snprintf(fps_buf, sizeof(fps_buf), "FPS:%u C:%llu B:%llu",
                               fps_current, fps_cpu_ms, fps_blit_ms);
                ssfn_puts(fps_buf);
                ssfn_dst = saved_dst;

                // No screen_draw() needed — we wrote directly to trueAddress
            }
        }
    }

    m3ApiSuccess();
}

// Helper: read a doom key global constant from the module
static i32 doom_read_key_global(IM3Module module, const char *name) {
    IM3Global g = m3_FindGlobal(module, name);
    if (!g) return -1;
    M3TaggedValue val;
    M3Result r = m3_GetGlobal(g, &val);
    if (r) return -1;
    return (i32)val.value.i32;
}

// Scancode → doom key mapping
// Doom scancodes come from the original DOS doom keyboard handler
// These are the standard doom key codes used by the WASM port
#define DOOM_SC_UP       0x48
#define DOOM_SC_DOWN     0x50
#define DOOM_SC_LEFT     0x4B
#define DOOM_SC_RIGHT    0x4D
#define DOOM_SC_CTRL     0x1D
#define DOOM_SC_SPACE    0x39
#define DOOM_SC_ENTER    0x1C
#define DOOM_SC_ESCAPE   0x01
#define DOOM_SC_TAB      0x0F
#define DOOM_SC_LSHIFT   0x2A
#define DOOM_SC_RSHIFT   0x36
#define DOOM_SC_ALT      0x38
#define DOOM_SC_W        0x11
#define DOOM_SC_A        0x1E
#define DOOM_SC_S        0x1F
#define DOOM_SC_D        0x20
#define DOOM_SC_Q        0x10
#define DOOM_SC_E        0x12
#define DOOM_SC_Y        0x15
#define DOOM_SC_N        0x31

// Doom key code to doom key index for the globals
// Hybrid edge+state tracking:
//   consume_down/up   → captures keys across scheduling gaps
//   was_down           → suppresses typematic repeat events
typedef struct {
    u8 scancode;
    i32 *key_val_ptr;   // points to the value of the global
    bool was_down;
} doom_key_map_t;

u0 wasm_doom_test(u0 *arg) {
    const char *wasm_path = (const char *) arg;
    if (!wasm_path) wasm_path = "doom-v0.1.0.wasm";

    IM3Environment env = null;
    IM3Runtime runtime = null;
    u8 *wasm_data = null;

    // Capture the current session at thread start to avoid a TOCTOU race
    // (the user could press F2 and session_switch before we set the flag).
    term_session_t *doom_session = active_session;
    doom_session_ref = doom_session;
    if (doom_session) doom_session->owns_framebuffer = true;

    // --- Boot-phase profiling: track elapsed time at each step ---
    // sw has ~10ms granularity; times in timer ticks (~10ms each)
    #define BOOT_LOG(step) serial_outsf("DOOM BOOT[%lld]: " step "\n", sw)

    BOOT_LOG("Opening WASM file...");
    i32 fd = fs_open(wasm_path);
    if (fd < 0) {
        screen_push_linef("WASM DOOM: Could not open %s", wasm_path);
        goto Label_Done;
    }

    u32 size = fs_size(fd);
    BOOT_LOG("Allocating WASM buffer...");
    wasm_data = kmalloc(size);
    if (!wasm_data) {
        screen_push_line("WASM DOOM: Out of memory for WASM data");
        fs_close(fd);
        goto Label_Done;
    }
    serial_outsf("DOOM BOOT: Reading %d bytes WASM...\n", size);
    fs_read(fd, wasm_data, size);
    fs_close(fd);
    BOOT_LOG("WASM file loaded.");

    BOOT_LOG("Initializing environment...");
    env = m3_NewEnvironment();
    if (!env) {
        screen_push_line("WASM DOOM: Could not create environment");
        goto Label_Done;
    }

    BOOT_LOG("Initializing runtime...");
    runtime = m3_NewRuntime(env, 512 * 1024, NULL);
    if (!runtime) {
        screen_push_line("WASM DOOM: Could not create runtime");
        goto Label_Done;
    }

    IM3Module module = NULL;
    BOOT_LOG("Parsing module...");
    M3Result result = m3_ParseModule(env, &module, wasm_data, size);
    if (result) {
        screen_push_linef("WASM DOOM: Parse error: %s", result);
        goto Label_Done;
    }

    if (!module) {
        screen_push_line("WASM DOOM: Parsing failed, module is NULL");
        goto Label_Done;
    }

    BOOT_LOG("Loading module...");
    result = m3_LoadModule(runtime, module);
    if (result) {
        screen_push_linef("WASM DOOM: Load error: %s", result);
        goto Label_Done;
    }

    // Link the 10 custom Doom imports
    BOOT_LOG("Linking Doom imports...");
    m3_LinkRawFunction(module, "console", "onErrorMessage", "v(ii)", &doom_onErrorMessage);
    m3_LinkRawFunction(module, "console", "onInfoMessage", "v(ii)", &doom_onInfoMessage);
    m3_LinkRawFunction(module, "gameSaving", "readSaveGame", "i(ii)", &doom_readSaveGame);
    m3_LinkRawFunction(module, "gameSaving", "sizeOfSaveGame", "i(i)", &doom_sizeOfSaveGame);
    m3_LinkRawFunction(module, "gameSaving", "writeSaveGame", "i(iii)", &doom_writeSaveGame);
    m3_LinkRawFunction(module, "loading", "onGameInit", "v(ii)", &doom_onGameInit);
    m3_LinkRawFunction(module, "loading", "readWads", "v(ii)", &doom_readWads);
    m3_LinkRawFunction(module, "loading", "wadSizes", "v(ii)", &doom_wadSizes);
    m3_LinkRawFunction(module, "runtimeControl", "timeInMilliseconds", "I()", &doom_timeInMilliseconds);
    m3_LinkRawFunction(module, "ui", "drawFrame", "v(i)", &doom_drawFrame);

    // Doom also needs the standard file-descriptor host imports.
    // (These live in src/kernel/wasm_spawn.c now; we call them by name.)
    m3_LinkRawFunction(module, "env", "fd_open",  "i(i)",    &wasm_fd_open);
    m3_LinkRawFunction(module, "env", "fd_read",  "i(iii)",  &wasm_fd_read);
    m3_LinkRawFunction(module, "env", "fd_close", "i(i)",    &wasm_fd_close);
    m3_LinkRawFunction(module, "env", "fd_write", "i(iiii)", &wasm_fd_write);

    BOOT_LOG("Linking LibC...");
    result = m3_LinkLibC(module);
    if (result) {
        screen_push_linef("WASM DOOM: Link error: %s", result);
    }

    // Read global key constants from the WASM module
    BOOT_LOG("Reading key globals...");
    static i32 k_up = 0, k_down = 0, k_left = 0, k_right = 0;
    static i32 k_fire = 0, k_use = 0, k_enter = 0, k_escape = 0;
    static i32 k_tab = 0, k_shift = 0, k_alt = 0, k_strafe_l = 0, k_strafe_r = 0;

    static i32 k_y = 0, k_n = 0, k_backspace = 0;
    static i32 k_num[10] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9' };

    k_up     = doom_read_key_global(module, "KEY_UPARROW");
    k_down   = doom_read_key_global(module, "KEY_DOWNARROW");
    k_left   = doom_read_key_global(module, "KEY_LEFTARROW");
    k_right  = doom_read_key_global(module, "KEY_RIGHTARROW");
    k_fire   = doom_read_key_global(module, "KEY_FIRE");
    k_use    = doom_read_key_global(module, "KEY_USE");
    k_enter  = doom_read_key_global(module, "KEY_ENTER");
    k_escape = doom_read_key_global(module, "KEY_ESCAPE");
    k_tab    = doom_read_key_global(module, "KEY_TAB");
    k_shift  = doom_read_key_global(module, "KEY_SHIFT");
    k_alt    = doom_read_key_global(module, "KEY_ALT");
    k_strafe_l = doom_read_key_global(module, "KEY_STRAFE_L");
    k_strafe_r = doom_read_key_global(module, "KEY_STRAFE_R");
    k_y      = doom_read_key_global(module, "KEY_Y");
    k_n      = doom_read_key_global(module, "KEY_N");
    k_backspace = doom_read_key_global(module, "KEY_BACKSPACE");

    // Fallback: if the module doesn't export KEY_Y/KEY_N, use ASCII values
    // Doom's menu quit confirmation checks for 'y'/'Y' (ASCII), not raw scancodes
    if (k_y < 0) k_y = 'y';
    if (k_n < 0) k_n = 'n';
    if (k_backspace < 0) k_backspace = 0x0E;  // PS/2 backspace scancode

    serial_outsf("DOOM Keys: Up=%d Down=%d Left=%d Right=%d Fire=%d Use=%d Esc=%d\n",
                 k_up, k_down, k_left, k_right, k_fire, k_use, k_escape);

    // Set up scancode-to-key mapping table
    doom_key_map_t key_map[] = {
        { DOOM_SC_UP,     &k_up,     false },
        { DOOM_SC_DOWN,   &k_down,   false },
        { DOOM_SC_LEFT,   &k_left,   false },
        { DOOM_SC_RIGHT,  &k_right,  false },
        { DOOM_SC_CTRL,   &k_fire,   false },
        { DOOM_SC_SPACE,  &k_use,    false },
        { DOOM_SC_ENTER,  &k_enter,  false },
        { DOOM_SC_ESCAPE, &k_escape, false },
        { DOOM_SC_TAB,    &k_tab,    false },
        { DOOM_SC_LSHIFT, &k_shift,  false },
        { DOOM_SC_RSHIFT, &k_shift,  false },
        { DOOM_SC_ALT,    &k_alt,    false },
        { DOOM_SC_W,      &k_up,     false },
        { DOOM_SC_A,      &k_left,   false },
        { DOOM_SC_S,      &k_down,   false },
        { DOOM_SC_D,      &k_right,  false },
        { DOOM_SC_E,      &k_strafe_r, false },
        { DOOM_SC_Q,      &k_strafe_l, false },
        { DOOM_SC_Y,      &k_y,         false },
        { DOOM_SC_N,      &k_n,         false },
        { 0x0E,           &k_backspace, false },  // Backspace key (menu navigation)
        { 0x02,           &k_num[1],    false },  // '1'
        { 0x03,           &k_num[2],    false },  // '2'
        { 0x04,           &k_num[3],    false },  // '3'
        { 0x05,           &k_num[4],    false },  // '4'
        { 0x06,           &k_num[5],    false },  // '5'
        { 0x07,           &k_num[6],    false },  // '6'
        { 0x08,           &k_num[7],    false },  // '7'
        { 0x09,           &k_num[8],    false },  // '8'
        { 0x0A,           &k_num[9],    false },  // '9'
        { 0x0B,           &k_num[0],    false },  // '0'
    };
    const int key_map_count = sizeof(key_map) / sizeof(key_map[0]);

    // Find keyboard input functions
    IM3Function report_keydown_func = null;
    IM3Function report_keyup_func = null;
    if (m3_FindFunction(&report_keydown_func, runtime, "reportKeyDown")) {
        report_keydown_func = null;
    }
    if (m3_FindFunction(&report_keyup_func, runtime, "reportKeyUp")) {
        report_keyup_func = null;
    }
    serial_outsf("DOOM: reportKeyDown=%p reportKeyUp=%p\n", report_keydown_func, report_keyup_func);

    IM3Function init_func;
    result = m3_FindFunction(&init_func, runtime, "initGame");
    if (result) {
        screen_push_linef("WASM DOOM: Cannot find initGame: %s", result);
        goto Label_Done;
    }

    IM3Function tick_func;
    result = m3_FindFunction(&tick_func, runtime, "tickGame");
    if (result) {
        screen_push_linef("WASM DOOM: Cannot find tickGame: %s", result);
        goto Label_Done;
    }

    serial_outsl("WASM DOOM: Calling initGame()...");
    screen_push_line("WASM DOOM: Calling initGame()...");
    BOOT_LOG("Calling initGame()...");
    result = m3_Call(init_func, 0, NULL);
    BOOT_LOG("initGame() complete.");
    if (result) {
        screen_push_linef("WASM DOOM: initGame error: %s", result);
        goto Label_Done;
    }

    serial_outsl("WASM DOOM: Entering game loop...");
    screen_push_line("DOOM: Running! Press ESC to exit...");

    // Clear the real framebuffer to black (we draw directly, so backbuffer clear won't help)
    {
        display_t *disp = screen_current_display();
        if (disp && disp->trueAddress) {
            mem_set32((u32 *)disp->trueAddress, COLOR_BLACK,
                      disp->surface.pitch * disp->surface.height / 4);
        }
    }

    // CPU accounting: snapshot our task's run_ticks so we can measure how
    // much CPU the scheduler actually grants us vs. wall time (a round-robin
    // that alternates with a halting idle thread gives us only ~50%).
    kern_task_t *doom_task = sched_get_current_task();
    u64 last_cpu_ticks = doom_task ? doom_task->run_ticks : 0;

    while (true) {
        u64 t_loop_start = sw;

        // --- Keyboard input: edge-detect with typematic suppression ---
        // Uses consume_down / consume_up instead of state-based is_pressed,
        // so a key pressed-and-released between Doom's timeslices is
        // still captured (the ISR sets edge flags that persist until
        // consumed).  The was_down guard suppresses typematic repeats
        // (the PS/2 controller sends make codes repeatedly while held).
        u64 t_kbd_start = sw;
        for (int i = 0; i < key_map_count; i++) {
            doom_key_map_t *km = &key_map[i];
            bool edge_down = keyboard_scancode_consume_down(km->scancode);
            bool edge_up   = keyboard_scancode_consume_up(km->scancode);

            if (edge_down && !km->was_down) {
                km->was_down = true;
                if (report_keydown_func && *km->key_val_ptr >= 0) {
                    const void *args[1] = { km->key_val_ptr };
                    m3_Call(report_keydown_func, 1, args);
                }
            }
            if (edge_up && km->was_down) {
                km->was_down = false;
                if (report_keyup_func && *km->key_val_ptr >= 0) {
                    const void *args[1] = { km->key_val_ptr };
                    m3_Call(report_keyup_func, 1, args);
                }
            }
        }
        prof_kbd_time += (sw - t_kbd_start);

        // --- Run one game tick ---
        u64 t_tick_start = sw;
        result = m3_Call(tick_func, 0, NULL);
        prof_tick_time += (sw - t_tick_start);

        if (result) {
            screen_push_linef("WASM DOOM: tickGame error: %s", result);
            serial_outsf("WASM DOOM: tickGame error: %s\n", result);
            break;
        }

        prof_loop_total += (sw - t_loop_start);

        // CPU accounting: how much CPU the scheduler granted us this frame.
        if (doom_task) {
            u64 now_ticks = doom_task->run_ticks;
            prof_cpu_ticks += now_ticks - last_cpu_ticks;
            last_cpu_ticks = now_ticks;
        }

        // Dump profiling stats every N frames (times in timer ticks, ~10ms each)
        // and yield every ~10 frames so the main loop can service keyboard
        // (TTY switching). Most multitasking is handled by the preemptive
        // scheduler; this infrequent yield is a safety valve.
        {
            static u32 prof_frame_count = 0;
            prof_frame_count++;
            if (prof_frame_count % PROFILE_EVERY_N == 0) {
                serial_outsf(
                    "PROFILE[%d frames]: wall=%-4lldms cpu=%-4lldms blit=%-4lldms\n",
                    PROFILE_EVERY_N,
                    prof_loop_total,           // wall time (all ticks, whoever ran)
                    prof_cpu_ticks * 10,       // CPU actually granted to doom
                    prof_blit_us / 1000        // blit wall time
                );
                fps_cpu_ms  = (prof_cpu_ticks * 10) / PROFILE_EVERY_N;
                fps_blit_ms = (prof_blit_us / 1000) / PROFILE_EVERY_N;
                prof_kbd_time = 0;
                prof_tick_time = 0;
                prof_blit_time = 0;
                prof_loop_total = 0;
                prof_cpu_ticks = 0;
                prof_blit_us = 0;
            }
            if (prof_frame_count % 10 == 0) {
                sched_yield();
            }
        }
    }

    screen_push_line("WASM DOOM: Game loop exited.");
    serial_outsl("WASM DOOM: Game loop exited.");

Label_Done:
    #undef BOOT_LOG
    keyboard_flush_queue();  // clear leftover extended key codes from Doom gameplay
    if (doom_session) doom_session->owns_framebuffer = false;
    doom_frame_width = 0;
    doom_frame_height = 0;
    if (runtime) m3_FreeRuntime(runtime);
    if (env) m3_FreeEnvironment(env);
    if (wasm_data) kfree(wasm_data);
    if (arg) kfree(arg);
}

u0 handle_command() {
    char workingbuf[256] = {0};
    u64 add = PAGE_SIZE * 1000;

    if (typingbuf[0] == '\0') return;

    word = cmd_parse(typingbuf, kmalloc);
    if (!word) return;

    serial_outsf("[[%s]]\n", word->loc);

    if (cmd_word_eq(word, "proc")) {
        PROFILE_SCOPE("cmd:proc");
        kern_task_t *head = sched_get_task_list_head();
        if (!head) {
            screen_push_line("No tasks found.");
            goto Label_Free;
        }

        screen_push_line("PROCESSES:");
        screen_push_line("PID  CR3               HEAP_VPTR     MEM (KB)  THREADS");
        screen_push_line("---  ---               ---------     ---       -------");

        kern_task_t *curr = head;
        kern_process_t *visited[256] = {0};
        int visited_count = 0;

        do {
            kern_process_t *proc = curr->process;
            if (proc) {
                bool already_visited = false;
                for (int i = 0; i < visited_count; i++) {
                    if (visited[i] == proc) {
                        already_visited = true;
                        break;
                    }
                }

                if (!already_visited && visited_count < 256) {
                    visited[visited_count++] = proc;

                    int thread_count = 0;
                    kern_task_t *t_curr = head;
                    do {
                        if (t_curr->process == proc) thread_count++;
                        t_curr = t_curr->next;
                    } while (t_curr != head);

                    u64 total_mem = 0;
                    kern_mem_region_t *region = proc->mem_regions;
                    while (region) {
                        total_mem += region->page_count * PAGE_SIZE;
                        region = region->next;
                    }

                    screen_push_linef("%-3d  %016llx  %012llx  %-8lld  %d",
                                      proc->pid, proc->cr3, proc->heap_vptr, total_mem / 1024, thread_count);
                }
            }
            curr = curr->next;
        } while (curr != head);

        screen_push_line("");

        screen_push_line("THREADS:");
        screen_push_line("TID  PID  STATE    RSP");
        screen_push_line("---  ---  -----    ---");

        curr = head;
        do {
            const char *state_str = "UNKNOWN";
            switch (curr->state) {
                case TASK_STATE_READY:
                    state_str = "READY";
                    break;
                case TASK_STATE_RUNNING:
                    state_str = "RUNNING";
                    break;
                case TASK_STATE_BLOCKED:
                    state_str = "BLOCKED";
                    break;
                case TASK_STATE_DEAD:
                    state_str = "DEAD";
                    break;
            }

            screen_push_linef("%-3d  %-3d  %-7s  %016llx",
                              curr->tid, curr->process ? curr->process->pid : -1, state_str, curr->rsp);

            curr = curr->next;
        } while (curr != head);

        goto Label_Free;
    }

    if (cmd_word_eq(word, "cpu")) {
        PROFILE_SCOPE("cmd:cpu");
        extern u64 rdmsr(u32 msr);
        extern u0 delay(u64 ms);

        u32 eax, ebx, ecx, edx;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(6));

        screen_push_line("=== CPU CLOCK & POWER DIAGNOSTICS ===");
        screen_push_linef("Base TSC Estimate: ~%llu MHz", profile_tsc_mhz());

        // Check HWP (Intel Speed Shift)
        if (eax & (1 << 7)) {
            u64 pm_en = rdmsr(0x770);
            u64 hwp_req = rdmsr(0x774);
            u64 hwp_cap = rdmsr(0x771);

            u8 highest = hwp_cap & 0xFF;
            u8 guaranteed = (hwp_cap >> 8) & 0xFF;
            u8 lowest = (hwp_cap >> 24) & 0xFF;

            u8 min_perf = hwp_req & 0xFF;
            u8 max_perf = (hwp_req >> 8) & 0xFF;
            u8 epp = (hwp_req >> 24) & 0xFF;

            screen_push_linef("Intel HWP: ENABLED (PM_EN=%d)", (int)(pm_en & 1));
            screen_push_linef("HWP Caps: Max=%dx (%dMHz), Base=%dx (%dMHz), Min=%dx (%dMHz)",
                              highest, highest * 100, guaranteed, guaranteed * 100, lowest, lowest * 100);
            screen_push_linef("HWP Req:  Min=%d, Max=%d, EPP=%d (%s)",
                              min_perf, max_perf, epp,
                              epp == 0 ? "Performance" : (epp == 128 ? "Balanced" : "PowerSave"));
        } else {
            screen_push_line("Intel HWP (Speed Shift): Not Supported / Disabled");
        }

        // Current multiplier from IA32_PERF_STATUS (0x198)
        u64 perf_stat = rdmsr(0x198);
        u8 cur_mult = (perf_stat >> 8) & 0xFF;
        screen_push_linef("IA32_PERF_STATUS: multiplier %dx (~%d MHz)", cur_mult, cur_mult * 100);

        // Platform limits from MSR_PLATFORM_INFO (0xCE)
        u64 plat_info = rdmsr(0xCE);
        u8 max_non_turbo = (plat_info >> 8) & 0xFF;
        u8 min_lfm = (plat_info >> 48) & 0xFF;
        screen_push_linef("Platform Info: Max Non-Turbo=%dx (%dMHz), Min LFM=%dx (%dMHz)",
                          max_non_turbo, max_non_turbo * 100, min_lfm, min_lfm * 100);

        // Thermal / Throttle status from IA32_THERM_STATUS (0x19C) & IA32_PACKAGE_THERM_STATUS (0x1B1)
        u64 therm_core = rdmsr(0x19C);
        u64 therm_pkg  = rdmsr(0x1B1);
        u64 pwr_ctl    = rdmsr(0x1FC);

        bool prochot_core = (therm_core & 1) != 0;
        bool pwr_limit_core = ((therm_core >> 10) & 1) != 0;
        bool prochot_pkg = (therm_pkg & 1) != 0;
        bool pwr_limit_pkg = ((therm_pkg >> 10) & 1) != 0;
        bool bd_prochot = (pwr_ctl & 1) != 0;

        screen_push_linef("Throttling: PROCHOT=%d, PowerLimit=%d (Pkg: PROCHOT=%d, PwrLim=%d)",
                          prochot_core, pwr_limit_core, prochot_pkg, pwr_limit_pkg);
        screen_push_linef("MSR_POWER_CTL (0x1FC): %016llx (BD_PROCHOT=%d)", pwr_ctl, bd_prochot);

        // Stress test under active compute load to trigger HWP dynamic turbo ramp
        if (ecx & 1) {
            u64 m0 = rdmsr(0xE7);
            u64 a0 = rdmsr(0xE8);

            // 50 million iterations of integer arithmetic
            volatile u64 dummy = 0;
            for (u64 i = 0; i < 50000000ULL; i++) {
                dummy += i;
            }

            u64 m1 = rdmsr(0xE7);
            u64 a1 = rdmsr(0xE8);
            u64 stat_loaded = rdmsr(0x198);
            u8 loaded_mult = (stat_loaded >> 8) & 0xFF;

            u64 mdiff = m1 - m0;
            u64 ratio = mdiff ? ((a1 - a0) * 100) / mdiff : 0;
            u64 eff_mhz = (profile_tsc_mhz() * ratio) / 100;
            screen_push_linef("Under Load (50M ops): Mult=%dx (%dMHz), Ratio=%llu%% (~%llu MHz)",
                              loaded_mult, loaded_mult * 100, ratio, eff_mhz);
        }

        goto Label_Free;
    }

    if (cmd_word_eq(word, "doom")) {
        PROFILE_BEGIN("cmd:doom");
        // Doom has a custom game loop (initGame + tickGame) that doesn't fit
        // the normal wasm_thread_entry -> _start convention. Use thread_entry
        // to have wasm_spawn handle process/foreground setup while doom
        // provides its own loader (wasm_doom_test) with custom imports.
        char *path = null;
        if (word->next != null) {
            path = str_view_to_c(word->next->text);
        } else {
            path = str_view_to_c(STR_VIEW_LIT("doom-v0.1.0.wasm"));
        }

        wasm_spawn_opts_t opts = {
            .path = path,
            .foreground = true,
            .thread_entry = wasm_doom_test,
            .custom_arg = path,
        };
        if (wasm_spawn(&opts) < 0) {
            // wasm_spawn printed diagnostics; path wasn't consumed
            if (path) kfree(path);
        }

        PROFILE_END("cmd:doom");
        // On success, wasm_doom_test owns `path` and frees it in Label_Done.
        goto Label_Free;
    }

    if(cmd_word_eq(word, "write") && word->next && word->next->next){
        PROFILE_SCOPE("cmd:write");
        i32 fd = fs_open_view(word->next->text);
        cmd_word_t *w = word->next->next;
        fs_write(fd, w->loc, str_len(w->loc));

        fs_close(fd);
    }

    if(cmd_word_eq(word, "touch") && word->next) {
        PROFILE_SCOPE("cmd:touch");
        i32 fd = fs_create(word->next->loc);
        if (fd >= 0) fs_close(fd);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "wasm")) {
        PROFILE_SCOPE("cmd:wasm");
        // `wasm <file>` runs an arbitrary .wasm with the common host import set.
        // argv = ["wasm", file], no foreground, no wait (fire-and-forget).
        char *path = null;
        if (word->next != null) {
            path = str_view_to_c(word->next->text);
        }
        char *argv[2] = { "wasm", path };
        int argc = path ? 2 : 1;
        wasm_spawn_opts_t opts = {
            .path = path,
            .argc = argc,
            .argv = (char *const *) argv,
            .foreground = false,
            .wait = false,
        };
        wasm_spawn(&opts);
        if (path) kfree(path);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "doxw")) {
        // Stress test: spawn file_test.wasm N times. No foreground, no wait
        // (fire-and-forget); we don't track individual PIDs here.
        PROFILE_SCOPE("cmd:doxw");
        i64 count = 10;
        if (word->next && word->next->val_type == CMD_WT_i64) {
            count = word->next->val_i64;
        }

        wasm_spawn_opts_t opts = {
            .path = "file_test.wasm",
            .argc = 0,
            .argv = null,
            .foreground = false,
            .wait = false,
        };

        while (count--) {
            if (wasm_spawn(&opts) < 0) {
                screen_push_line("doxw: Failed to spawn (OOM)");
                break;
            }
        }

        goto Label_Free;
    }

    if (cmd_word_eq(word, "dox")) {
        PROFILE_SCOPE("cmd:dox");
        i64 count = 10;
        if (word->next && word->next->val_type == CMD_WT_i64) {
            count = word->next->val_i64;
        }

        while (count--) {
            kern_process_t *new_proc = process_create();
            sched_create_process_thread(new_proc, dotest, (u0 *) 5);
        }

        goto Label_Free;
    }

    if (cmd_word_eq(word, "do")) {
        PROFILE_SCOPE("cmd:do");
        i64 val = 5;
        if (word->next && word->next->val_type == CMD_WT_i64) {
            val = word->next->val_i64;
        }

        kern_process_t *new_proc = process_create();
        sched_create_process_thread(new_proc, dotest, (u0 *) val);
        goto Label_Free;
    }


    if (cmd_word_eq(word, "kmalloc")) {
        PROFILE_SCOPE("cmd:kmalloc");
        u64 size = 0;
        serial_outs("Testing Kmalloc and page fault handling.\n");
        i64 inc = 0;
        while (1) {
            size += add;
            kmalloc(add);
            stbsp_snprintf(workingbuf, 255, "%lldB\n", size);
            serial_outsf("%lldB\n", size);
            if (inc % 2 == 0) {
                screen_puts_r(workingbuf, V2I(0, font_height * 2), COLOR_WHITE, COLOR_BLACK);
                screen_draw();
            }
            ++inc;
        }
        goto Label_Free;
    }

    if (cmd_word_eq(word, "kill") && word->next) {
        PROFILE_SCOPE("cmd:kill");
        if (word->next->val_type != CMD_WT_i64) {
            screen_push_line("Argument is not a valid i64 value");
        } else {
            i32 pid = (i32) word->next->val_i64;
            if (sched_kill_process(pid)) {
                screen_push_linef("Process %d marked for termination", pid);
            } else {
                screen_push_linef("Process %d not found or protected", pid);
            }
        }
        goto Label_Free;
    }


    if (cmd_word_eq(word, "cls")) {
        PROFILE_SCOPE("cmd:cls");
        typingbuf[0] = 0;
        screen_terminal_clear();
        goto Label_Free;
    }

    if (cmd_word_eq(word, "lsr")) {
        PROFILE_SCOPE("cmd:lsr");
        // `lsr [path]` — recursive dir listing via the foreground lsr.wasm.
        char *path = null;
        if (word->next != null) {
            path = str_view_to_c(word->next->text);
        }
        char *argv[2] = { "lsr", path };
        int argc = path ? 2 : 1;
        wasm_spawn_opts_t opts = {
            .path = "lsr.wasm",
            .argc = argc,
            .argv = (char *const *) argv,
            .foreground = true,
            .wait = true,
        };
        wasm_spawn(&opts);
        if (path) kfree(path);
        goto Label_Free;
    }


    // ── cd: change directory / switch drives ────────────────────────────
    if (cmd_word_eq(word, "cd")) {
        PROFILE_SCOPE("cmd:cd");

        if (word->next == null) {
            screen_push_linef("cwd: %s", cwd);
            goto Label_Free;
        }

        char *target = str_view_to_c(word->next->text);
        if (!target) goto Label_Free;

        // `cd //` — go to synthetic root (just prints drives via ls)
        if (target[0] == '/' && target[1] == '/' && target[2] == '\0') {
            screen_push_line("cd: use ls // to list drives");
            kfree(target);
            goto Label_Free;
        }

        // Resolve the path to validate it exists and is a directory
        u32 inode_no = 2;
        ext2_inode_t *dir_inode = ext2_find_path(target, &inode_no);
        if (!dir_inode) {
            screen_push_linef("cd: path not found: %s", target);
            kfree(target);
            goto Label_Free;
        }
        if ((dir_inode->mode & 0xF000) != 0x4000) {
            screen_push_linef("cd: not a directory: %s", target);
            kfree(dir_inode);
            kfree(target);
            goto Label_Free;
        }
        kfree(dir_inode);

        // Update cwd
        u32 tlen = str_len(target);
        if (tlen >= 255) tlen = 254;
        mem_copy((u8*)cwd, (u8*)target, tlen);
        cwd[tlen] = '\0';

        kfree(target);
        goto Label_Free;
    }

    // ── ls: spawn ls.wasm (flat listing) or list drives ──────────────────
    if (cmd_word_eq(word, "ls")) {
        PROFILE_SCOPE("cmd:ls");

        char *ls_path = null;
        if (word->next != null) {
            ls_path = str_view_to_c(word->next->text);
        } else {
            ls_path = str_dup(cwd, kmalloc);
        }
        if (!ls_path) goto Label_Free;

        // `ls //` — list drives (handled in-kernel)
        if (ls_path && ls_path[0] == '/' && ls_path[1] == '/' && ls_path[2] == '\0') {
            screen_push_line("DRIVES:");
            screen_push_line("NAME  TYPE      STATUS");
            screen_push_line("----  ----      ------");
            for (u8 i = 0; i < MAX_DRIVES; i++) {
                if (!drives[i].present) continue;
                const char *sel_name = (drives[i].backend == DRIVE_BACKEND_RAMDISK) ? "RAMDISK" :
                                       (drives[i].ide_drive_sel == IDE_DRIVE_MASTER) ? "MASTER" : "SLAVE";
                const char *active = (&drives[i] == active_drive) ? " < active" : "";
                screen_push_linef("%-4s  %-8s  %s%s", drives[i].name, sel_name,
                                  drives[i].present ? "ok" : "--", active);
            }
            if (ls_path) kfree(ls_path);
            goto Label_Free;
        }

        // Spawn ls.wasm with the path as argv[1]
        char *argv[2] = { "ls", ls_path };
        wasm_spawn_opts_t opts = {
            .path = "ls.wasm",
            .argc = 2,
            .argv = (char *const *) argv,
            .foreground = true,
            .wait = true,
        };
        wasm_spawn(&opts);
        if (ls_path) kfree(ls_path);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "open") && word->next != null) {
        PROFILE_SCOPE("cmd:open");
        str_view_t path_sv = word->next->text;

        i32 w = fs_open_view(path_sv);

        if (w < 0) {
            screen_push_linef("Failed to open file at `%.*s`", (int)path_sv.len, path_sv.data);
            goto Label_Free;
        }

        fs_seek(w, 0, SEEK_END);
        i32 len = fs_tell(w);
        fs_seek(w, 0, SEEK_SET);


        char *dat = kmalloc(len + 1);
        fs_read(w, dat, len);
        dat[len] = '\0';
        fs_close(w);

        screen_push_line(dat);

        kfree(dat);

        goto Label_Free;
    }

    if (cmd_word_eq(word, "ext2") && word->next != null) {
        PROFILE_SCOPE("cmd:ext2");
        char *path = str_view_to_c(word->next->text);
        kern_process_t *proc = process_create();
        sched_create_process_thread(proc, test_ext2, path);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "wat2wasm")) {
        PROFILE_SCOPE("cmd:wat2wasm");
        // `wat2wasm <input.wat> <output.wasm>` — compile WAT to WASM.
        // Also supports: `wat2wasm <input.wat> -o <output.wasm>`
        // Runs natively via wasm2c (embedded wabt), no wasm3 required.
        char *input = null, *output = null;
        if (word->next != null) {
            input = str_view_to_c(word->next->text);
        }
        // Check for optional "-o" flag before the output filename
        cmd_word_t *out_word = word->next ? word->next->next : null;
        if (out_word != null && cmd_word_eq(out_word, "-o")) {
            out_word = out_word->next;  // skip "-o", point to actual filename
        }
        if (out_word != null) {
            output = str_view_to_c(out_word->text);
        }
        if (!input || !output) {
            screen_push_line("Usage: wat2wasm <input.wat> <output.wasm>");
            if (input) kfree(input);
            if (output) kfree(output);
            goto Label_Free;
        }

        // Run the native wasm2c compiler inline (synchronous, no wasm3/process)
        screen_push_linef("wat2wasm: compiling %s -> %s", input, output);
        char *argv_native[4] = { "wat2wasm", input, "-o", output };
        wat2wasm_native(4, argv_native);

        kfree(input);
        kfree(output);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "fileinfo") && word->next != null) {
        PROFILE_SCOPE("cmd:fileinfo");
        str_view_t path_sv = word->next->text;
        i32 fd = fs_open_view(path_sv);
        if (fd < 0) {
            screen_push_linef("fileinfo: could not open %.*s", (int)path_sv.len, path_sv.data);
        } else {
            u32 size = fs_size(fd);
            screen_push_linef("fileinfo: %.*s, size = %u bytes", (int)path_sv.len, path_sv.data, size);
            u8 buf[16];
            i32 r = fs_read(fd, buf, 16);
            if (r > 0) {
                screen_push_linef("  first bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                                  buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            }
            if (size > 16) {
                fs_seek(fd, size - 16, SEEK_SET);
                r = fs_read(fd, buf, 16);
                if (r > 0) {
                    screen_push_linef("  last bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                                      buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
                }
            }
            fs_close(fd);
        }
        goto Label_Free;
    }

    if (cmd_word_eq(word, "cat")) {
        PROFILE_SCOPE("cmd:cat");
        // `cat [path]` — foreground cat.wasm.
        char *path = null;
        if (word->next != null) {
            path = str_view_to_c(word->next->text);
        }
        char *argv[2] = { "cat", path };
        int argc = path ? 2 : 1;
        wasm_spawn_opts_t opts = {
            .path = "cat.wasm",
            .argc = argc,
            .argv = (char *const *) argv,
            .foreground = true,
            .wait = true,
        };
        wasm_spawn(&opts);
        if (path) kfree(path);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "write") && word->next != null && word->next->next != null) {
        PROFILE_SCOPE("cmd:write");
        str_view_t path_sv = word->next->text;
        char *str  = str_view_to_c(word->next->next->text);

        i32 fd = fs_open_view(path_sv);
        if (fd >= 0) {
            i32 written = fs_write(fd, (u8 *) str, word->next->next->len);
            if (written >= 0) {
                screen_push_linef("Wrote %d bytes to %.*s", written, (int)path_sv.len, path_sv.data);
            } else {
                screen_push_line("Error writing to file");
            }
            fs_close(fd);
        } else {
            screen_push_linef("Could not open: %.*s", (int)path_sv.len, path_sv.data);
        }

        kfree(str);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "kmalloc2")) {
        PROFILE_SCOPE("cmd:kmalloc2");
        u64 sum = 0;
        serial_outs("Testing Kmalloc and freeing.\n");
        while (sum < system.total_mem_size * 4) {
            sum += add;
            void *dat = kmalloc(add);
            if (!dat) break;
            mem_set(dat, COLOR_MAGENTA, add);
            kfree(dat);
            stbsp_snprintf(workingbuf, 255, "%lld\n", sum);
            serial_outs(workingbuf);
        }
        goto Label_Free;
    }


    // ── cache: query or resize the ext2 block cache ───────────────────
    if (cmd_word_eq(word, "cache")) {
        PROFILE_SCOPE("cmd:cache");
        u32 hits = 0, misses = 0, used = 0, cap = 0;
        block_cache_stats(&hits, &misses, &used, &cap);
        u32 block_size = active_drive->block_size;

        if (word->next && word->next->val_type == CMD_WT_i64) {
            u32 new_cap = (u32) word->next->val_i64;
            block_cache_set_capacity(new_cap);
            block_cache_stats(&hits, &misses, &used, &cap);
            screen_push_linef("cache: resized to %u entries (~%u KB)",
                              cap, (cap * block_size) / 1024);
        } else {
            u32 total = hits + misses;
            u32 pct = total > 0 ? (hits * 100) / total : 0;
            screen_push_linef("cache: %u/%u used, %u hits / %u misses (%u%% hit rate)",
                              used, cap, hits, misses, pct);
        }
        goto Label_Free;
    }

    if (cmd_word_eq(word, "pci")) {
        PROFILE_SCOPE("cmd:pci");
        pci_device_t *dev = system.pci_list_head;
        while (dev) {
            screen_push_linef("C:%X S:%X | V:%X D:%X\n",
                              dev->class_code, dev->subclass,
                              dev->vendor_id, dev->device_id);
            dev = dev->next;
        }
        goto Label_Free;
    }

    if (cmd_word_eq(word, "usb")) {
        PROFILE_SCOPE("cmd:usb");
        xhci_list_devices();
        goto Label_Free;
    }

    if (cmd_word_eq(word, "ipc")) {
        PROFILE_SCOPE("cmd:ipc");
        // Spawn sender + receiver, create shmem, wire them together.
        // Type into the shell — sender reads stdin byte-by-byte, writes
        // each byte to shared memory, signals receiver. Receiver prints
        // to stdout. On EOF (Ctrl+D not available, use empty read or
        // switch session to kill), sender sends TERM, both exit.

        // 1. Spawn sender as foreground (so keyboard input routes to it)
        //    and receiver as background (it only writes to stdout).
        wasm_spawn_opts_t s_opts = {
            .path = "ipc_sender.wasm",
            .foreground = true,
            .wait = false,
        };
        i32 pid_s = wasm_spawn(&s_opts);
        if (pid_s < 0) {
            screen_push_line("ipc: failed to spawn sender");
            goto Label_Free;
        }

        wasm_spawn_opts_t r_opts = {
            .path = "ipc_receiver.wasm",
            .foreground = false,
            .wait = false,
        };
        i32 pid_r = wasm_spawn(&r_opts);
        if (pid_r < 0) {
            screen_push_line("ipc: failed to spawn receiver");
            goto Label_Free;
        }

        // 2. Get process pointers.
        kern_task_t *t_s = sched_get_by_pid(pid_s);
        kern_task_t *t_r = sched_get_by_pid(pid_r);
        if (!t_s || !t_r || !t_s->process || !t_r->process) {
            screen_push_line("ipc: could not find spawned processes");
            goto Label_Free;
        }
        kern_process_t *p_s = t_s->process;
        kern_process_t *p_r = t_r->process;

        // 3. Create shared memory.
        u64 va_s = 0, va_r = 0;
        kern_shmem_t *sh = shmem_create(p_s, p_r, PAGE_SIZE,
                                        PAGE_RW | PAGE_USER, &va_s, &va_r);
        if (!sh) {
            screen_push_line("ipc: shmem_create failed");
            goto Label_Free;
        }

        // 4. Hand off setup data to each child.
        ipc_setup_send(pid_s, va_s, pid_r);
        ipc_setup_send(pid_r, va_r, pid_s);

        screen_push_linef("ipc: sender pid=%d, receiver pid=%d, shmem ready", pid_s, pid_r);
        screen_push_line("ipc: type to send messages via IPC");
        goto Label_Free;
    }

    if (cmd_word_eq(word, "wm")) {
        PROFILE_SCOPE("cmd:wm");
        // Launch the compositor. It claims the display and takes over all input.
        // Ctrl+C kills it and returns to shell.
        wasm_spawn_opts_t opts = {
            .path = "wm.wasm",
            .foreground = true,
            .wait = false,
        };
        i32 pid = wasm_spawn(&opts);
        if (pid >= 0) {
            screen_push_linef("wm: compositor PID %d launched", pid);
        }
        goto Label_Free;
    }

    if (cmd_word_eq(word, "crashme")) {
        PROFILE_SCOPE("cmd:crashme");
        // Adversarial WASM test: exercises every sandbox boundary.
        // Deep recursion, invalid host fn offsets, tight dispatch loops.
        // Does NOT claim the compositor — purely tests sandboxing.
        wasm_spawn_opts_t opts = {
            .path = "crashme.wasm",
            .foreground = false,
            .wait = true,
        };
        i32 pid = wasm_spawn(&opts);
        if (pid >= 0) {
            screen_push_linef("crashme: PID %d — sandbox held", pid);
        } else {
            screen_push_line("crashme: spawn failed");
        }
        goto Label_Free;
    }

    Label_Fail:
    {
        const char *failure = "Failure to parse command";
        screen_push_line(failure);
        serial_outsf("%s: %s\n", failure, word->loc);
    }

    Label_Free:
    cmd_parse_free(word, kfree);
}
