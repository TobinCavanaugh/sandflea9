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
#include "../util/str_slice.h"
#include "../include/stbsupport.h"
#include "../include/kern_ext2.h"
#include "../include/kern_profile.h"
#include "../include/kern_ipc.h"
#include "../include/kern_compositor.h"

#include "wasm3-0.5.0/source/m3_env.h"
#include "wasm3-0.5.0/source/m3_api_libc.h"
#include "../external/xxhash/xxhash.h"

// ============================================================================
// Helpers
// ============================================================================

extern volatile u64 sw;

// Local strdup using the new str_view_t system (allocates via kmalloc).
static char * str_dup_safe(const char *s, void *(*alloc)(u64)) {
    (void)alloc;
    return str_view_to_c(str_view_from_c(s));
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
// Parse-result cache — PoC stage.
// Each unique .wasm is parsed ONCE per boot (or until evicted). Subsequent
// spawns compute the same XXH64 hash, walk a single-linked list, find a hit,
// and skip the parse step. The master parsed module is NEVER m3_LoadModule'd
// directly — every spawn gets a deep clone.
//
// Why a deep clone (and not a re-load): wasm3 forbids loading one parsed
// module into multiple runtimes (moduleAlreadyLinked error, "Do not free
// modules once successfully loaded into the runtime"). Cloning is the only
// public-API-safe way to reuse a parse across runs.
//
// Lifetime model:
//   - Cache owns: bytes, master-parsed module, master strings.
//   - Clone shares raw-byte pointers with cache (zero-copy).
//   - Clone DEEP-COPIES all string fields (names[], globals[].name, import
//     moduleUtf8/fieldUtf8) so m3_FreeModule on the clone won't free strings
//     still referenced by master.
//
// Single-core assumption: we don't take a spinlock here. wasm_spawn is
// serialized per the comment near s_wasi_ctx — cache list access is single
// threaded by construction.
// ============================================================================

// Cache entry: one per unique .wasm ever seen by the kernel.
typedef struct wasm_parse_cache_entry {
    XXH64_hash_t                   hash;    // xxh64 of raw bytes, seed=0
    u8                            *bytes;   // kmalloc'd; outlives all clones
    u32                            size;
    IM3Module                      master;  // parsed; never m3_LoadModule'd
    struct wasm_parse_cache_entry *next;
} wasm_parse_cache_entry_t;

// Head of the cache list. Simple prepend-on-insert; O(n) lookup; bounded by
// the number of distinct .wasm files ever spawned. Fine for PoC.
static wasm_parse_cache_entry_t *g_wasm_parse_cache = NULL;

// Shared environment that outlives all runtimes so that cached master
// modules' funcTypes pointers remain valid across spawns. Created lazily
// on first use and never freed (lives for the entire boot).
static IM3Environment g_wasm_cache_env = NULL;

// Linear-list lookup. Returns NULL on miss. O(n); fine for PoC.
static wasm_parse_cache_entry_t *wasm_parse_cache_lookup(XXH64_hash_t hash) {
    wasm_parse_cache_entry_t *e = g_wasm_parse_cache;
    while (e) {
        if (e->hash == hash) return e;
        e = e->next;
    }
    return NULL;
}

// Prepend. Cache takes ownership of bytes + master module — caller MUST NOT
// kfree them after this returns.
static wasm_parse_cache_entry_t *wasm_parse_cache_insert(XXH64_hash_t hash, u8 *bytes, u32 size, IM3Module master) {
    wasm_parse_cache_entry_t *e =
        (wasm_parse_cache_entry_t *) kmalloc(sizeof(wasm_parse_cache_entry_t));
    if (!e) {
        serial_outsl("WASM: cache insert OOM; master not cached");
        return NULL;
    }
    e->hash   = hash;
    e->bytes  = bytes;
    e->size   = size;
    e->master = master;
    e->next   = g_wasm_parse_cache;
    g_wasm_parse_cache = e;
    serial_outsf("WASM: cached parsed module, hash=%llx, size=%u\n",
                 (unsigned long long) hash, size);
    return e;
}

// Deep-clone a parsed module so it can be m3_LoadModule'd into a fresh
// runtime. All raw-byte pointer fields reference `src_bytes` (the same
// persistent buffer the cache owns), so we don't duplicate megabytes.
//
// m3_FreeModule compat: we deep-copy every string that m3_FreeModule
// would otherwise free (so freeing the clone won't touch shared master
// strings). All arrays are kmalloc'd so they can be m3_Free'd independently.
//
// Field-by-field contract:
//  M3Module fields:
//   runtime, next            — NULL (never bound to runtime)
//   environment              — same shared env (clones share funcType dedup)
//   wasmStart/wasmEnd        — shared src_bytes (zero-copy)
//   name                     — shallow (".unnamed" literal; m3_FreeModule
//                              does NOT free module name; only global/func names)
//   funcTypes[]              — fresh array; elements shared with env,
//                              m3_Free frees only the array
//   functions[]              — fresh array; per-function: struct mem_copy,
//                              names[] deep-copied up to numNames;
//                              module back-ptr re-pointed to clone
//   dataSegments[]           — fresh array; initExpr/data pointer fields
//                              shared with src_bytes
//   globals[]                — fresh array; per-global: initExpr shared
//                              with src_bytes; name + import strings
//                              deep-copied because m3_FreeModule's
//                              FreeImportInfo frees them
//   elementSection/elementSectionEnd — shared src_bytes
//   table0                   — left NULL; parse never pre-populates it,
//                              wasm3 lazy-loads against THIS clone's
//                              functions[] when needed
//
// Returns NULL on any allocation failure (caller treats as spawn OOM).
static IM3Module wasm_clone_module(IM3Module src, u8 *src_bytes, IM3Environment env) {
    (void) src_bytes;  // invariant: cache_bytes == src_bytes at call site,
                        // so a RELOC would be a no-op. Kept as a parameter
                        // to document that constraint.

    // kmallocz (m3_Malloc = calloc = kmallocz) so the dst M3Module is
    // guaranteed zero on allocation — unparsed-section fields stay NULL/0
    // without polluting present-section fields (e.g. add_test.wat globals).
    IM3Module dst = (IM3Module) kmallocz(sizeof(M3Module));
    if (!dst) return NULL;

    dst->runtime        = NULL;
    dst->environment    = env;                   // use current env, not
                                                  // master's (which may be
                                                  // freed on cache hit)
    dst->wasmStart      = src->wasmStart;     // shared src_bytes
    dst->wasmEnd        = src->wasmEnd;
    dst->name           = src->name;          // ".unnamed" static literal
    dst->startFunction  = src->startFunction;
    dst->memoryInfo     = src->memoryInfo;
    dst->memoryImported = src->memoryImported;
    dst->next           = NULL;

    // funcTypes: shallow-copy ARRAY (elements shared with env).
    if (src->numFuncTypes > 0) {
        dst->funcTypes = (IM3FuncType *) kmalloc(sizeof(IM3FuncType) * src->numFuncTypes);
        if (!dst->funcTypes) { kfree(dst); return NULL; }
        for (u32 i = 0; i < src->numFuncTypes; i++) {
            dst->funcTypes[i] = src->funcTypes[i];
        }
        dst->numFuncTypes = src->numFuncTypes;
    }
    dst->numFuncImports = src->numFuncImports;

    // functions: deep-copy ARRAY, per-function struct copy + import name
    // deep-copy + name deep-copy.
    //
    // CRITICAL: M3Function has its OWN import.moduleUtf8/fieldUtf8 strings
    // that Function_Release frees via FreeImportInfo. Without deep-copying
    // these, freeing the clone would free master's strings. ALSO: null out
    // all runtime-tied fields (compiled, code, constants, codePageRefs,
    // ownsWasmCode) so a future refactor that does load the master can't
    // silently poison clones with stale pointers.
    if (src->numFunctions > 0) {
        dst->functions = (M3Function *) kmalloc(sizeof(M3Function) * src->numFunctions);
        if (!dst->functions) {
            kfree(dst->funcTypes); kfree(dst); return NULL;
        }
        for (u32 i = 0; i < src->numFunctions; i++) {
            M3Function *sf = &src->functions[i];
            M3Function *df = &dst->functions[i];
            mem_copy((u8 *) df, (u8 *) sf, sizeof(M3Function));
            df->module = dst;          // back-pointer to clone
            df->wasm    = sf->wasm;    // shared src_bytes
            df->wasmEnd = sf->wasmEnd;
            // Invalidate every runtime-tied / mutable field. Master never
            // gets m3_LoadModule'd, so these are NULL/0 today — we're
            // making that invariant explicit so a future change can't break
            // us silently.
            df->compiled         = NULL;
            df->constants        = NULL;
            df->ownsWasmCode     = false;
            // Reset compile-derived scalars. m3_CompileFunction mutates
            // these on first call; starting them at 0 means we don't
            // depend on what kmalloc happened to leave in the master's
            // bytes.
            df->maxStackSlots        = 0;
            df->numRetSlots          = 0;
            df->numRetAndArgSlots    = 0;
            df->numConstantBytes     = 0;
#           if (d_m3EnableCodePageRefCounting)
            df->codePageRefs     = NULL;
            df->numCodePageRefs  = 0;
#           endif
            // Deep-copy import strings — Function_Release's FreeImportInfo
            // will free these. Aliased names[0] (== fieldUtf8) gets a
            // redundant copy; harmless, may waste ~16 bytes per import.
            df->import.moduleUtf8 = NULL;
            df->import.fieldUtf8  = NULL;
            if (sf->import.moduleUtf8) {
                u32 l = str_len(sf->import.moduleUtf8) + 1;
                char *copy = (char *) kmalloc(l);
                mem_copy((u8 *) copy, (const u8 *) sf->import.moduleUtf8, l);
                df->import.moduleUtf8 = copy;
            }
            if (sf->import.fieldUtf8) {
                u32 l = str_len(sf->import.fieldUtf8) + 1;
                char *copy = (char *) kmalloc(l);
                mem_copy((u8 *) copy, (const u8 *) sf->import.fieldUtf8, l);
                df->import.fieldUtf8 = copy;
            }
            // Only the populated names[0..numNames-1] need deep-copy.
            // Function_Release frees non-aliased entries via m3_Free.
            for (u32 n = 0; n < sf->numNames; n++) {
                if (sf->names[n]) {
                    u32 l = str_len(sf->names[n]) + 1;
                    char *copy = (char *) kmalloc(l);
                    mem_copy((u8 *) copy, (const u8 *) sf->names[n], l);
                    df->names[n] = copy;
                } else {
                    df->names[n] = NULL;
                }
            }
        }
        dst->numFunctions = src->numFunctions;
    }

    // dataSegments: deep-copy ARRAY; raw-byte pointer fields relocated.
    if (src->numDataSegments > 0) {
        dst->dataSegments = (M3DataSegment *) kmalloc(sizeof(M3DataSegment) * src->numDataSegments);
        if (!dst->dataSegments) {
            kfree(dst->functions); kfree(dst->funcTypes); kfree(dst); return NULL;
        }
        for (u32 i = 0; i < src->numDataSegments; i++) {
            M3DataSegment *s = &src->dataSegments[i];
            M3DataSegment *d = &dst->dataSegments[i];
            d->memoryRegion = s->memoryRegion;
            d->initExprSize = s->initExprSize;
            d->size         = s->size;
            d->initExpr     = s->initExpr;   // shared src_bytes
            d->data         = s->data;       // shared src_bytes
        }
        dst->numDataSegments = src->numDataSegments;
    }

    // globals: deep-copy ARRAY; raw-byte pointer relocated; strings deep-copied
    // (m3_FreeModule's FreeImportInfo frees them).
    if (src->numGlobals > 0) {
        dst->globals = (M3Global *) kmalloc(sizeof(M3Global) * src->numGlobals);
        if (!dst->globals) {
            kfree(dst->dataSegments); kfree(dst->functions);
            kfree(dst->funcTypes); kfree(dst); return NULL;
        }
        for (u32 i = 0; i < src->numGlobals; i++) {
            M3Global *s = &src->globals[i];
            M3Global *d = &dst->globals[i];
            d->type         = s->type;
            d->imported     = s->imported;
            d->isMutable    = s->isMutable;
            d->intValue     = s->intValue;
            d->initExpr     = s->initExpr;    // shared src_bytes
            d->initExprSize = s->initExprSize;
            d->name = NULL;
            d->import.moduleUtf8 = NULL;
            d->import.fieldUtf8  = NULL;

            if (s->name) {
                u32 l = str_len(s->name) + 1;
                char *copy = (char *) kmalloc(l);
                mem_copy((u8 *) copy, (const u8 *) s->name, l);
                d->name = copy;
            }
            if (s->import.moduleUtf8) {
                u32 l = str_len(s->import.moduleUtf8) + 1;
                char *copy = (char *) kmalloc(l);
                mem_copy((u8 *) copy, (const u8 *) s->import.moduleUtf8, l);
                d->import.moduleUtf8 = copy;
            }
            if (s->import.fieldUtf8) {
                u32 l = str_len(s->import.fieldUtf8) + 1;
                char *copy = (char *) kmalloc(l);
                mem_copy((u8 *) copy, (const u8 *) s->import.fieldUtf8, l);
                d->import.fieldUtf8 = copy;
            }
        }
        dst->numGlobals = src->numGlobals;
    }

    // element section: byte-pointer fields into src_bytes; left shared.
    dst->numElementSegments = src->numElementSegments;
    dst->elementSection     = src->elementSection;
    dst->elementSectionEnd  = src->elementSectionEnd;

    // table0: NOT pre-populated by parse. m3 lazy-load fills it against
    // THIS clone's own functions[] array — don't pre-fill here.
    dst->table0 = NULL;
    dst->table0Size = 0;

    return dst;
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
    serial_outsf("wasm close: fs_close returned %d, about to m3ApiReturn\n", result);
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
                    term_write((const char *)host_buf, len);
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

        // Save/restore active drive — lsr is read-only.
        drive_t *saved_drive = active_drive;

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
            ext2_switch_drive(saved_drive);
            m3ApiReturn(0);
        } else {
            screen_push_linef("lsr: Path not found: %s", path);
            ext2_switch_drive(saved_drive);
            m3ApiReturn(-1);
        }
    } else {
        m3ApiReturn(-1);
    }
}

// Flat (non-recursive) directory listing — used by ls.wasm.
// Saves/restores active drive so ls doesn't permanently switch drives.
m3ApiRawFunction(wasm_ls) {
    m3ApiReturnType (i32)
    m3ApiGetArg     (u32, path_offset)

    u32 memory_size = 0;
    u8 *mem = m3_GetMemory(runtime, &memory_size, 0);
    if (mem && path_offset < memory_size) {
        const char *path = (const char *) (mem + path_offset);

        // Save active drive — ext2_find_path may switch it, but ls is
        // read-only and should not change the user's working drive.
        drive_t *saved_drive = active_drive;

        u32 inode_no = 2;
        ext2_inode_t *dir_inode = ext2_find_path(path, &inode_no);
        if (!dir_inode) {
            screen_push_linef("ls: path not found: %s", path);
            ext2_switch_drive(saved_drive);
            m3ApiReturn(-1);
        }
        if ((dir_inode->mode & 0xF000) != 0x4000) {
            screen_push_line(path);
            kfree(dir_inode);
            ext2_switch_drive(saved_drive);
            m3ApiReturn(0);
        }

        u32 block_size = active_drive->block_size;
        u8 *dir_data = kmalloc(block_size);
        if (dir_data) {
            u32 total_blocks = (dir_inode->size + block_size - 1) / block_size;
            for (u32 b = 0; b < total_blocks; b++) {
                u32 phys_block = ext2_get_bmap(dir_inode, b);
                if (phys_block == 0) continue;
                ext2_read_block(phys_block, dir_data);
                u32 cur_offset = 0;
                while (cur_offset < block_size) {
                    ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(dir_data + cur_offset);
                    if (entry->rec_len < 8) break;
                    if (entry->inode != 0) {
                        if (entry->name_len != 1 || entry->name[0] != '.') {
                            if (entry->name_len != 2 || entry->name[0] != '.' || entry->name[1] != '.') {
                                char name[256];
                                u32 ncopy = entry->name_len < 255 ? entry->name_len : 255;
                                mem_copy((u8*)name, (u8*)entry->name, ncopy);
                                name[ncopy] = '\0';
                                const char *type = (entry->file_type == 2) ? "/" : "";
                                screen_push_linef("%-32s %s", name, type);
                            }
                        }
                    }
                    cur_offset += entry->rec_len;
                }
            }
            kfree(dir_data);
        }
        kfree(dir_inode);
        ext2_switch_drive(saved_drive);
        m3ApiReturn(0);
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
// Kernel-native WASI implementation
// Maps wasi_snapshot_preview1 host functions to sandfleaOS kernel APIs.
// Avoids depending on m3_api_wasi.c (which requires POSIX headers unavailable
// in our freestanding kernel).  All file I/O goes through fs_open/read/write/close.
// ============================================================================

// WASI errno constants
#define WESI_ESUCCESS   0
#define WESI_EBADF      8
#define WESI_EINVAL     28
#define WESI_ENOENT     44
#define WESI_ENOSYS     52
#define WESI_ENOTDIR    54
#define WESI_ENOTSUP    58
#define WESI_EEXIST     20
#define WESI_EACCES     2

// WASI file types for fd_fdstat_get
#define WESI_FILETYPE_DIRECTORY        3
#define WESI_FILETYPE_REGULAR_FILE     4
#define WESI_FILETYPE_CHARACTER_DEVICE 5

// WASI lookup flags
#define WESI_LOOKUPFLAGS_SYMLINK_FOLLOW 1

// WASI oflags
#define WESI_OFLAGS_CREAT     (1 << 0)
#define WESI_OFLAGS_DIRECTORY (1 << 1)
#define WESI_OFLAGS_EXCL      (1 << 2)
#define WESI_OFLAGS_TRUNC     (1 << 3)

// WASI rights (returned by fdstat but not enforced in MVP)
#define WESI_RIGHT_FD_READ     (1ULL << 1)
#define WESI_RIGHT_FD_WRITE    (1ULL << 5)
#define WESI_RIGHT_FD_SEEK     (1ULL << 7)
#define WESI_RIGHT_PATH_OPEN   (1ULL << 8)
#define WESI_RIGHT_FD_READDIR  (1ULL << 13)

// WASI clock IDs
#define WESI_CLOCK_MONOTONIC 1
#define WESI_CLOCK_REALTIME  0

// WASI whence for fd_seek
#define WESI_WHENCE_SET 0
#define WESI_WHENCE_CUR 1
#define WESI_WHENCE_END 2

// Preopened directory: fd 3 is "/" (standard WASI convention)
#define WESI_PREOPEN_FD   3
#define WESI_PREOPEN_PATH "/"

// Context for the WASI implementation — populated before calling _start.
// Uses a static because wasm_spawn is serialized (each spawn waits for
// the previous one to finish before starting the next).
static struct {
    u32   argc;
    char **argv;
    i32   exit_code;
} s_wasi_ctx;

// Forward decl so m3ApiRawFunction functions can call each other
// (currently unused, kept for cleanliness)

// ----------------------------------------------------------------
// WASI: args_sizes_get
// Returns the number of arguments and the total size of the argument buffer.
//
// args_sizes_get(argc_ptr: ptr<i32>, argv_buf_size_ptr: ptr<i32>) -> errno<i32>
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_args_sizes_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, argc_ptr)
    m3ApiGetArg(u32, argv_buf_size_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) m3ApiReturn(WESI_EINVAL);

    if (argc_ptr + 4 > mem_size || argv_buf_size_ptr + 4 > mem_size)
        m3ApiReturn(WESI_EINVAL);

    *(i32*)(mem + argc_ptr) = (i32)s_wasi_ctx.argc;

    // Calculate total buffer size: sum of strlen(arg[i]) + 1 for each arg
    u32 buf_size = 0;
    for (u32 i = 0; i < s_wasi_ctx.argc && i < 16; i++) {
        if (s_wasi_ctx.argv[i])
            buf_size += str_len(s_wasi_ctx.argv[i]) + 1;
    }
    *(i32*)(mem + argv_buf_size_ptr) = (i32)buf_size;

    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: args_get
// Writes argument strings and pointer table into WASM linear memory.
//
// args_get(argv_ptr: ptr, argv_buf_ptr: ptr) -> errno
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_args_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, argv_ptr)     // pointer to the argv pointer table in WASM memory
    m3ApiGetArg(u32, argv_buf_ptr) // pointer to the string buffer in WASM memory

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) m3ApiReturn(WESI_EINVAL);

    u32 str_off = argv_buf_ptr;
    for (u32 i = 0; i < s_wasi_ctx.argc && i < 16; i++) {
        if (!s_wasi_ctx.argv[i]) break;

        // Write pointer to argv table
        if (argv_ptr + i * 4 + 4 > mem_size) m3ApiReturn(WESI_EINVAL);
        *(u32*)(mem + argv_ptr + i * 4) = str_off;

        // Write string
        const char *s = s_wasi_ctx.argv[i];
        u32 len = str_len(s) + 1;
        if (str_off + len > mem_size) m3ApiReturn(WESI_EINVAL);
        mem_copy(mem + str_off, (const u8*)s, len);
        str_off += len;
    }

    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: environ_sizes_get / environ_get
// Stub — no environment variables in sandfleaOS yet.
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_environ_sizes_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, environc_ptr)
    m3ApiGetArg(u32, environ_buf_size_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || environc_ptr + 4 > mem_size || environ_buf_size_ptr + 4 > mem_size)
        m3ApiReturn(WESI_EINVAL);

    *(i32*)(mem + environc_ptr) = 0;
    *(i32*)(mem + environ_buf_size_ptr) = 0;
    m3ApiReturn(WESI_ESUCCESS);
}

m3ApiRawFunction(kern_wasi_environ_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, environ_ptr)
    m3ApiGetArg(u32, environ_buf_ptr)
    // No environment — nothing to do
    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_close
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_close) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)

    if (fd < 3) m3ApiReturn(WESI_ESUCCESS);  // stdin/stdout/stderr: no-op
    i32 res = fs_close(fd);
    m3ApiReturn(res == 0 ? WESI_ESUCCESS : WESI_EBADF);
}

// ----------------------------------------------------------------
// WASI: fd_seek
// WASI whence: 0=SET, 1=CUR, 2=END (same as our SEEK_SET/CUR/END)
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_seek) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(i64, offset)
    m3ApiGetArg(i32, whence)
    m3ApiGetArg(u32, newoffset_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || newoffset_ptr + 8 > mem_size) m3ApiReturn(WESI_EINVAL);

    if (fd < 3) m3ApiReturn(WESI_ENOSYS);  // can't seek stdin/stdout/stderr

    i32 res = fs_seek(fd, (i32)offset, whence);
    if (res < 0) m3ApiReturn(WESI_EBADF);

    i32 new_pos = fs_tell(fd);
    if (new_pos < 0) m3ApiReturn(WESI_EBADF);

    *(i64*)(mem + newoffset_ptr) = (i64)new_pos;
    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_read
// WASI iovec structure: [buf_ptr: u32, buf_len: u32]
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_read) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, iovs_ptr)
    m3ApiGetArg(i32, iovs_len)
    m3ApiGetArg(u32, nread_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem) m3ApiReturn(WESI_EINVAL);

    if (nread_ptr + 4 > mem_size) m3ApiReturn(WESI_EINVAL);
    if (iovs_ptr + iovs_len * 8 > mem_size) m3ApiReturn(WESI_EINVAL);

    u32 total_read = 0;

    for (i32 i = 0; i < iovs_len; i++) {
        u32 buf_off = *(u32*)(mem + iovs_ptr + i * 8);
        u32 buf_len = *(u32*)(mem + iovs_ptr + i * 8 + 4);

        if (buf_off + buf_len > mem_size) continue;
        if (buf_len == 0) continue;

        u8 *kernel_buf = mem + buf_off;

        if (fd == 0) {
            // stdin: read from keyboard foreground queue
            // (same logic as wasm_fd_read for fd==0)
            u32 rd = 0;
            while (rd < buf_len) {
                u8 k = keyboard_fg_eat();
                if (k != 0) {
                    if (k == '\r') k = '\n';
                    if (k == '\b') {
                        if (rd > 0) rd--;
                        continue;
                    }
                    kernel_buf[rd++] = k;
                    screen_push_buf((const char *)&k, 1);
                    if (k == '\n') break;
                } else {
                    sched_yield();
                }
            }
            total_read += rd;
        } else {
            i32 br = fs_read(fd, kernel_buf, buf_len);
            if (br <= 0) break;
            total_read += br;
        }
    }

    *(u32*)(mem + nread_ptr) = total_read;
    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_write
// WASI iovec structure: [buf_ptr: u32, buf_len: u32]
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_write) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, iovs_ptr)
    m3ApiGetArg(i32, iovs_len)
    m3ApiGetArg(u32, nwritten_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    serial_outsf("WASI fd_write: fd=%d iovs=0x%x len=%d nwritten=0x%x mem_sz=%u\n",
                 fd, iovs_ptr, iovs_len, nwritten_ptr, mem_size);
    if (!mem) {
        serial_outsl("WASI fd_write: NULL memory — aborting");
        m3ApiReturn(WESI_EINVAL);
    }

    if (nwritten_ptr + 4 > mem_size) m3ApiReturn(WESI_EINVAL);
    if (iovs_ptr + iovs_len * 8 > mem_size) m3ApiReturn(WESI_EINVAL);

    u32 total_written = 0;
    kern_process_t *proc = sched_get_current_process();

    for (i32 i = 0; i < iovs_len; i++) {
        u32 buf_off = *(u32*)(mem + iovs_ptr + i * 8);
        u32 buf_len = *(u32*)(mem + iovs_ptr + i * 8 + 4);

        if (buf_off + buf_len > mem_size) continue;
        if (buf_len == 0) continue;

        u8 *host_buf = mem + buf_off;

        if (fd == 1 || fd == 2) {
            // stdout/stderr: write to terminal
            term_write((const char *)host_buf, buf_len);
            total_written += buf_len;
        } else if (proc && fd >= 0 && fd < MAX_FILE_HANDLES && proc->fd_table[fd] != null) {
            i32 res = fs_write(fd, host_buf, buf_len);
            if (res < 0) break;
            total_written += res;
        } else {
            break;
        }
    }

    *(u32*)(mem + nwritten_ptr) = total_written;
    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_fdstat_get
// Returns file descriptor metadata (type, flags, rights).
// fd: file descriptor
// stat_ptr: pointer to __wasi_fdstat_t (24 bytes)
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_fdstat_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, stat_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || stat_ptr + 24 > mem_size) m3ApiReturn(WESI_EINVAL);

    // Determine file type and rights based on fd
    u32 filetype;
    u64 rights_base;
    u64 rights_inheriting = 0;

    switch (fd) {
        case 0:  // stdin
            filetype = WESI_FILETYPE_CHARACTER_DEVICE;
            rights_base = WESI_RIGHT_FD_READ;
            break;
        case 1:  // stdout
        case 2:  // stderr
            filetype = WESI_FILETYPE_CHARACTER_DEVICE;
            rights_base = WESI_RIGHT_FD_WRITE;
            break;
        case WESI_PREOPEN_FD:  // preopened root directory
            filetype = WESI_FILETYPE_DIRECTORY;
            rights_base = WESI_RIGHT_PATH_OPEN | WESI_RIGHT_FD_READDIR;
            break;
        default:  // regular file
            if (fd >= MAX_FILE_HANDLES) m3ApiReturn(WESI_EBADF);
            filetype = WESI_FILETYPE_REGULAR_FILE;
            rights_base = WESI_RIGHT_FD_READ | WESI_RIGHT_FD_WRITE | WESI_RIGHT_FD_SEEK;
            break;
    }

    // Layout of __wasi_fdstat_t (24 bytes):
    //   fs_filetype: u8  (offset 0)
    //   fs_flags:    u16 (offset 2)
    //   fs_rights_base: u64 (offset 8)
    //   fs_rights_inheriting: u64 (offset 16)
    mem_set(mem + stat_ptr, 0, 24);
    mem[stat_ptr] = filetype;           // fs_filetype
    *(u64*)(mem + stat_ptr + 8) = rights_base;
    *(u64*)(mem + stat_ptr + 16) = rights_inheriting;

    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_fdstat_set_flags (stub — no-op)
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_fdstat_set_flags) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(i32, flags)
    // Not supported — return success (programs continue anyway)
    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_prestat_get
// Returns preopened directory info. fd 3 = "/"
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_prestat_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, prestat_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || prestat_ptr + 8 > mem_size) m3ApiReturn(WESI_EINVAL);

    if (fd == WESI_PREOPEN_FD) {
        // __wasi_prestat_t: { tag: u8 (0=dir), u32: pr_name_len }
        mem[prestat_ptr] = 0;  // tag = __WASI_PREOPEN_TYPE_DIR
        *(u32*)(mem + prestat_ptr + 4) = 1;  // strlen("/") = 1
        m3ApiReturn(WESI_ESUCCESS);
    }

    m3ApiReturn(WESI_EBADF);
}

// ----------------------------------------------------------------
// WASI: fd_prestat_dir_name
// Returns the preopened directory name for a given fd.
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_prestat_dir_name) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, path_ptr)
    m3ApiGetArg(u32, path_max_len)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || path_ptr + path_max_len > mem_size) m3ApiReturn(WESI_EINVAL);

    if (fd == WESI_PREOPEN_FD) {
        if (path_max_len < 1) m3ApiReturn(WESI_EINVAL);
        mem[path_ptr] = '/';
        m3ApiReturn(WESI_ESUCCESS);
    }

    m3ApiReturn(WESI_EBADF);
}

// ----------------------------------------------------------------
// WASI: path_open
// Opens a file relative to a preopened directory.
// dir_fd: preopened directory fd (3 for "/")
// path_ptr / path_len: the file path relative to dir_fd
// oflags: open flags (creat, directory, excl, trunc)
// Returns the new fd via ret_fd ptr.
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_path_open) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, dir_fd)
    m3ApiGetArg(i32, dirflags)
    m3ApiGetArg(u32, path_ptr)
    m3ApiGetArg(i32, path_len)
    m3ApiGetArg(i32, oflags)
    m3ApiGetArg(i64, fs_rights_base)
    m3ApiGetArg(i64, fs_rights_inheriting)
    m3ApiGetArg(i32, fdflags)
    m3ApiGetArg(u32, ret_fd_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || ret_fd_ptr + 4 > mem_size) m3ApiReturn(WESI_EINVAL);
    if (path_ptr + path_len > mem_size) m3ApiReturn(WESI_EINVAL);

    // Build null-terminated path from WASM memory
    // (copy to a local buffer since path_len may not include the null terminator)
    char path_buf[256];
    u32 copy_len = (u32)path_len;
    if (copy_len > 250) copy_len = 250;
    mem_copy((u8*)path_buf, mem + path_ptr, copy_len);
    path_buf[copy_len] = 0;

    serial_outsf("WASI: path_open(%s) oflags=0x%x\n", path_buf, oflags);

    // Open the file
    i32 new_fd = fs_open(path_buf);

    // If file doesn't exist and CREAT flag is set, create it
    if (new_fd < 0 && (oflags & WESI_OFLAGS_CREAT)) {
        new_fd = fs_create(path_buf);
        if (new_fd < 0) {
            serial_outsf("WASI: path_open create failed: %s\n", path_buf);
            m3ApiReturn(WESI_ENOENT);
        }
        serial_outsf("WASI: path_open created file: %s (fd=%d)\n", path_buf, new_fd);
    } else if (new_fd < 0) {
        m3ApiReturn(WESI_ENOENT);
    }

    *(i32*)(mem + ret_fd_ptr) = new_fd;
    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: path_filestat_get
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_path_filestat_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, dir_fd)
    m3ApiGetArg(u32, flags)
    m3ApiGetArg(u32, path_ptr)
    m3ApiGetArg(u32, path_len)
    m3ApiGetArg(u32, stat_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || stat_ptr + 64 > mem_size) m3ApiReturn(WESI_EINVAL);
    if (path_ptr + path_len > mem_size) m3ApiReturn(WESI_EINVAL);

    char path_buf[256];
    u32 copy_len = path_len < 250 ? path_len : 250;
    mem_copy((u8*)path_buf, mem + path_ptr, copy_len);
    path_buf[copy_len] = 0;

    i32 fd = fs_open(path_buf);
    if (fd < 0) m3ApiReturn(WESI_ENOENT);

    u32 fsize = fs_size(fd);
    fs_close(fd);

    mem_set(mem + stat_ptr, 0, 64);
    mem[stat_ptr + 16] = WESI_FILETYPE_REGULAR_FILE;  // 4
    *(u64*)(mem + stat_ptr + 32) = (u64)fsize;

    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_filestat_get
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_filestat_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiGetArg(u32, stat_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || stat_ptr + 64 > mem_size) m3ApiReturn(WESI_EINVAL);

    u32 filetype;
    u64 fsize = 0;

    if (fd < 3) {
        filetype = WESI_FILETYPE_CHARACTER_DEVICE;  // 5
    } else if (fd == WESI_PREOPEN_FD) {
        filetype = WESI_FILETYPE_DIRECTORY;         // 3
    } else {
        if (fd >= MAX_FILE_HANDLES) m3ApiReturn(WESI_EBADF);
        i32 sz = fs_size(fd);
        if (sz < 0) m3ApiReturn(WESI_EBADF);
        filetype = WESI_FILETYPE_REGULAR_FILE;      // 4
        fsize = (u64)sz;
    }

    mem_set(mem + stat_ptr, 0, 64);
    mem[stat_ptr + 16] = filetype;
    *(u64*)(mem + stat_ptr + 32) = fsize;

    m3ApiReturn(WESI_ESUCCESS);
}


// ----------------------------------------------------------------
// WASI: proc_exit
// Terminates the process with the given exit code.
// Uses m3ApiTrap() to stop execution (the caller checks the ctx).
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_proc_exit) {
    m3ApiGetArg(i32, code)

    s_wasi_ctx.exit_code = code;
    serial_outsf("WASI: proc_exit(%d)\n", code);

    // Use m3ApiTrap to unwind the wasm3 execution stack.
    // The caller (wasm_thread_entry) will see the trap, log it (or not),
    // and then fall through to cleanup.  The exit_code is in the context.
    m3ApiTrap("proc_exit");
}

// ----------------------------------------------------------------
// WASI: random_get
// Fills a buffer with pseudo-random bytes.
// Uses a simple LCG seeded from the kernel timer.
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_random_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(u32, buf_ptr)
    m3ApiGetArg(i32, buf_len)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || buf_ptr + (u32)buf_len > mem_size) m3ApiReturn(WESI_EINVAL);

    // Simple LCG seeded from kernel timer
    static u64 rand_state = 0;
    if (rand_state == 0) rand_state = sw;

    for (i32 i = 0; i < buf_len; i++) {
        rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
        mem[buf_ptr + i] = (u8)(rand_state >> 32);
    }

    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: clock_time_get
// Returns the current time for the given clock id.
// clock_id 0 = REALTIME, 1 = MONOTONIC
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_clock_time_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, clock_id)
    m3ApiGetArg(i64, precision)
    m3ApiGetArg(u32, time_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || time_ptr + 8 > mem_size) m3ApiReturn(WESI_EINVAL);

    // Return kernel timer value in nanoseconds (sw ticks at ~10ms = 10,000,000 ns)
    // This gives us the monotonic clock with ~10ms resolution.
    i64 time_ns = (i64)sw * 10000000LL;
    *(i64*)(mem + time_ptr) = time_ns;

    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: clock_res_get
// Returns the resolution of the given clock.
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_clock_res_get) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, clock_id)
    m3ApiGetArg(u32, resolution_ptr)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || resolution_ptr + 8 > mem_size) m3ApiReturn(WESI_EINVAL);

    // ~10ms resolution = 10,000,000 ns
    *(i64*)(mem + resolution_ptr) = 10000000LL;

    m3ApiReturn(WESI_ESUCCESS);
}

// ----------------------------------------------------------------
// WASI: fd_datasync (stub — no-op)
// ----------------------------------------------------------------
m3ApiRawFunction(kern_wasi_fd_datasync) {
    m3ApiReturnType(i32)
    m3ApiGetArg(i32, fd)
    m3ApiReturn(WESI_ESUCCESS);  // no-op: our ext2 driver doesn't cache writes
}

// ----------------------------------------------------------------
// Link all WASI functions into the module under wasi_snapshot_preview1.
// Called from wasm_thread_entry before _start.
// ----------------------------------------------------------------
static u0 kern_link_wasi(IM3Module module, kern_process_t *proc) {
    if (!module) return;

    // Populate the static context
    if (proc) {
        s_wasi_ctx.argc = proc->argc;
        s_wasi_ctx.argv = proc->argv;
    } else {
        s_wasi_ctx.argc = 0;
        s_wasi_ctx.argv = null;
    }
    s_wasi_ctx.exit_code = 0;

    const char *ns = "wasi_snapshot_preview1";

    // Suppress lookup failure macro: if a module doesn't import a particular
    // function, m3_LinkRawFunction returns an error — we ignore it.
    // However, if there is a signature/type mismatch, we log it.
#define LINK(name, sig, fn) do {                            \
    M3Result _r = m3_LinkRawFunction(module, ns, name, sig, fn); \
    if (_r && _r != m3Err_functionLookupFailed) { \
        serial_outsf("WASI: Link failed for %s (%s): %s\n", name, sig, _r); \
    } \
} while(0)

    LINK("args_sizes_get",       "i(ii)", &kern_wasi_args_sizes_get);
    LINK("args_get",             "i(ii)", &kern_wasi_args_get);
    LINK("environ_sizes_get",    "i(ii)", &kern_wasi_environ_sizes_get);
    LINK("environ_get",          "i(ii)", &kern_wasi_environ_get);
    LINK("fd_close",             "i(i)",  &kern_wasi_fd_close);
    LINK("fd_fdstat_get",        "i(ii)", &kern_wasi_fd_fdstat_get);
    LINK("fd_fdstat_set_flags",  "i(ii)", &kern_wasi_fd_fdstat_set_flags);
    LINK("fd_prestat_dir_name",  "i(iii)",&kern_wasi_fd_prestat_dir_name);
    LINK("fd_prestat_get",       "i(ii)", &kern_wasi_fd_prestat_get);
    LINK("fd_read",              "i(iiii)",&kern_wasi_fd_read);
    LINK("fd_seek",              "i(iIii)",&kern_wasi_fd_seek);
    LINK("fd_write",             "i(iiii)",&kern_wasi_fd_write);
    LINK("path_open",            "i(iiiiiIIii)", &kern_wasi_path_open);
    LINK("proc_exit",            "v(i)",  &kern_wasi_proc_exit);
    LINK("random_get",           "i(ii)", &kern_wasi_random_get);
    LINK("clock_time_get",       "i(iIi)", &kern_wasi_clock_time_get);
    LINK("clock_res_get",        "i(ii)", &kern_wasi_clock_res_get);
    LINK("fd_datasync",          "i(i)",  &kern_wasi_fd_datasync);
    LINK("path_filestat_get",    "i(iiiii)", &kern_wasi_path_filestat_get);
    LINK("fd_filestat_get",      "i(ii)", &kern_wasi_fd_filestat_get);

    // Also link under wasi_unstable (legacy name)
    const char *ns_unstable = "wasi_unstable";
    LINK("fd_seek",              "i(iIii)", &kern_wasi_fd_seek);

#undef LINK

    serial_outsl("WASI: Linked kernel-native WASI implementation");
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
    IM3Environment cache_env = null;   // long-lived env for m3_ParseModule
    IM3Runtime     runtime   = null;
    u8            *wasm_data = null;
    kern_process_t *proc = sched_get_current_process();

    // Top-level scope: auto-closes on any exit (return, goto, fall-through).
    PROFILE_SCOPE("wasm:thread_entry");
    serial_outsf("WASM: Loading %s\n", wasm_path);

    // 1. Read file from disk — block-scoped so goto is safe.
    u32 wasm_size;
    {
        PROFILE_SCOPE("wasm:fs_load");
        i32 fd = fs_open(wasm_path);
        if (fd < 0) {
            screen_push_linef("WASM: Could not open %s", wasm_path);
            goto Label_Done;
        }
        wasm_size = fs_size(fd);
        wasm_data = kmalloc(wasm_size);
        if (!wasm_data) {
            screen_push_line("WASM: Out of memory for WASM data");
            fs_close(fd);
            goto Label_Done;
        }
        ra->wasm_data = wasm_data;
        fs_read(fd, wasm_data, wasm_size);
        fs_close(fd);
    }

    // 2. Allocate environment + runtime.
    //
    // g_wasm_cache_env is a long-lived environment whose ONLY purpose is
    // to keep parsed funcType pointers alive for cached master modules.
    // It is NEVER passed to m3_NewRuntime — each runtime gets its own
    // private environment so code pages (pagesReleased list) are not
    // shared across concurrent WASM processes.  Sharing the page pool
    // caused a data race in RemoveCodePageOfCapacity → code page
    // corruption → #GP at consistent RIP.
    //
    // cache_env : long-lived, used by m3_ParseModule only.
    // env        : per-runtime, used by m3_NewRuntime + clone environment field.
    {
        PROFILE_SCOPE("wasm:env_runtime");
        if (!g_wasm_cache_env) {
            g_wasm_cache_env = m3_NewEnvironment();
            if (!g_wasm_cache_env) {
                screen_push_line("WASM: Could not create shared parse environment");
                goto Label_Done;
            }
        }
        cache_env = g_wasm_cache_env;

        // Per-runtime environment: owns this runtime's code page pool.
        IM3Environment runtime_env = m3_NewEnvironment();
        if (!runtime_env) {
            screen_push_line("WASM: Could not create runtime environment");
            goto Label_Done;
        }
        env = runtime_env;            // for clone / local use
        ra->env = runtime_env;        // tracked so cleanup can free it

        u32 stack_bytes = ra->stack_kb * 1024u;
        runtime = m3_NewRuntime(runtime_env, stack_bytes, null);
        if (!runtime) {
            screen_push_line("WASM: Could not create runtime");
            m3_FreeEnvironment(runtime_env);
            ra->env = null;
            goto Label_Done;
        }
        ra->runtime = runtime;
    }

    // 3. Hash + cache lookup, then parse-or-clone, then load.
    // PoC parse-cache: on a hash hit we skip m3_ParseModule entirely;
    // on a miss we parse once and stash the master in the cache.
    XXH64_hash_t wasm_hash;
    {
        PROFILE_SCOPE("wasm:hash");
        wasm_hash = XXH64(wasm_data, wasm_size, 0);
    }
    wasm_parse_cache_entry_t *cache_entry = wasm_parse_cache_lookup(wasm_hash);

    IM3Module module = null;
    M3Result result;
    if (cache_entry) {
        // HIT: skip parse. The hash_buf is throwaway; cache_entry->bytes is
        // the persistent buffer that survives across spawns.
        {
            PROFILE_SCOPE("wasm:cache_hit");
            serial_outsf("WASM: cache hit %s (hash=%llx)\n",
                         wasm_path, (unsigned long long) wasm_hash);
            kfree(wasm_data);
            wasm_data = null;
            ra->wasm_data = null;
            module = wasm_clone_module(cache_entry->master, cache_entry->bytes, env);
            if (!module) {
                screen_push_linef("WASM: clone failed (cache hit for %s)", wasm_path);
                goto Label_Done;
            }
            cache_entry = NULL;
        }
    } else {
        // MISS: parse, then hand bytes + master to the cache, then clone for
        // THIS spawn so m3_LoadModule can take ownership of the clone while
        // master stays unlinked.
        {
            PROFILE_SCOPE("wasm:parse");
            result = m3_ParseModule(cache_env, &module, wasm_data, wasm_size);
            if (result || !module) {
                screen_push_linef("WASM: Parse error: %s", result ? result : "null module");
                goto Label_Done;
            }
            // Hand wasm_data + master to the cache; ownership transfers.
            wasm_parse_cache_entry_t *inserted =
                wasm_parse_cache_insert(wasm_hash, wasm_data, wasm_size, module);
            if (!inserted) {
                // OOM: cache failed but parse succeeded. Fall back to loading
                // the master directly (no cache benefit, no clone). Bytes
                // stay owned by us so Label_Done can kfree them.
                goto do_load;
            }
            // After insert: cache owns wasm_data and master module.
            wasm_data = null;
            ra->wasm_data = null;
            module = wasm_clone_module(inserted->master, inserted->bytes, env);
            if (!module) {
                screen_push_linef("WASM: clone failed after parse for %s", wasm_path);
                // The clone alloc failed but the master is fine in cache.
                // We just can't run THIS spawn. Fall through to free
                // everything via Label_Done. Bytes are owned by cache, so
                // they survive.
                goto Label_Done;
            }
            cache_entry = NULL;
        }
    }
do_load:
    // m3_LoadModule transfers ownership of the module to the runtime.
    // Per wasm3.h: "Do not free modules once successfully loaded into
    // the runtime" and "only modules not loaded into a M3Runtime need
    // to be freed. A module is considered unloaded if m3_LoadModule
    // returned a result."
    result = m3_LoadModule(runtime, module);
    if (result) {
        screen_push_linef("WASM: Load error: %s", result);
        m3_FreeModule(module);
        module = null;
        goto Label_Done;
    }

    // 4. Link common imports (every program gets fd_*, get_arg_*, and env.lsr).
    {
        PROFILE_SCOPE("wasm:link");
        m3_LinkRawFunction(module, "env", "fd_open",         "i(i)",    &wasm_fd_open);
    m3_LinkRawFunction(module, "env", "fd_read",         "i(iii)",  &wasm_fd_read);
    m3_LinkRawFunction(module, "env", "fd_close",        "i(i)",    &wasm_fd_close);
    m3_LinkRawFunction(module, "env", "fd_write",        "i(iiii)", &wasm_fd_write);
        m3_LinkRawFunction(module, "env", "lsr",             "i(i)",    &wasm_lsr);
        m3_LinkRawFunction(module, "env", "ls",              "i(i)",    &wasm_ls);
        m3_LinkRawFunction(module, "env", "get_arg_count",   "i()",     &wasm_get_arg_count);
        m3_LinkRawFunction(module, "env", "get_arg",         "i(iii)",  &wasm_get_arg);

        // 4b. IPC host functions (kern_ipc.c). Best-effort — modules that
        //     don't import them will silently skip.
        m3_LinkRawFunction(module, "env", "ipc_get_pid",     "i()",     &wasm_ipc_get_pid);
        m3_LinkRawFunction(module, "env", "ipc_setup_wait",  "i(ii)",   &wasm_ipc_setup_wait);
        m3_LinkRawFunction(module, "env", "ipc_signal_send", "i(ii)",   &wasm_ipc_signal_send);
        m3_LinkRawFunction(module, "env", "ipc_signal_wait", "i(i)",    &wasm_ipc_signal_wait);

        // 4c. Compositor host functions (kern_compositor.c). Best-effort.
        //     Non-compositor modules silently skip these.
        m3_LinkRawFunction(module, "display", "claimCompositor", "i()",      &wasm_display_claim_compositor);
        m3_LinkRawFunction(module, "display", "getResolution",   "i()",      &wasm_display_get_resolution);
        m3_LinkRawFunction(module, "display", "present",         "i(i)",     &wasm_display_present);
        m3_LinkRawFunction(module, "display", "claimBuffer",     "i()",      &wasm_display_claim_buffer);
        m3_LinkRawFunction(module, "display", "blitFromPid",     "i(iiiiiii)", &wasm_display_blit_from_pid);
        m3_LinkRawFunction(module, "input",   "pollEvents",      "i(ii)",    &wasm_input_poll_events);
        m3_LinkRawFunction(module, "proc",    "spawn",           "i(iii)",   &wasm_compositor_proc_spawn);
        m3_LinkRawFunction(module, "proc",    "dequeueSpawn",     "i()",      &wasm_compositor_proc_dequeue_spawn);
        m3_LinkRawFunction(module, "proc",    "signal",          "i(iii)",   &wasm_compositor_proc_signal);

        // 5. Program-specific imports (doom-style). Best-effort: missing imports
        //    will surface as m3 errors during _start anyway.
        if (ra->link_extra) ra->link_extra(module, runtime, ra->link_user);

        // 6. WASI link (required for wasi-sdk compiled modules).
        // Links wasi_snapshot_preview1 imports using our kernel-native
        // implementation that maps directly to sandfleaOS kernel APIs.
        kern_link_wasi(module, proc);

        // 7. LibC link (non-fatal — modules that don't import _exit etc are fine).
        result = m3_LinkLibC(module);
        if (result) {
            screen_push_linef("WASM: LinkLibC: %s", result);
        }
    }

    // 8. Find _start and call _start(argc, argv_ptr) wasi-style.
    {
        PROFILE_SCOPE("wasm:_start");
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
            serial_outsl("WASM: _start returned");
        }
    }

    serial_outsl("WASM: entering Label_Done");

Label_Done:
    // Free WASM-owned resources. Null each slot after freeing so the
    // process cleanup hook (wasm_proc_cleanup) is a no-op after normal exit.
    if (runtime) { m3_FreeRuntime(runtime);          ra->runtime = null; }
    serial_outsl("WASM: freed runtime");
    if (env)     { m3_FreeEnvironment(env);          ra->env = null; }
    serial_outsl("WASM: freed environment");
    if (wasm_data) { kfree(wasm_data);               ra->wasm_data = null; }
    if (ra->wasm_path_alloc) { kfree(ra->wasm_path_alloc); ra->wasm_path_alloc = null; }
    serial_outsl("WASM: Label_Done complete");
}

// ============================================================================
// Public API: spawn a new process running the given .wasm.
// Caller must own opts and any string data pointed to by opts->argv (it's copied).
// ============================================================================
i32 wasm_spawn(const wasm_spawn_opts_t *opts) {
    PROFILE_SCOPE("wasm:spawn");
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
        if (active_session) active_session->foreground_proc = (void*)proc;
        keyboard_fg_flush();
    }

    // Create the thread: either the custom entry point (doom-style) or the
    // normal WASM loading loop (wasm_thread_entry).
    kern_task_t *task;
    if (opts->thread_entry) {
        task = sched_create_process_thread(proc, opts->thread_entry, opts->custom_arg);
    } else {
        task = sched_create_process_thread(proc, wasm_thread_entry, ra);
    }
    if (!task) {
        screen_push_line("WASM: Failed to create thread");
        if (opts->foreground) {
            foreground_proc = null;
            if (active_session) active_session->foreground_proc = NULL;
        }
        // process_exit will run wasm_proc_cleanup on `ra`, which frees
        // wasm_path_alloc (the only populated field) and the args struct.
        // For thread_entry path, ra was never allocated, so cleanup_fn
        // is NULL and process_exit just frees proc resources.
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
            }
            sched_yield();
        }
    }

    return proc->pid;
}
