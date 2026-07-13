#ifndef WASM_SPAWN_H
#define WASM_SPAWN_H

#include "dialect.h"

// m3_env.h must come from src/kernel/wasm3-0.5.0/source (already on the -I path in build.sh)
// It defines IM3Module, IM3Runtime, and is needed by the link_extra hook below.
#include "m3_env.h"

typedef struct wasm_spawn_opts {
    const char *path;             // required: ".wasm" file path on ext2
    int         argc;             // 0 if no argv; values beyond 16 are truncated
    char       *const *argv;      // argv[0] = program name (e.g. "lsr"); copied into proc->argv
    bool        foreground;       // route stdin to keyboard / take over the prompt
    bool        wait;             // block caller until the process exits
    u32         stack_kb;         // wasm3 stack size for m3_NewRuntime; default = 64 (doom uses 512)
    // wasi_argv: when true, the shim injects argv into wasm linear memory and
    // calls _start(argc, argv_ptr) wasi-style. Default = false (legacy: _start()
    // with no args, programs use env.get_arg_count / env.get_arg at runtime).
    // Set to true ONLY for modules compiled with wasi-sdk's `_start(i32,i32)` ABI.
    bool        wasi_argv;
    // Optional: link program-specific imports *after* the common set.
    // Only invoked for modules with custom host contracts (doom's drawFrame etc.).
    void      (*link_extra)(IM3Module mod, IM3Runtime rt, void *user);
    void       *link_user;
} wasm_spawn_opts_t;

// Spawn a new process running the given WASM module.
// Returns the new process's PID (>=1) or -1 on failure (diagnostics printed to screen+serial).
i32 wasm_spawn(const wasm_spawn_opts_t *opts);

// Thread entry — registered with sched_create_process_thread().
// Does the actual file read, parse/load/link, argv injection, _start call, and cleanup.
u0 wasm_thread_entry(u0 *arg);

// Forward decls of the standard host functions so other TUs (e.g. doom's
// wasm_doom_test in kern_tests.c) can take their address for m3_LinkRawFunction.
// The macro expands to a C forward declaration matching the definitions.
m3ApiRawFunction(wasm_fd_open);
m3ApiRawFunction(wasm_fd_close);
m3ApiRawFunction(wasm_fd_read);
m3ApiRawFunction(wasm_fd_write);

#endif //WASM_SPAWN_H
