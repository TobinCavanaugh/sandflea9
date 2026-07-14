/*
 * wat2wasm_wrapper.c — WASI bridge + entry point for wasm2c'd wat2wasm.
 *
 * The generated code (wasm2c_wat2wasm.c) calls WASI functions passing
 * a w2c_wasi__snapshot__preview1* as the first argument. This file:
 *   1. Defines that struct with a back-pointer to the module instance
 *   2. Implements all 15 WASI imports using sandfleaOS kernel APIs
 *   3. Provides wat2wasm_native() — the single entry point from the shell
 */

#include "../include/kern_fs.h"
#include "../include/kern_sched.h"
#include "../include/kern_vmm.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"
#include "../include/kern_screen.h"
#include "../include/kern_terminal.h"
#include "../include/kern_keyboard.h"
#include "../include/util_cmd.h"

/* Our dialect.h already typedefs u8/u32/etc. — prevent wasm2c from redefining. */
#define WASM_RT_CORE_TYPES_DEFINED
#include "wasm2c_wat2wasm.h"

/* Custom trap code for normal proc_exit. Must be non-zero because
   longjmp(buf, 0) is defined to return 1 (not 0) from setjmp.
   Value 100 is well outside the wasm_rt_trap_t enum range. */
#define WASM2C_EXIT_TRAP 100

/* Our WASI context struct — passed as the first arg to every WASI function. */
struct w2c_wasi__snapshot__preview1 {
    w2c_wat2wasm *instance;     /* back-pointer to the module instance */
    int           argc;
    char        **argv;
    int           exit_code;
};

/* Helper: get the WASM linear memory from the WASI context */
static u8* wasi_mem(struct w2c_wasi__snapshot__preview1 *w) {
    if (!w || !w->instance) return NULL;
    return w->instance->w2c_memory.data;
}

static u32 wasi_mem_size(struct w2c_wasi__snapshot__preview1 *w) {
    if (!w || !w->instance) return 0;
    return (u32)w->instance->w2c_memory.size;
}

/* ===================================================================
 * WASI imports — implemented in terms of sandfleaOS kernel APIs
 * =================================================================== */

u32 w2c_wasi__snapshot__preview1_args_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 argv_ptr, u32 argv_buf) {
    u8 *mem = wasi_mem(w);
    u32 mem_sz = wasi_mem_size(w);
    if (!mem || argv_ptr + (u32)w->argc * 4 > mem_sz) return 1;  /* EINVAL */

    u32 str_off = argv_buf;
    for (int i = 0; i < w->argc && i < 16; i++) {
        if (!w->argv[i]) break;
        u32 len = str_len(w->argv[i]) + 1;
        if (argv_ptr + i * 4 + 4 > mem_sz || str_off + len > mem_sz) return 1;
        *(u32*)(mem + argv_ptr + i * 4) = str_off;
        mem_copy(mem + str_off, (const u8*)w->argv[i], len);
        str_off += len;
    }
    return 0;  /* ESUCCESS */
}

u32 w2c_wasi__snapshot__preview1_args_sizes_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 argc_ptr, u32 buf_size_ptr) {
    u8 *mem = wasi_mem(w);
    u32 mem_sz = wasi_mem_size(w);
    if (!mem || argc_ptr + 4 > mem_sz || buf_size_ptr + 4 > mem_sz) return 1;
    *(i32*)(mem + argc_ptr) = w->argc;
    u32 total = 0;
    for (int i = 0; i < w->argc && i < 16; i++)
        if (w->argv[i]) total += str_len(w->argv[i]) + 1;
    *(u32*)(mem + buf_size_ptr) = total;
    return 0;
}

u32 w2c_wasi__snapshot__preview1_environ_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 environ_ptr, u32 environ_buf) {
    return 0;
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 environc_ptr, u32 buf_size_ptr) {
    u8 *mem = wasi_mem(w);
    u32 mem_sz = wasi_mem_size(w);
    if (!mem || environc_ptr + 4 > mem_sz || buf_size_ptr + 4 > mem_sz) return 1;
    *(u32*)(mem + environc_ptr) = 0;
    *(u32*)(mem + buf_size_ptr) = 0;
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_close(
    struct w2c_wasi__snapshot__preview1 *w, u32 fd) {
    if (fd < 3) return 0;  /* stdin/stdout/stderr: no-op */
    return fs_close((i32)fd) == 0 ? 0 : 8;  /* EBADF */
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 stat_ptr) {
    u8 *mem = wasi_mem(w);
    if (!mem || stat_ptr + 24 > wasi_mem_size(w)) return 1;

    mem_set(mem + stat_ptr, 0, 24);
    if (fd == 3)      mem[stat_ptr] = 3;  /* directory */
    else if (fd < 3)  mem[stat_ptr] = 5;  /* character_device */
    else              mem[stat_ptr] = 4;  /* regular_file */
    *(u64*)(mem + stat_ptr + 8) = 0x1f;  /* rights: read/write/seek/path_open */
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(
    struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 flags) {
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_get(
    struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 prestat_ptr) {
    u8 *mem = wasi_mem(w);
    if (!mem || prestat_ptr + 8 > wasi_mem_size(w)) return 1;
    if (fd == 3) {
        mem[prestat_ptr] = 0;
        *(u32*)(mem + prestat_ptr + 4) = 1;
        return 0;
    }
    return 8;
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_dir_name(
    struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 path_ptr, u32 path_len) {
    u8 *mem = wasi_mem(w);
    if (!mem || path_ptr + path_len > wasi_mem_size(w)) return 1;
    if (fd == 3 && path_len >= 1) {
        mem[path_ptr] = '/';
        return 0;
    }
    return 8;
}

u32 w2c_wasi__snapshot__preview1_fd_read(
    struct w2c_wasi__snapshot__preview1 *w,
    u32 fd, u32 iovs_ptr, u32 iovs_len, u32 nread_ptr) {
    u8 *mem = wasi_mem(w);
    u32 mem_sz = wasi_mem_size(w);
    if (!mem || nread_ptr + 4 > mem_sz) return 1;

    u32 total = 0;
    for (u32 i = 0; i < iovs_len; i++) {
        if (iovs_ptr + i * 8 + 8 > mem_sz) break;
        u32 buf = *(u32*)(mem + iovs_ptr + i * 8);
        u32 len = *(u32*)(mem + iovs_ptr + i * 8 + 4);
        if (buf + len > mem_sz) continue;
        if (len == 0) continue;

        i32 br = fs_read((i32)fd, mem + buf, len);
        if (br <= 0) break;
        total += (u32)br;
    }
    *(u32*)(mem + nread_ptr) = total;
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_seek(
    struct w2c_wasi__snapshot__preview1 *w,
    u32 fd, u64 offset, u32 whence, u32 newoffset_ptr) {
    u8 *mem = wasi_mem(w);
    if (!mem || newoffset_ptr + 8 > wasi_mem_size(w)) return 1;
    if (fd < 3) return 52;

    i32 res = fs_seek((i32)fd, (i32)offset, (i32)whence);
    if (res < 0) return 8;
    *(u64*)(mem + newoffset_ptr) = (u64)fs_tell((i32)fd);
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_write(
    struct w2c_wasi__snapshot__preview1 *w,
    u32 fd, u32 iovs_ptr, u32 iovs_len, u32 nwritten_ptr) {
    u8 *mem = wasi_mem(w);
    u32 mem_sz = wasi_mem_size(w);
    if (!mem || nwritten_ptr + 4 > mem_sz) return 1;

    u32 total = 0;
    for (u32 i = 0; i < iovs_len; i++) {
        if (iovs_ptr + i * 8 + 8 > mem_sz) break;
        u32 buf = *(u32*)(mem + iovs_ptr + i * 8);
        u32 len = *(u32*)(mem + iovs_ptr + i * 8 + 4);
        if (buf + len > mem_sz) continue;
        if (len == 0) continue;

        if (fd == 1 || fd == 2) {
            term_write((const char*)(mem + buf), len);
            total += len;
        } else {
            i32 res = fs_write((i32)fd, mem + buf, len);
            if (res < 0) break;
            total += (u32)res;
        }
    }
    *(u32*)(mem + nwritten_ptr) = total;
    return 0;
}

u32 w2c_wasi__snapshot__preview1_path_open(
    struct w2c_wasi__snapshot__preview1 *w,
    u32 dir_fd, u32 dirflags, u32 path_ptr, u32 path_len,
    u32 oflags, u64 rights_base, u64 rights_inheriting,
    u32 fdflags, u32 ret_fd_ptr) {
    u8 *mem = wasi_mem(w);
    u32 mem_sz = wasi_mem_size(w);
    if (!mem || ret_fd_ptr + 4 > mem_sz) return 1;
    if (path_ptr + path_len > mem_sz) return 1;

    char path_buf[256];
    u32 copy = path_len < 250 ? path_len : 250;
    mem_copy((u8*)path_buf, mem + path_ptr, copy);
    path_buf[copy] = 0;

    serial_outsf("wasm2c: path_open(%s) oflags=0x%x\n", path_buf, oflags);

    i32 new_fd = fs_open(path_buf);
    if (new_fd < 0 && (oflags & 1)) {
        extern i32 fs_create(const char*);
        new_fd = fs_create(path_buf);
        serial_outsf("wasm2c: create(%s) -> %d\n", path_buf, new_fd);
    }
    if (new_fd < 0) return 44;
    *(i32*)(mem + ret_fd_ptr) = new_fd;
    return 0;
}

u32 w2c_wasi__snapshot__preview1_path_filestat_get(
    struct w2c_wasi__snapshot__preview1 *w,
    u32 dir_fd, u32 dirflags, u32 path_ptr, u32 path_len, u32 stat_ptr) {
    u8 *mem = wasi_mem(w);
    if (!mem || stat_ptr + 64 > wasi_mem_size(w)) return 1;
    if (path_ptr + path_len > wasi_mem_size(w)) return 1;

    /* Build path and check existence via fs_open */
    char path_buf[256];
    u32 copy = path_len < 250 ? path_len : 250;
    mem_copy((u8*)path_buf, mem + path_ptr, copy);
    path_buf[copy] = 0;

    i32 fd = fs_open(path_buf);
    if (fd < 0) return 44;  /* ENOENT */

    /* Fill in a minimal stat: regular file, size from fs_size */
    u32 fsize = fs_size(fd);
    fs_close(fd);
    mem_set(mem + stat_ptr, 0, 64);
    mem[stat_ptr] = 4;               /* fs_filetype = regular_file */
    *(u64*)(mem + stat_ptr + 24) = (u64)fsize;  /* fs_size */
    return 0;
}

void w2c_wasi__snapshot__preview1_proc_exit(
    struct w2c_wasi__snapshot__preview1 *w, u32 code) {
    w->exit_code = (int)code;
    serial_outsf("wasm2c: proc_exit(%d)\n", code);
    /* Use sentinel value (non-zero) so longjmp returns it verbatim */
    wasm_rt_trap((wasm_rt_trap_t)WASM2C_EXIT_TRAP);
}

/* ===================================================================
 * Public API — called from the shell command handler
 * =================================================================== */

int wat2wasm_native(int argc, char **argv) {
    if (argc < 2) return -1;

    w2c_wat2wasm instance;
    wasm2c_wat2wasm_instantiate(&instance, NULL);

    struct w2c_wasi__snapshot__preview1 wasi_ctx;
    wasi_ctx.instance   = &instance;
    wasi_ctx.argc       = argc;
    wasi_ctx.argv       = argv;
    wasi_ctx.exit_code  = 0;

    instance.w2c_wasi__snapshot__preview1_instance = &wasi_ctx;

    serial_outsf("wasm2c: Running wat2wasm with %d args (mem=%d bytes)\n",
                 argc, (u32)instance.w2c_memory.size);

    /* Run _start with trap protection */
    wasm_rt_trap_t trap = (wasm_rt_trap_t)wasm_rt_impl_try();
    if (trap == WASM_RT_TRAP_NONE) {
        w2c_wat2wasm_0x5Fstart(&instance);
    } else if (trap != (wasm_rt_trap_t)WASM2C_EXIT_TRAP) {
        /* Real trap — not a normal proc_exit */
        screen_push_linef("wasm2c: Trap: %s", wasm_rt_strerror(trap));
    }
    /* On proc_exit: trap == 100, wasi_ctx.exit_code is set. Fall through. */

    wasm2c_wat2wasm_free(&instance);
    serial_outsf("wasm2c: Done (exit code %d)\n", wasi_ctx.exit_code);
    return wasi_ctx.exit_code;
}
