// src/kernel/wasm_spawn.c
//
// Single chokepoint for loading + running a WASM program as a new sandfleaOS process.
// Replaces the duplicated wasm_test() / inline boilerplate that used to live in
// handle_command(). Doom/test_ext2 use the same shim with a link_extra hook.

#include "../include/wasm_spawn.h"

#include "../include/kern_serial.h"
#include "../include/kern_terminal.h"
#include "../include/kern_screen.h"
#include "../include/kern_sched.h"
#include "../include/kern_vmm.h"   // kmalloc / kfree / pmm_* / vmm_*
#include "../include/kern_fs.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_mem.h"
#include "../include/util_cmd.h"
#include "../util/util_str.h"
#include "../include/stbsupport.h"
#include "../include/kern_ext2.h"

#include "wasm3-0.5.0/source/m3_env.h"
#include "wasm3-0.5.0/source/m3_api_libc.h"

// ============================================================================
// Helpers
// ============================================================================

// Local strdup that hands the allocation off to whichever kernel allocator
// the caller prefers (kmalloc today; tests can swap in a counting alloc).
static char * str_dup_safe(const char *s, void *(*alloc)(u64)) {
    if (!s) return null;
    u32 len = str_len(s);
    char *dup = (char *) alloc(len + 1);
    if (!dup) return null;
    mem_copy((u8 *) dup, (const u8 *) s, len);
    dup[len] = 0;
    return dup;
}

// Per-thread args allocated by wasm_spawn() and freed by wasm_proc_cleanup().
// Lives entirely on the kernel heap. All fields except the function pointers
// are populated as the thread runs; if the thread is force-killed mid-flight
// (Ctrl+C), wasm_proc_cleanup sees the partially populated state and frees
// whatever is set.
typedef struct wasm_run_args {
    char          *wasm_path_alloc;   // deep-copied from opts->path
    u8            *wasm_data;         // raw .wasm bytes (kmalloc'd)
    IM3Environment env;               // m3_NewEnvironment result
    IM3Runtime     runtime;           // m3_NewRuntime result
    u32            stack_kb;
    bool           wasi_argv;         // honor opts->wasi_argv: pass argv to _start?
    void         (*link_extra)(IM3Module, IM3Runtime, void *);
    void          *link_user;
} wasm_run_args_t;

// Per-process cleanup hook: invoked from kern_sched.c::process_exit() before
// the proc struct is freed. Frees the WASM environment/runtime/bytes/path,
// then the args struct itself. Safe to call from any thread (the killed
// thread is DEAD and no longer in the task list when this runs).
//
// Idempotency note: wasm_thread_entry's Label_Done also frees env/runtime/
// wasm_data/wasm_path_alloc and then nulls each pointer. By the time this
// runs after a normal exit, all those pointers are null, so the if-guards
// skip them and only kfree(ra) does any work.
static u0 wasm_proc_cleanup(u0 *ctx) {
    wasm_run_args_t *ra = (wasm_run_args_t *) ctx;
    if (!ra) return;
    if (ra->runtime)         m3_FreeRuntime(ra->runtime);
    if (ra->env)             m3_FreeEnvironment(ra->env);
    if (ra->wasm_data)       kfree(ra->wasm_data);
    if (ra->wasm_path_alloc) kfree(ra->wasm_path_alloc);
    kfree(ra);
}

// ============================================================================
// Host imports — previously declared in kern_tests.c.
// These are the host functions available to *.wasm programs via (import "env" ...).
// ============================================================================

typedef struct __wasi_ciovec_t {
    uint32_t buf;
    uint32_t buf_len;
} __wasi_ciovec_t;

m3ApiRawFunction(wasm_fd_open) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (u32, path_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (mem && path_offset < memory_size) {
        const char *path = (const char *) (mem + path_offset);
        i32 fd = fs_open(path);
        serial_outsf("WASM: Open called for path: %s -> fd=%d\n", path, fd);
        m3ApiReturn(fd);
    } else {
        screen_push_line("WASM: Invalid memory access in sys_open");
        m3ApiReturn(-1);
    }
}

m3ApiRawFunction(wasm_fd_close) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (i32, fd)

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

    serial_outsf("WASM: Read called for fd: %d, count: %d\n", fd, count);
    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (mem && buf_offset + count <= memory_size) {
        u8 *kernel_buf = mem + buf_offset;
        if (fd == 0) {
            u32 read_bytes = 0;
            while (read_bytes < count) {
                u8 k = keyboard_fg_eat();
                if (k != 0) {
                    if (k == '\r') k = '\n';
                    if (k == '\b') {
                        if (read_bytes > 0) read_bytes--;
                        continue;
                    }
                    kernel_buf[read_bytes++] = k;
                    screen_push_buf((const char *) &k, 1);
                    if (k == '\n') break;
                } else {
                    sched_yield();
                }
            }
            m3ApiReturn((i32) read_bytes);
        } else {
            i32 bytes_read = fs_read(fd, kernel_buf, count);
            m3ApiReturn(bytes_read);
        }
    } else {
        m3ApiReturn(-1);
    }
}

m3ApiRawFunction(wasm_fd_write) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (i32, fd)
    m3ApiGetArg     (u32, iovs_offset)
    m3ApiGetArg     (i32, iovs_len)
    m3ApiGetArg     (u32, nwritten_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (!mem) m3ApiReturn(-1);

    __wasi_ciovec_t *iovs = (__wasi_ciovec_t *) (mem + iovs_offset);
    u32 *nwritten = (u32 *) (mem + nwritten_offset);

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
                    if (res < 0) break;
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

m3ApiRawFunction(wasm_lsr) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (u32, path_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (mem && path_offset < memory_size) {
        const char *path = (const char *) (mem + path_offset);
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
            m3ApiReturn(0);
        } else {
            screen_push_linef("lsr: Path not found: %s", path);
            m3ApiReturn(-1);
        }
    } else {
        m3ApiReturn(-1);
    }
}

// Legacy argv interface used by add_test.wasm / file_test.wasm today.
// Stage 2 may retire this in favor of _start(argc, argv_ptr) for newly compiled WASM.
m3ApiRawFunction(wasm_get_arg_count) {
    m3ApiReturnType (i32)
    kern_process_t *proc = sched_get_current_process();
    if (proc) {
        m3ApiReturn(proc->argc);
    } else {
        m3ApiReturn(0);
    }
}

m3ApiRawFunction(wasm_get_arg) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (i32, index)
    m3ApiGetArg     (u32, buf_offset)
    m3ApiGetArg     (u32, max_len)

    kern_process_t *proc = sched_get_current_process();
    if (!proc || index < 0 || index >= proc->argc) {
        m3ApiReturn(-1);
    }
    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (mem && buf_offset < memory_size) {
        const char *arg = proc->argv[index];
        u32 len = str_len(arg);
        if (len >= max_len) len = max_len - 1;
        mem_copy(mem + buf_offset, (const u8 *) arg, len);
        mem[buf_offset + len] = 0;
        m3ApiReturn((i32) len);
    } else {
        m3ApiReturn(-1);
    }
}

// ============================================================================
// argv injection: write (argc, argv) into wasm linear memory in wasi-style layout
// (strings packed at offset 16, then u32 ptr[] table at next aligned offset).
// Returns the offset of the ptr[] table on success, or 0 on overflow.
// MVP: assumes the wasm module's initial memory is at least ~1 page after grow.
// TODO: explicitly call memory.grow when offset+argv_bytes > mem_size.
// ============================================================================
static u32 inject_argv(IM3Runtime rt, int argc, char **argv, kern_process_t *proc) {
    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(rt, &mem_size, 0);
    if (!mem || argc <= 0) return 0;

    u32 offset = 16;            // skip the wasm zero page
    u32 ptrs[16] = {0};
    for (int i = 0; i < argc && i < 16; i++) {
        if (!argv[i]) break;
        u32 len = str_len(argv[i]) + 1;
        if (offset + len > mem_size) {
            screen_push_linef("WASM: argv overflow at entry %d (need %u, have %u)",
                              i, offset + len, mem_size);
            return 0;
        }
        mem_copy(mem + offset, (const u8 *) argv[i], len);
        ptrs[i] = offset;
        offset += len;
    }

    u32 aligned_offset = (offset + 3u) & ~3u;
    if (aligned_offset + ((u32) argc) * 4u > mem_size) {
        screen_push_linef("WASM: argv ptr table overflow (need %u, have %u)",
                          aligned_offset + argc * 4u, mem_size);
        return 0;
    }

    for (int i = 0; i < argc; i++) {
        *(u32 *) (mem + aligned_offset + i * 4u) = ptrs[i];
    }
    return aligned_offset;
}

// ============================================================================
// Thread entry: load + parse + link + inject argv + call _start + cleanup.
// Single Label_Done means runtime/env/bytes all freed in one place.
// ============================================================================
u0 wasm_thread_entry(u0 *arg) {
    wasm_run_args_t *ra = (wasm_run_args_t *) arg;
    char *wasm_path = ra->wasm_path_alloc;

    // All WASM-owned pointers live in the args struct so the cleanup hook
    // (wasm_proc_cleanup) can find and free them after a force-kill.
    ra->env       = null;
    ra->runtime   = null;
    ra->wasm_data = null;
    IM3Environment env       = null;
    IM3Runtime     runtime   = null;
    u8            *wasm_data = null;
    kern_process_t *proc = sched_get_current_process();

    serial_outsf("WASM: Loading %s\n", wasm_path);

    // 1. Read file from disk
    i32 fd = fs_open(wasm_path);
    if (fd < 0) {
        screen_push_linef("WASM: Could not open %s", wasm_path);
        goto Label_Done;
    }
    u32 wasm_size = fs_size(fd);
    wasm_data = kmalloc(wasm_size);
    if (!wasm_data) {
        screen_push_line("WASM: Out of memory for WASM data");
        fs_close(fd);
        goto Label_Done;
    }
    ra->wasm_data = wasm_data;
    fs_read(fd, wasm_data, wasm_size);
    fs_close(fd);

    // 2. Allocate environment + runtime
    env = m3_NewEnvironment();
    if (!env) {
        screen_push_line("WASM: Could not create environment");
        goto Label_Done;
    }
    ra->env = env;
    u32 stack_bytes = ra->stack_kb * 1024u;
    runtime = m3_NewRuntime(env, stack_bytes, null);
    if (!runtime) {
        screen_push_line("WASM: Could not create runtime");
        goto Label_Done;
    }
    ra->runtime = runtime;

    // 3. Parse + load
    IM3Module module = null;
    M3Result result = m3_ParseModule(env, &module, wasm_data, wasm_size);
    if (result || !module) {
        screen_push_linef("WASM: Parse error: %s", result ? result : "null module");
        goto Label_Done;
    }
    result = m3_LoadModule(runtime, module);
    if (result) {
        screen_push_linef("WASM: Load error: %s", result);
        goto Label_Done;
    }

    // 4. Link common imports (every program gets fd_*, get_arg_*, and env.lsr).
    m3_LinkRawFunction(module, "env", "fd_open",         "i(i)",    &wasm_fd_open);
    m3_LinkRawFunction(module, "env", "fd_read",         "i(iii)",  &wasm_fd_read);
    m3_LinkRawFunction(module, "env", "fd_close",        "i(i)",    &wasm_fd_close);
    m3_LinkRawFunction(module, "env", "fd_write",        "i(iiii)", &wasm_fd_write);
    m3_LinkRawFunction(module, "env", "lsr",             "i(i)",    &wasm_lsr);
    m3_LinkRawFunction(module, "env", "get_arg_count",   "i()",     &wasm_get_arg_count);
    m3_LinkRawFunction(module, "env", "get_arg",         "i(iii)",  &wasm_get_arg);

    // 5. Program-specific imports (doom-style). Best-effort: missing imports
    //    will surface as m3 errors during _start anyway.
    if (ra->link_extra) ra->link_extra(module, runtime, ra->link_user);

    // 6. LibC link (non-fatal — modules that don't import _exit etc are fine).
    result = m3_LinkLibC(module);
    if (result) {
        screen_push_linef("WASM: LinkLibC: %s", result);
    }

    // 7. Find _start and call _start(argc, argv_ptr) wasi-style.
    IM3Function func = null;
    result = m3_FindFunction(&func, runtime, "_start");
    if (result) {
        screen_push_linef("WASM: _start not found: %s", result);
        goto Label_Done;
    }

    if (proc && ra->wasi_argv && proc->argc > 0 && proc->argv[0]) {
        u32 argv_ptr = inject_argv(runtime, proc->argc, proc->argv, proc);
        if (argv_ptr != 0) {
            char argc_str[12], argv_str[12];
            stbsp_snprintf(argc_str,  sizeof(argc_str),  "%d", proc->argc);
            stbsp_snprintf(argv_str,  sizeof(argv_str),  "%u", argv_ptr);
            const char *args[2] = { argc_str, argv_str };
            screen_push_linef("WASM: Calling _start(argc=%d, argv=%u)", proc->argc, argv_ptr);
            result = m3_CallArgv(func, 2, args);
        } else {
            // InjectArgv reported a problem; fall back to no-args.
            screen_push_line("WASM: Calling _start() without argv");
            result = m3_CallArgv(func, 0, null);
        }
    } else {
        screen_push_line("WASM: Calling _start()...");
        result = m3_CallArgv(func, 0, null);
    }

    if (result) {
        screen_push_linef("WASM: Call error: %s", result);
        serial_outsf("WASM: Call error: %s\n", result);
    } else {
        screen_push_line("WASM: _start returned");
    }

Label_Done:
    // Free WASM-owned resources. After freeing, null the slot in `ra` so
    // the cleanup hook (wasm_proc_cleanup) is a no-op for these fields
    // when it runs after a normal exit. The args struct itself is freed
    // by the cleanup hook — see process_exit.
    if (runtime) { m3_FreeRuntime(runtime);   ra->runtime = null; }
    if (env)     { m3_FreeEnvironment(env);   ra->env     = null; }
    if (wasm_data) { kfree(wasm_data);        ra->wasm_data = null; }
    if (ra->wasm_path_alloc) { kfree(ra->wasm_path_alloc); ra->wasm_path_alloc = null; }
}

// ============================================================================
// Public API: spawn a new process running the given .wasm.
// Caller must own opts and any string data pointed to by opts->argv (it's copied).
// ============================================================================
i32 wasm_spawn(const wasm_spawn_opts_t *opts) {
    if (!opts || !opts->path) {
        screen_push_line("WASM: path required");
        return -1;
    }

    kern_process_t *proc = process_create();
    if (!proc) {
        screen_push_line("WASM: Failed to create process (OOM)");
        return -1;
    }

    // Args struct is shared between the thread and the per-process cleanup
    // hook. Allocated up front so it survives even if thread creation fails
    // (in which case the cleanup hook will run with all-NULL WASM fields
    // and just kfree the args struct itself).
    wasm_run_args_t *ra = kmalloc(sizeof(wasm_run_args_t));
    if (!ra) {
        screen_push_line("WASM: OOM allocating run args");
        process_exit(proc);
        return -1;
    }
    ra->wasm_path_alloc = str_dup_safe(opts->path, kmalloc);
    ra->wasm_data       = null;
    ra->env             = null;
    ra->runtime         = null;
    ra->stack_kb        = opts->stack_kb > 0 ? opts->stack_kb : 64u;
    ra->wasi_argv       = opts->wasi_argv;
    ra->link_extra      = opts->link_extra;
    ra->link_user       = opts->link_user;

    proc->cleanup_fn  = wasm_proc_cleanup;
    proc->cleanup_ctx = ra;

    // Copy argv into proc->argv (deep copies owned by proc; freed in process_exit).
    if (opts->argc > 0 && opts->argv) {
        u32 to_copy = opts->argc;
        if (to_copy > 16) to_copy = 16;
        proc->argc = (i32) to_copy;
        for (u32 i = 0; i < to_copy; i++) {
            if (opts->argv[i]) {
                proc->argv[i] = str_dup_safe(opts->argv[i], kmalloc);
            }
        }
    }

    // Foreground: this process owns keyboard + the prompt area on the tty.
    if (opts->foreground) {
        foreground_proc = proc;
        keyboard_fg_flush();
    }

    kern_task_t *task = sched_create_process_thread(proc, wasm_thread_entry, ra);
    if (!task) {
        screen_push_line("WASM: Failed to create thread");
        // process_exit will run wasm_proc_cleanup on `ra`, which frees
        // wasm_path_alloc (the only populated field) and the args struct.
        process_exit(proc);
        return -1;
    }

    // Optionally block the caller until the child exits.
    if (opts->wait) {
        i32 pid = proc->pid;
        u8 killed = 0;
        while (sched_get_by_pid(pid) != null) {
            if (opts->foreground) {
                // Pump keys from the kernel queue into the foreground
                // queue so the WASM program can read them via
                // fd_read(fd=0). Without this, input typed while a
                // foreground command is running would silently buffer
                // in the kernel queue and never reach the WASM program.
                u8 k;
                while ((k = keyboard_eat_key())) {
                    if (k == KEY_CTRL_C) {
                        if (!killed) {
                            screen_push_line("^C");
                            serial_outsf("Killed Process %d with Ctrl+C\n", pid);
                            // Mark every thread of the foreground process
                            // DEAD. The reaper will call process_exit,
                            // which runs wasm_proc_cleanup (frees env /
                            // runtime / wasm_data / path / args), then
                            // frees the proc and its fd_table / argv /
                            // mem_regions. The wait loop exits the next
                            // time sched_get_by_pid returns null.
                            sched_kill_process(pid);
                            // Drop anything still queued for the doomed
                            // process so a later command doesn't see
                            // stale input.
                            keyboard_fg_flush();
                            killed = 1;
                        }
                        break;
                    }
                    keyboard_fg_push(k);
                }
                screen_render_shell();
            }
            sched_yield();
        }
    }

    return proc->pid;
}
