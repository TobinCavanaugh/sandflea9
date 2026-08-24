# Layered WASM Import Resolution

**Status:** Design Proposal
**Date:** August 2026
**Author:** Tobin (via Buffy)

---

## 1. The Problem

sandfleaOS needs a clean way for WASM programs to declare what they need and for the kernel to satisfy those needs. Today we have two extremes:

- **Simple programs** (ls, cat, lsr): use only the standard host imports (`fd_read`, `fd_write`, `fd_open`). These go through `wasm_thread_entry` with no custom code. Clean.
- **Complex programs** (Doom): need custom host functions (`drawFrame`, `readWads`, `tickGame`). These require a 500-line native C shim (`wasm_doom_test`), a custom `link_extra` callback, and a custom entry point. Dirty.

There's no middle ground. If we add a second complex program (image viewer, web browser, game engine), we'd duplicate the shim pattern. That doesn't scale.

## 2. The Insight

**WASM already declares its needs.** The import section of a `.wasm` file IS the capability manifest:

```wat
(import "env"      "fd_read"    (func $fd_read    (param i32 i32 i32) (result i32)))
(import "display"  "drawFrame"  (func $drawFrame  (param i32 i32 i32)))
(import "console"  "onInfo"     (func $onInfo     (param i32 i32)))
(import "doom-engine" "tickGame" (func $tickGame  (result i32)))
```

The question isn't "what does this program need?" — the module tells us. The question is: **who satisfies each import, and how?**

Some imports must be kernel-provided (`drawFrame` writes to hardware). Some could be provided by other WASM modules (`doom-engine`). Some should be overridable per-program (different versions of an engine). A flat registry can't capture this.

## 3. The Three-Layer Model

```
                    ┌─────────────────────────────────────┐
                    │   Per-Program Config (Perm File)     │  Layer 3
                    │   "resolve 'doom-engine' → v1.wasm"  │  Highest priority
                    │   also: capability grants            │
                    └──────────────┬──────────────────────┘
                                   │ overrides
                    ┌──────────────▼──────────────────────┐
                    │   System-Wide Symlink Dir            │  Layer 2
                    │   /bin/doom-engine → v2.wasm         │  Default resolution
                    │   /bin/sh → /wasm/sh.wasm            │
                    └──────────────┬──────────────────────┘
                                   │ fallback
                    ┌──────────────▼──────────────────────┐
                    │   Kernel Host Function Table         │  Layer 1
                    │   "fd_read" → &kern_fd_read          │  Non-negotiable
                    │   "drawFrame" → &display_draw_frame  │  Hardware access
                    │   "ipc_signal_send" → &ipc_send      │
                    └─────────────────────────────────────┘
```

### Layer 1: Kernel Host Functions (Hard Registry)

Functions that cannot live in userland because they access hardware or kernel state. This is a fixed table:

```c
typedef struct {
    const char *module;   // "env", "wasi_snapshot_preview1", "display", etc.
    const char *name;     // "fd_read", "drawFrame", etc.
    const char *sig;      // "i(iii)", "v(i)", etc.
    M3RawCall   fn;       // the actual C function pointer
    u32         flags;    // capability bits for permission checking
} host_fn_entry_t;

static host_fn_entry_t g_host_fn_table[] = {
    // Standard — all programs get these (subject to perm file)
    { "env",     "fd_read",   "i(iii)",  &wasm_fd_read,        CAP_STDIO     },
    { "env",     "fd_write",  "i(iiii)", &wasm_fd_write,       CAP_STDIO     },
    { "env",     "fd_open",   "i(i)",    &wasm_fd_open,        CAP_FILESYS   },
    { "env",     "fd_close",  "i(i)",    &wasm_fd_close,       CAP_FILESYS   },

    // Display — only programs with CAP_DISPLAY
    { "display", "getResolution", "i()",     &wasm_display_get_resolution, CAP_DISPLAY },
    { "display", "drawFrame",     "i(iii)",  &wasm_display_draw_frame,     CAP_DISPLAY },
    { "display", "present",       "v()",     &wasm_display_present,        CAP_DISPLAY },

    // IPC
    { "env",     "ipc_setup_wait",  "i(ii)", &wasm_ipc_setup_wait,  CAP_IPC },
    { "env",     "ipc_signal_send", "i(ii)", &wasm_ipc_signal_send, CAP_IPC },
    { "env",     "ipc_signal_wait", "i(i)",  &wasm_ipc_signal_wait, CAP_IPC },

    // WASI
    { "wasi_snapshot_preview1", "fd_read",  "i(iiii)", &kern_wasi_fd_read,  CAP_FILESYS },
    { "wasi_snapshot_preview1", "fd_write", "i(iiii)", &kern_wasi_fd_write, CAP_FILESYS },
    // ... etc
};
```

**Rule:** If a module asks for a function in this table and has the right capabilities (per perm file), it gets linked. If it asks for something NOT in this table, the kernel doesn't panic — it falls through to Layer 2.

### Layer 2: Symlink Directory (System-Wide Default Resolution)

When a module imports from module `"doom-engine"` and the kernel doesn't recognize that module name, the resolver checks the symlink dir:

```
/bin/doom-engine → /wasm/doom-engine-v2.wasm
/bin/zlib        → /wasm/zlib.wasm
/bin/regex       → /wasm/regex.wasm
```

**Rule:** `(import "doom-engine" "tickGame" ...)` → resolve `doom-engine` via `/bin/doom-engine` → load `doom-engine-v2.wasm` → link `tickGame` from that module's exports.

This is the default. It's what most programs use. It's what the system maintainer sets up at boot via `build.sh`:

```sh
ln -s /wasm/doom-engine-v2.wasm /bin/doom-engine
```

The resolver in `wasm_spawn`:

```c
IM3Module resolve_wasm_dependency(const char *module_name) {
    // Layer 2: check symlink dir
    char target[256];
    if (cmd_dir_lookup(module_name, target, sizeof(target))) {
        // Found a symlink → load the WASM → return the parsed module
        return load_wasm_module(target);
    }
    return NULL;
}

void link_module_imports(IM3Module module, IM3Runtime runtime,
                         kern_process_t *proc, const perm_t *perms) {
    for each import in module:
        // Layer 1: kernel host functions
        host_fn_entry_t *entry = lookup_host_fn(import.module, import.name);
        if (entry) {
            if (!perms->capabilities & entry->flags)
                return error("permission denied: %s.%s", import.module, import.name);
            m3_LinkRawFunction(module, entry->module, entry->name, entry->sig, entry->fn);
            continue;
        }

        // Layer 2: check per-program overrides → symlink dir
        IM3Module dep = NULL;
        if (perms->overrides[import.module]) {
            dep = load_wasm_module(perms->overrides[import.module]);
        } else {
            dep = resolve_wasm_dependency(import.module);
        }
        if (dep) {
            // Link the WASM-to-WASM import
            m3_LinkWasmFunction(module, import.module, import.name, dep, import.name);
            continue;
        }

        // Unresolved import — fatal
        return error("unresolved import: %s.%s", import.module, import.name);
}
```

### Layer 3: Per-Program Override (Perm File)

A sidecar file at `/perms/bin/doom.wasm:0xHHHH` that can override Layer 2 resolution:

```
# doom.wasm permission file
capabilities: display, filesys, audio

# Override default dependency resolution:
override doom-engine = /wasm/doom-engine-v1.wasm
override zlib        = /wasm/zlib-legacy.wasm
```

This is how a user says "Doom specifically should use engine v1, not the system default v2." Without this file, the symlink dir at `/bin/` provides the default.

**Security:** The perm file is named with the WASM's content hash (128-bit SHA-256 prefix). If an attacker replaces `doom.wasm` with malicious code, the hash changes, the perm file doesn't match, and the program fails to load. The wildcard hash `0x0000...` is allowed during development but rejected in production builds.

## 4. End-to-End Example: Doom

```
User types:  doom
  │
  ├─ 1. Shell resolves command
  │     /bin/doom → /wasm/doom.wasm
  │
  ├─ 2. wasm_spawn loads doom.wasm
  │     parses import section:
  │       (import "display" "drawFrame" ...)     ← Layer 1 hit
  │       (import "console" "onErrorMessage" ...) ← Layer 1 hit
  │       (import "doom-engine" "tickGame" ...)   ← Layer 1 miss
  │       (import "doom-engine" "initGame" ...)   ← Layer 1 miss
  │
  ├─ 3. Resolve each import:
  │     "display.drawFrame"     → g_host_fn_table → &wasm_display_draw_frame ✓
  │     "console.onErrorMessage" → g_host_fn_table → &doom_onError ✓
  │     "doom-engine.tickGame"  → Layer 1 miss
  │       → check perms: override? no
  │       → Layer 2: /bin/doom-engine → /wasm/doom-engine-v2.wasm → load it
  │       → link tickGame, initGame from engine's exports ✓
  │
  ├─ 4. Call _start()
  │
  └─ Done. No custom entry point. No link_extra. No 500-line shim.
```

## 5. Comparison: Today vs. Proposed

| Aspect | Today | Proposed |
|--------|-------|----------|
| **Doom host functions** | 500-line C shim in kern_tests.c | 3 rows in g_host_fn_table |
| **Doom entry point** | Custom `wasm_doom_test()` | Standard `wasm_thread_entry` |
| **Doom command dispatch** | `cmd_word_eq(word, "doom")` with custom opts | `cmd_dir_lookup("doom")` → symlink → spawn |
| **Dependency resolution** | Hardcoded in Doom's C shim | Automatic via Layer 2 symlink dir |
| **Version pinning** | Rebuild kernel | Edit perm file |
| **New complex app** | Write another 500-line shim | Add rows to host fn table + symlink in /bin/ |
| **Capability enforcement** | None (trust) | Perm file + capability flags |
| **WASM→WASM linkage** | Not supported | Layer 2 resolves module names to WASM files |

## 6. Why This Avoids DLL Hell

Native DLLs fragment because they couple at the ABI level:

- **Symbol collisions:** Two DLLs export `malloc` → linker picks one arbitrarily
- **ABI mismatches:** GCC vs MSVC calling conventions, struct layouts
- **Version hell:** `libfoo.so.1` vs `libfoo.so.2` with incompatible APIs
- **Global state:** One DLL's static variables are visible to all consumers

WASM imports avoid all of these:

- **No symbol collisions:** Imports are `(module, name, signature)` — fully qualified by module prefix
- **No ABI mismatch:** Signature is part of the import — `(param i32)` vs `(param f64)` is unambiguous
- **Versioning by naming convention:** `doom-engine` vs `doom-engine-v1` vs `doom-engine-v2` — each is a distinct module name, resolved independently
- **No global state:** Each WASM instance has its own linear memory, own function table, own globals. No shared heap.

The symlink approach means "doom-engine" resolves to whatever the system maintainer linked. A program that wants the legacy version overrides it in its perm file. Both can coexist — different programs use different versions of the same engine — because each loads a separate WASM module instance with separate state.

## 7. Capability Flags

Each host function carries a capability bit. The perm file grants a subset:

```c
#define CAP_STDIO    (1 << 0)  // fd_read, fd_write
#define CAP_FILESYS  (1 << 1)  // fd_open, fd_close, fs_read, fs_write
#define CAP_DISPLAY  (1 << 2)  // getResolution, drawFrame, present
#define CAP_IPC      (1 << 3)  // ipc_signal_send, ipc_signal_wait
#define CAP_AUDIO    (1 << 4)  // (future)
#define CAP_NET      (1 << 5)  // (future)
#define CAP_ALL      0xFFFFFFFF
```

A simple `cat.wasm` gets `CAP_STDIO | CAP_FILESYS`. Doom gets those plus `CAP_DISPLAY`. A calculator gets only `CAP_STDIO`. The perm file is the gate.

## 8. Migration Path

### Phase 1: Host Function Registry (replaces link_extra)
1. Build the `g_host_fn_table` with all current imports
2. Modify `wasm_thread_entry` to iterate module imports and link from the table
3. Remove `link_extra` callback — all imports go through the table
4. Doom still works (its imports are in the table)

### Phase 2: Symlink-Based Command Dispatch (command_dispatch_and_symlinks.md)
1. Move `ls`, `cat`, `lsr` etc. to `/bin/` symlinks
2. `handle_command` checks builtins → symlink dir → spawn
3. Shrinks `kern_tests.c` from ~600 lines of dispatch to ~50

### Phase 3: Per-Program Perm Files
1. Implement perm file reader
2. `wasm_spawn` checks perm file before linking
3. Capability flags enforced

### Phase 4: WASM→WASM Dependency Resolution
1. Layer 2 symlink resolution for module imports
2. `wasm_spawn` recursively loads dependencies
3. Doom's engine dependency resolved automatically

### Phase 5: Per-Program Overrides
1. Perm file `override` directives
2. Program pins specific dependency versions
3. Multiple versions of the same engine coexist

## 9. Summary

| Layer | What | Resolution | Overridable? | Examples |
|-------|------|-----------|-------------|----------|
| **3** | Per-program perm file | `override doom-engine = /wasm/v1.wasm` | — | Legacy version pinning |
| **2** | System symlink dir | `/bin/doom-engine → /wasm/v2.wasm` | By Layer 3 | System defaults |
| **1** | Kernel host function table | `g_host_fn_table[]` lookup | No — only kernel can add | `fd_read`, `drawFrame`, IPC |

The import section of a `.wasm` file IS the capability manifest. The three layers answer "who satisfies this import?" in priority order: per-program config, system defaults, kernel built-ins. No DLLs, no ABI fragmentation, no version hell — just `(module, name, signature)` resolved through symlinks.