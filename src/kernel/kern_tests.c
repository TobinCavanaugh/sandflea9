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

#define DoM3Logging 0


#include "wasm3-0.5.0/source/m3_env.h"
#include "wasm3-0.5.0/source/m3_api_libc.h"


// TODO Running kmalloc2 and then kmalloc causes early page fault? I think

cmd_word_t *word;

// Doom globals
bool doom_active = false;
static u32 doom_frame_width = 0;
static u32 doom_frame_height = 0;
static volatile u64 doom_last_draw_time = 0;  // watchdog: tracks last drawFrame call

// --- Doom profiling ---
// NOTE: sw timer has 10ms granularity — short ops (<10ms) will report as 0
#define PROFILE_EVERY_N 30  // dump stats every N frames
static u64   prof_kbd_time    = 0;  // keyboard polling
static u64   prof_tick_time   = 0;  // tickGame execution
static u64   prof_blit_time   = 0;  // drawFrame (direct-to-fb blit)
static u64   prof_loop_total  = 0;  // active CPU per iteration

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

m3ApiRawFunction(wasm_fd_open) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (u32, path_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);

    if (mem && path_offset < memory_size) {
        const char *path = (const char *) (mem + path_offset);
        serial_outsf("WASM: Open called for path: %s\n", path);

        i32 fd = fs_open(path);
        m3ApiReturn(fd);
    } else {
        screen_push_line("WASM: Invalid memory access in sys_open");
        m3ApiReturn(-1);
    }
}

m3ApiRawFunction(wasm_fd_close) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)

    screen_push_linef("wasm close: %d", fd);
    serial_outsf("wasm close: %d\n", fd);
    i32 result = fs_close(fd);
    m3ApiReturn(result);
}

m3ApiRawFunction(wasm_fd_read) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (i32, fd)
    m3ApiGetArg     (u32, buf_offset)
    m3ApiGetArg     (u32, count)

    serial_outsf("WASM: Read called for fd: %d\n", fd);
    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);

    if (mem && buf_offset + count <= memory_size) {
        u8 *kernel_buf = mem + buf_offset;
        i32 bytes_read = fs_read(fd, kernel_buf, count);
        m3ApiReturn(bytes_read);
    } else {
        m3ApiReturn(-1);
    }
}

typedef struct __wasi_ciovec_t {
    uint32_t buf;      // Offset in Wasm memory
    uint32_t buf_len;  // Length of this buffer
} __wasi_ciovec_t;

m3ApiRawFunction(wasm_fd_write) {
    m3ApiGetArg     (i32, fd)
    m3ApiGetArg     (u32, iovs_offset)
    m3ApiGetArg     (i32, iovs_len)
    m3ApiGetArg     (u32, nwritten_offset)
    m3ApiReturnType (i32);

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);

    if (!mem) m3ApiReturn(-1);

    __wasi_ciovec_t *iovs = (__wasi_ciovec_t *) (mem + iovs_offset);
    u32 *nwritten = (u32 *) (mem + nwritten_offset);

    // Basic bounds check for iovs and nwritten
    if (iovs_offset + iovs_len * sizeof(__wasi_ciovec_t) > memory_size ||
        nwritten_offset + sizeof(u32) > memory_size) {
        m3ApiReturn(-1);
    }

    u32 total_written = 0;
    kern_process_t *proc = sched_get_current_process();

    serial_outsf("WASM: Write called for fd: %d\n", fd);
    for (i32 i = 0; i < iovs_len; i++) {
        u32 buf_offset = iovs[i].buf;
        u32 len = iovs[i].buf_len;

        if (buf_offset + len <= memory_size) {
            u8 *host_buf = mem + buf_offset;
            if (len > 0) {
                if (proc && fd >= 0 && fd < MAX_FILE_HANDLES && proc->fd_table[fd] != null) {
                    i32 res = fs_write(fd, host_buf, len);
                    if (res < 0) {
                        break;
                    }
                    total_written += res;
                } else if (fd == 1 || fd == 2) {
                    screen_push_buf(host_buf, len);
                    total_written += len;
                } else {
                    break;
                }
            }
        }
    }

    if (nwritten) *nwritten = total_written;
    m3ApiReturn(0);
}

extern volatile u64 sw;

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

    doom_last_draw_time = sw;  // update watchdog on every frame

    // Blit Doom framebuffer directly to the real hardware framebuffer
    // (skips the backbuffer and full-screen copy for max performance)
    if (doom_frame_width > 0 && doom_frame_height > 0) {
        u64 t_blit_start = sw;
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

                // Clamp to screen size
                if (copy_w > disp->surface.width) copy_w = disp->surface.width;
                if (copy_h > disp->surface.height) copy_h = disp->surface.height;

                // Center the Doom frame on screen
                u32 dst_x_off = (disp->surface.width - copy_w) / 2;
                u32 dst_y_off = (disp->surface.height - copy_h) / 2;

                // Blit row by row (pitches can differ)
                for (u32 y = 0; y < copy_h; y++) {
                    mem_copy(
                        (u8 *)(dst + (dst_y_off + y) * dst_pitch_px + dst_x_off),
                        (u8 *)(src + y * src_pitch_px),
                        copy_w * 4
                    );
                }

                prof_blit_time += (sw - t_blit_start);
                // No screen_draw() needed — we wrote directly to trueAddress
            }
        }
    }

    m3ApiSuccess();
}

u0 wasm_test(u0 *arg) {
    const char *wasm_path = (const char *) arg;
    if (!wasm_path) wasm_path = "add_test.wasm";

    IM3Environment env = null;
    IM3Runtime runtime = null;
    u8 *wasm_data = null;

    serial_outsf("WASM: Loading %s\n", wasm_path);
    i32 fd = fs_open(wasm_path);
    if (fd < 0) {
        screen_push_linef("WASM: Could not open %s", wasm_path);
        goto Label_Done;
    }

    u32 size = fs_size(fd);
    wasm_data = kmalloc(size);
    if (!wasm_data) {
        screen_push_line("WASM: Out of memory for WASM data");
        fs_close(fd);
        goto Label_Done;
    }
    fs_read(fd, wasm_data, size);
    fs_close(fd);

    serial_outsl("WASM: Initializing environment...");
    env = m3_NewEnvironment();
    if (!env) {
        screen_push_line("WASM: Could not create environment");
        goto Label_Done;
    }

    serial_outsl("WASM: Initializing runtime...");
    runtime = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!runtime) {
        screen_push_line("WASM: Could not create runtime");
        goto Label_Done;
    }


    IM3Module module = NULL;
    serial_outsl("WASM: Parsing module...");
    M3Result result = m3_ParseModule(env, &module, wasm_data, size);
    if (result) {
        screen_push_linef("WASM: Parse error: %s", result);
        goto Label_Done;
    }

    if (!module) {
        screen_push_line("WASM: Parsing failed, module is NULL");
        goto Label_Done;
    }

    serial_outsf("m3_LoadModule: %p\n", m3_LoadModule);
    result = m3_LoadModule(runtime, module);
    if (result) {
        screen_push_linef("WASM: Load error: %s", result);
        goto Label_Done;
    }

    m3_LinkRawFunction(module, "env", "fd_open", "i(i)", &wasm_fd_open);
    m3_LinkRawFunction(module, "env", "fd_read", "i(iii)", &wasm_fd_read);
    m3_LinkRawFunction(module, "env", "fd_close", "i(i)", &wasm_fd_close);
    m3_LinkRawFunction(module, "env", "fd_write", "i(iiii)", &wasm_fd_write);

    serial_outsl("WASM: Linking LibC...");
    result = m3_LinkLibC(module);
    if (result) {
        screen_push_linef("WASM: Link error: %s", result);
        goto Label_Done;
    }

    IM3Function f;
    result = m3_FindFunction(&f, runtime, "_start");
    if (result) {
        screen_push_linef("WASM: Function error: %s", result);
        goto Label_Done;
    }

    serial_outsl("WASM: Calling _start()...");
    result = m3_CallArgv(f, 0, NULL);
    if (result) {
        screen_push_linef("WASM: Call error: %s", result);
        serial_outsf("WASM: Call error: %s", result);
    } else {
        i32 res = 0;
        m3_GetResultsV(f, &res);
        screen_push_linef("WASM: _start() returned %d", res);
        serial_outsf("WASM: _start() returned %d\n", res);
    }

    Label_Done:
    if (runtime) m3_FreeRuntime(runtime);
    if (env) m3_FreeEnvironment(env);
    if (wasm_data) kfree(wasm_data);
    if (arg) kfree(arg);
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

    doom_active = true;

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

    // Also link standard WASI-like file descriptors
    m3_LinkRawFunction(module, "env", "fd_open", "i(i)", &wasm_fd_open);
    m3_LinkRawFunction(module, "env", "fd_read", "i(iii)", &wasm_fd_read);
    m3_LinkRawFunction(module, "env", "fd_close", "i(i)", &wasm_fd_close);
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

    while (true) {
        u64 t_loop_start = sw;

        // --- Keyboard input: check all mapped scancodes ---
        u64 t_kbd_start = sw;
        for (int i = 0; i < key_map_count; i++) {
            doom_key_map_t *km = &key_map[i];
            bool pressed = keyboard_scancode_is_pressed(km->scancode);

            if (pressed && !km->was_down) {
                // Key just pressed
                if (report_keydown_func && *km->key_val_ptr >= 0) {
                    const void *args[1] = { km->key_val_ptr };
                    m3_Call(report_keydown_func, 1, args);
                }
            } else if (!pressed && km->was_down) {
                // Key just released
                if (report_keyup_func && *km->key_val_ptr >= 0) {
                    const void *args[1] = { km->key_val_ptr };
                    m3_Call(report_keyup_func, 1, args);
                }
            }
            km->was_down = pressed;
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

        // Watchdog: if Doom hasn't drawn a frame in ~1 second, it likely quit internally
        // (the WASM module's _exit() is a stub since it's not imported)
        if (doom_last_draw_time > 0 && (sw - doom_last_draw_time) > 50) {
            serial_outsl("DOOM: drawFrame watchdog triggered — game appears to have quit");
            break;
        }

        // Dump profiling stats every N frames (times in timer ticks, ~10ms each)
        {
            static u32 prof_frame_count = 0;
            prof_frame_count++;
            if (prof_frame_count % PROFILE_EVERY_N == 0) {
                serial_outsf(
                    "PROFILE[%d frames, ~%dms per tick]: kbd=%-4lld tick=%-5lld blit=%-4lld active=%-5lld\n",
                    PROFILE_EVERY_N, 10,
                    prof_kbd_time, prof_tick_time, prof_blit_time, prof_loop_total
                );
                prof_kbd_time = 0;
                prof_tick_time = 0;
                prof_blit_time = 0;
                prof_loop_total = 0;
            }
        }

        // No delay — tickGame handles internal frame timing;
        // tight loop for maximum responsiveness
    }

    screen_push_line("WASM DOOM: Game loop exited.");
    serial_outsl("WASM DOOM: Game loop exited.");

Label_Done:
    #undef BOOT_LOG
    keyboard_flush_queue();  // clear leftover extended key codes from Doom gameplay
    doom_active = false;
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

    if (cmd_word_eq(word, "doom")) {
        char *path = null;
        if (word->next != null) {
            path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        } else {
            path = str_dup("doom-v0.1.0.wasm", kmalloc);
        }

        kern_process_t *proc = process_create();
        if (proc) {
            if (!sched_create_process_thread(proc, wasm_doom_test, path)) {
                screen_push_line("DOOM: Failed to create thread");
                if (path) kfree(path);
            }
        } else {
            screen_push_line("DOOM: Failed to create process (OOM)");
            if (path) kfree(path);
        }
        goto Label_Free;
    }

    if (cmd_word_eq(word, "wasm")) {
        char *path = null;
        if (word->next != null) {
            path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        }

        kern_process_t *proc = process_create();
        if (proc) {
            if (!sched_create_process_thread(proc, wasm_test, path)) {
                screen_push_line("WASM: Failed to create thread");
                if (path) kfree(path);
                // process_exit(proc); // Should probably clean up the process too
            }
        } else {
            screen_push_line("WASM: Failed to create process (OOM)");
            if (path) kfree(path);
        }
        goto Label_Free;
    }

    if (cmd_word_eq(word, "doxw")) {
        i64 count = 10;
        if (word->next && word->next->val_type == CMD_WT_i64) {
            count = word->next->val_i64;
        }

        while (count--) {
            kern_process_t *new_proc = process_create();
            if (!new_proc) {
                screen_push_line("doxw: Failed to create process (OOM)");
                break;
            }
            char *p = str_dup("file_test.wasm", kmalloc);
            if (!p || !sched_create_process_thread(new_proc, wasm_test, p)) {
                screen_push_line("doxw: Failed to create thread (OOM)");
                if (p) kfree(p);
                break;
            }
        }

        goto Label_Free;
    }

    if (cmd_word_eq(word, "dox")) {
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
        i64 val = 5;
        if (word->next && word->next->val_type == CMD_WT_i64) {
            val = word->next->val_i64;
        }

        kern_process_t *new_proc = process_create();
        sched_create_process_thread(new_proc, dotest, (u0 *) val);
        goto Label_Free;
    }


    if (cmd_word_eq(word, "kmalloc")) {
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
        typingbuf[0] = 0;
        screen_terminal_clear();
        goto Label_Free;
    }

    if (cmd_word_eq(word, "lsr")) {
        char *path = null;
        if (word->next != null) {
            path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        }
        sched_create_thread((u0 (*)(u0 *)) test_lsr, path);
//        test_lsr(); // works fine
        goto Label_Free;
    }


    if (cmd_word_eq(word, "open") && word->next != null) {
        char *path = str_dup(word->next->loc, kmalloc);
        path[word->next->len] = 0;

        i32 w = fs_open(path);

        if (w < 0) {
            screen_push_linef("Failed to open file at `%s`", path);
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
        char *path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        kern_process_t *proc = process_create();
        sched_create_process_thread(proc, test_ext2, path);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "cat") && word->next != null) {
        char *path = str_dup(word->next->loc, kmalloc);
        path[word->next->len] = 0;

        i32 fd = fs_open(path);
        if (fd >= 0) {
            u32 size = fs_size(fd);
            screen_push_linef("Reading %s (%d bytes) via FD %d", path, size, fd);

            u8 *buf = kmallocz(size + 1);
            i32 read = fs_read(fd, buf, size);
            if (read >= 0) {
                screen_push_line((char *) buf);
                serial_outsf("CAT: %s\n", (char *) buf);
            } else {
                screen_push_line("Error reading file");
            }
            kfree(buf);
            fs_close(fd);
        } else {
            screen_push_linef("Could not open: %s", path);
        }

        kfree(path);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "write") && word->next != null && word->next->next != null) {
        char *path = str_dup_len(word->next->loc, word->next->len, kmalloc);
        char *str = str_dup_len(word->next->next->loc, word->next->next->len, kmalloc);

        i32 fd = fs_open(path);
        if (fd >= 0) {
            i32 written = fs_write(fd, (u8 *) str, word->next->next->len);
            if (written >= 0) {
                screen_push_linef("Wrote %d bytes to %s", written, path);
            } else {
                screen_push_line("Error writing to file");
            }
            fs_close(fd);
        } else {
            screen_push_linef("Could not open: %s", path);
        }

        kfree(path);
        kfree(str);
        goto Label_Free;
    }

    if (cmd_word_eq(word, "kmalloc2")) {
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


    if (cmd_word_eq(word, "pci")) {
        pci_device_t *dev = system.pci_list_head;
        while (dev) {
            screen_push_linef("C:%X S:%X | V:%X D:%X\n",
                              dev->class_code, dev->subclass,
                              dev->vendor_id, dev->device_id);
            dev = dev->next;
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
