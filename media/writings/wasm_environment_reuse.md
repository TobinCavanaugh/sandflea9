# WASM Environment Reuse & Pooling

## Problem Statement

Loading a WASM program in sandfleaOS has two issues:
1. **Performance**: Every spawn does file I/O, parsing, compilation, linking — expensive
2. **Run-twice reliability**: Some WASM programs run once but fail on the second spawn due to resource leaks or stale global state

This document analyzes what wasm3 state can be pooled/reused and designs a pre-warmed environment pool.

---

## 1. wasm3 API Lifecycle

### Current Spawn Flow (wasm_spawn.c)

```
m3_NewEnvironment()        → IM3Environment    (1× per spawn)
m3_NewRuntime(env, ...)    → IM3Runtime        (1× per spawn)
m3_ParseModule(env, ...)   → IM3Module         (1× per spawn - expensive)
m3_LoadModule(runtime, ...)                    (1× per spawn - consumes module)
m3_LinkRawFunction(...)                        (1× per spawn)
m3_LinkLibC(...)                               (1× per spawn)
m3_FindFunction(&func, ...)                    (1× per spawn)
m3_Call(func, ...)                             (execute)
m3_FreeRuntime(runtime)                        (1× per spawn)
m3_FreeEnvironment(env)                        (1× per spawn)
kfree(wasm_data)                               (1× per spawn)
```

### Cost Breakdown

| Step | Cost | Can Be Cached? |
|------|------|----------------|
| File I/O (fs_open + fs_read) | **High** — disk read, ~1-5ms | ✅ Cache the raw WASM bytes in memory |
| `m3_NewEnvironment()` | **Low** — allocates a small struct (~48 bytes), no I/O | ✅ Create once, share forever |
| `m3_ParseModule()` | **High** — parses WASM binary format, creates function types, data segments, globals tables | ⚠️ Can cache the raw bytes, but must re-parse (module is consumed by LoadModule) |
| `m3_NewRuntime()` | **Medium** — allocates WASM stack + memory, both zeroed | ✅ Pool N pre-allocated runtimes |
| `m3_LoadModule()` | **High** — compiles WASM bytecode to m3 metacode, sets up memory, links imports | ❌ Consumes the module (sets `module->runtime`), can't re-load into another runtime |
| `m3_LinkRawFunction()` | **Low** — string matching | ✅ Always the same set of functions |
| `m3_Call()` | Runtime | The actual execution |
| Cleanup (free + kfree) | **Medium** — frees memory, but fragmentation accumulates | ❌ Pooling avoids alloc/free churn |

---

## 2. Internal State Structure

### 2.1 `M3Environment` — Safe to Share

From `m3_env.h`:
```c
typedef struct M3Environment {
    IM3FuncType   funcTypes;            // linked list of unique function type signatures
    IM3FuncType   retFuncTypes[];       // cached return types for basic types
    M3CodePage *  pagesReleased;        // freed code pages (reusable!)
};
```

The environment is a **pure cache**. It holds:
- A deduplicated set of function type signatures (strings like `"i(i)"`, `"v(ii)"`)
- A pool of freed code pages that can be reused across modules

**Sharing:** One `IM3Environment` can serve an unlimited number of module loads and runtimes. It has no per-runtime or per-module state.

### 2.2 `M3Runtime` — Per-Spawn, But Poolable

```c
typedef struct M3Runtime {
    M3Compilation  compilation;         // mutable compilation state
    IM3Environment environment;         // back-reference
    M3CodePage *   pagesOpen;           // code pages with writable space
    M3CodePage *   pagesFull;           // full code pages
    u32            numCodePages;
    IM3Module      modules;             // linked list of loaded modules ← KEY STATE
    void *         stack;               // WASM stack (allocated, must be released)
    u32            stackSize;
    M3Memory       memory;              // WASM linear memory ← STALE STATE LIVES HERE
    u32            memoryLimit;
    void *         userdata;
    M3ErrorInfo    error;
};
```

The runtime holds:
- **WASM stack** — a fixed-size allocation (e.g., 64KB). Zeroed on creation. After a module runs, the stack is no longer needed but is still allocated.
- **WASM linear memory** — grows as the program calls `memory.grow`. After a module finishes, it holds stale data.
- **Compiled code pages** — sequences of m3 metacode. Freed by `m3_FreeRuntime`.
- **Module list** — linked list of loaded modules. Must be emptied between runs.

### 2.3 `M3Module` — Single-Use (Consumed by Load)

```c
typedef struct M3Module {
    struct M3Runtime *      runtime;     // ← SET by m3_LoadModule, checked for already-linked
    struct M3Environment *  environment;
    bytes_t                 wasmStart;   // ← references the raw WASM bytes (must persist!)
    bytes_t                 wasmEnd;
    M3Function *            functions;   // parsed function table
    M3DataSegment *         dataSegments;
    M3Global *              globals;
    M3MemoryInfo            memoryInfo;  // initial memory size
    struct M3Module *       next;
};
```

Key constraint from wasm3.h:
```
// LoadModule transfers ownership of a module to the runtime.
// Do not free modules once successfully loaded into the runtime.
```

And the error constant:
```c
moduleAlreadyLinked = "attempting to bind module to multiple runtimes"
```

**A parsed module can ONLY be loaded into ONE runtime.** If you want to run the same WASM twice, you must:
- Re-parse it (expensive but correct), OR
- Deep-copy the parsed module's functions/data/code into a fresh module struct (complex, fragile)

---

## 3. Pooling Architecture

### 3.1 Three Layers of Reuse

```
┌────────────────────────────────────────────────────────────┐
│                    WASM SPAWN REQUEST                       │
└────────────────────┬───────────────────────────────────────┘
                     │
                     ▼
┌────────────────────────────────────────────────────────────┐
│  Layer 1: Shared Environment (created once, lives forever) │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  env = m3_NewEnvironment()                           │  │
│  │  // ↑ called once at boot, never freed               │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────┬───────────────────────────────────────┘
                     │
                     ▼
┌────────────────────────────────────────────────────────────┐
│  Layer 2: WASM Binary Cache (avoids repeated file I/O)    │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  cache[hash].data = kmalloc(size) + fs_read(...)     │  │
│  │  // ↑ cached until cache eviction / process exit     │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────┬───────────────────────────────────────┘
                     │
                     ▼
┌────────────────────────────────────────────────────────────┐
│  Layer 3: Runtime Pool (avoids alloc/free churn)          │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  pool[0] = m3_NewRuntime(env, 64KB, NULL)           │  │
│  │  pool[1] = m3_NewRuntime(env, 64KB, NULL)           │  │
│  │  pool[2] = m3_NewRuntime(env, 64KB, NULL)           │  │
│  │  pool[3] = m3_NewRuntime(env, 64KB, NULL)           │  │
│  │  // ↑ pre-warmed at boot, recycled after each spawn │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────┬───────────────────────────────────────┘
                     │
                     ▼
             Parse + Load + Run
             (still per-spawn, but all supporting
              infrastructure is pooled)
```

### 3.2 Shared Environment

```c
// At boot (or first spawn):
static IM3Environment g_wasm_env = NULL;

IM3Environment wasm_get_global_env(void) {
    if (!g_wasm_env) {
        g_wasm_env = m3_NewEnvironment();
    }
    return g_wasm_env;
}
```

**Memory impact:** ~48 bytes. Free.

### 3.3 WASM Binary Cache

```c
#define WASM_CACHE_SIZE 16

typedef struct {
    char    path[64];       // e.g. "add_test.wasm"
    u64     mtime;          // filesystem timestamp for invalidation
    u8     *data;
    u32     size;
    u32     refcount;       // how many in-flight spawns use this
    bool    in_use;
} wasm_cache_entry_t;

static wasm_cache_entry_t g_wasm_cache[WASM_CACHE_SIZE];

// Returns a pointer to cached data (or loads it on cache miss)
u8 *wasm_cache_load(const char *path, u32 *out_size) {
    // 1. Hash path → index
    // 2. Check if cached and not stale
    // 3. Cache hit → return data
    // 4. Cache miss → fs_open + fs_read + kmalloc → store → return
}

void wasm_cache_evict(const char *path) {
    // Free the cached data for a given path
    // Called when tests need to verify file I/O works
}
```

**Memory impact:** Each cached WASM module is the raw `.wasm` file bytes in memory. `add_test.wasm` is tiny (~50 bytes), `file_test.wasm` is small (~200 bytes), `lsr.wasm` is medium (~1KB), `doom-v0.1.0.wasm` is large (~1MB). The cache should only cache small modules; leave it to the OS page cache for large ones.

### 3.4 Runtime Pool

```c
#define RUNTIME_POOL_SIZE 4

typedef struct {
    IM3Runtime  runtime;
    bool        in_use;     // is this runtime currently running a WASM?
    u32         stack_kb;   // the stack size it was created with
} pooled_runtime_t;

static pooled_runtime_t g_runtime_pool[RUNTIME_POOL_SIZE];
static u32 g_next_runtime = 0;  // round-robin allocator

// Initialize the pool at boot
void wasm_pool_init(u32 stack_kb) {
    IM3Environment env = wasm_get_global_env();
    for (u32 i = 0; i < RUNTIME_POOL_SIZE; i++) {
        g_runtime_pool[i].runtime = m3_NewRuntime(env, stack_kb * 1024, NULL);
        g_runtime_pool[i].in_use  = false;
        g_runtime_pool[i].stack_kb = stack_kb;
    }
}

// Acquire a runtime from the pool
IM3Runtime wasm_pool_acquire(void) {
    for (u32 i = 0; i < RUNTIME_POOL_SIZE; i++) {
        u32 idx = (g_next_runtime + i) % RUNTIME_POOL_SIZE;
        if (!g_runtime_pool[idx].in_use) {
            g_runtime_pool[idx].in_use = true;
            g_next_runtime = (idx + 1) % RUNTIME_POOL_SIZE;
            return g_runtime_pool[idx].runtime;
        }
    }
    return NULL;  // all in use — caller falls back to creating a fresh one
}

// Release a runtime back to the pool
void wasm_pool_release(IM3Runtime rt) {
    // 1. Unload all modules from the runtime
    //    (walk the modules list and reset the module->runtime pointer)
    // 2. Reset WASM memory to initial size (or free and reallocate)
    // 3. Reset the stack (or just zero the stack pointer)
    // 4. Mark as available

    for (u32 i = 0; i < RUNTIME_POOL_SIZE; i++) {
        if (g_runtime_pool[i].runtime == rt) {
            wasm_reset_runtime(rt);
            g_runtime_pool[i].in_use = false;
            return;
        }
    }
    // Not from pool — free it normally
    m3_FreeRuntime(rt);
}
```

### 3.5 Reset Runtime — The Hard Part

The runtime accumulates state between runs. To reuse it, we need to reset:

```c
void wasm_reset_runtime(IM3Runtime rt) {
    // 1. Unload all modules
    // Each module's data segments have been written to memory.
    // The module struct itself is owned by the runtime.
    // We need to walk the module list and free each one.
    IM3Module mod = rt->modules;
    while (mod) {
        IM3Module next = mod->next;
        mod->runtime = NULL;  // disassociate from runtime
        m3_FreeModule(mod);   // free the module (only safe after LoadModule
                              // failure, or if we reverse the ownership)
        mod = next;
    }
    rt->modules = NULL;

    // 2. Free compiled code pages
    // The code pages contain compiled m3 metacode for the module.
    // Release them back to the environment's free list.
    M3CodePage *cp = rt->pagesOpen;
    while (cp) {
        M3CodePage *next = cp->next;
        ReleaseCodePage(rt, cp);  // internal wasm3 function
        cp = next;
    }
    rt->pagesOpen = NULL;

    cp = rt->pagesFull;
    while (cp) {
        M3CodePage *next = cp->next;
        ReleaseCodePage(rt, cp);
        cp = next;
    }
    rt->pagesFull = NULL;

    // 3. Reset WASM linear memory
    // The WASM memory may have grown via memory.grow.
    // We need to either:
    //   a) Free and re-allocate memory (simple but loses the pages)
    //   b) Zero the existing memory and reset to initial pages
    rt->memory.numPages = 0;
    // Re-allocate with 1 initial page (64KB)
    ResizeMemory(rt, 1);
    // Zero the whole page
    mem_set(rt->memory.mallocated->pages, 0, 65536);

    // 4. Reset the stack pointer
    // The stack is just a big allocation. Zero it.
    mem_set(rt->stack, 0, rt->stackSize);
    rt->numStackSlots = rt->stackSize / 8;

    // 5. Reset compilation state
    mem_set(&rt->compilation, 0, sizeof(rt->compilation));

    // 6. Reset error info
    rt->error.result = NULL;
    rt->error.runtime = NULL;
    rt->error.module = NULL;
    rt->error.function = NULL;
    rt->error.file = NULL;
    rt->error.line = 0;
    rt->error.message = NULL;
}
```

**Problem with `m3_FreeModule`**: The wasm3 API says *"Do not free modules once successfully loaded into the runtime."* This means `m3_FreeModule` may not work correctly for a module that was successfully loaded. We'd need to understand wasm3's internal memory management or use a different approach.

**Alternative**: Instead of trying to reset a runtime, we could:
1. **Never pool runtimes** — just pool the environment (easy) and the WASM binary cache (also easy)
2. **Re-parse the WASM binary each time** — parsing is the expensive step, but if we have the binary cached in memory, it's still just `m3_ParseModule(env, &module, cached_data, cached_size)` — no file I/O
3. **Create a new runtime each time** — `m3_NewRuntime` allocates stack + memory. This is `kmalloc` + `mem_set`, which is fast (~50µs)

The real question is: **what actually leaks between runs?**

---

## 4. What Actually Causes the "Runs Once, Not Twice" Bug?

There are three distinct failure modes:

### 4.1 Kernel-Side Resource Leak

**Symptom**: The WASM thread exits, but `process_exit` doesn't free something.

**Root cause**: A file handle, a kmalloc buffer, or a kernel struct is left dangling.

**Detection**: After the process exits, `test_get_process_count()` should return to baseline. If it doesn't, a process struct wasn't freed. `test_get_heap_used()` should be the same before and after.

**Fix**: Debug the process reaping code in `kern_sched.c`. Currently:

```c
// wasm_proc_cleanup — frees env, runtime, wasm_data, path, args struct
// Called from process_exit() when the thread is marked DEAD.

// Checklist for cleanup:
// 1. ✓ m3_FreeRuntime → frees runtime, stack, memory, code pages
// 2. ✓ m3_FreeEnvironment → if we own it (but with pooling we don't!)
// 3. ✓ kfree(wasm_data) → frees the raw WASM bytes
// 4. ✓ kfree(wasm_path_alloc) → frees the path string
// 5. ✓ kfree(ra) → frees the args struct itself
// 6. ? fs_close(fd) → closes any open file descriptor in the process table
// 7. ? kfree(proc->argv[i]) → frees argv strings (done in process_exit)
// 8. ? vmm_unmap / pmm_free → frees PML4 pages (done in process_exit)
```

**With pooling**, step 1 and 2 change:
- ❌ Don't call `m3_FreeRuntime` — return it to the pool instead
- ❌ Don't call `m3_FreeEnvironment` — it's shared
- Need to add: reset runtime for reuse (see §3.5)

### 4.2 WASM Module Internal State

**Symptom**: The WASM module uses global variables that aren't reset between runs.

**Root cause**: WASM globals are initialized when the module is loaded. If the module modified its globals during execution, they don't get reset when the runtime is freed.

**Example**:
```wasm
(module
    (global $counter (mut i32) (i32.const 0))
    (func $start (export "_start")
        global.get $counter
        i32.const 1
        i32.add
        global.set $counter
        ;; ... each run increments counter ...
    )
)
```

First run: counter = 1. Second run (with new runtime): counter = 1 again (reset by module init).

**With runtime pooling**, this is a problem. The module's initial values are loaded from the parsed module's globals table. If we reuse the runtime without reloading the module, the globals from the previous run persist.

**Fix**: Always reload the module into the runtime (even a pooled one). The module init process writes the initial global values into the runtime's memory.

### 4.3 WASM Linear Memory Not Zeroed

**Symptom**: The second run sees stale data from the first run.

**Root cause**: WASM linear memory is allocated once. After the first run, it contains whatever the program wrote. If the second run's module is loaded but the memory isn't zeroed, the second run reads garbage.

**Fix**: Reset the memory to zero between runs (or re-allocate fresh).

---

## 5. Recommended Pooling Design

### 5.1 What to Pool (Safe, High-Value)

| Resource | Pool Strategy | Impact |
|----------|--------------|--------|
| `IM3Environment` | **Single shared instance**, created once | Saves ~48 bytes of alloc/free |
| WASM binary data | **In-memory cache** keyed by path + mtime | Saves file I/O (biggest win) |
| `IM3Runtime` | **Pool of N**, recycled with full reset | Saves stack alloc + zero (~64KB/frame) |

### 5.2 What NOT to Pool (Unsafe or Low-Value)

| Resource | Why Not |
|----------|---------|
| Parsed `IM3Module` | Consumed by `m3_LoadModule`, can't re-load into another runtime |
| Linked host functions | Must be re-linked each time (module's function table is internal) |
| WASM stack contents | Must be zeroed between runs (done by m3_NewRuntime) |

### 5.3 The Modified Spawn Flow (With Pooling)

```
BEFORE (cold):
  1. m3_NewEnvironment()        → 48 bytes alloc
  2. m3_NewRuntime(env, 64KB)   → 64KB alloc + zero
  3. fs_open(path) + fs_read    → disk I/O
  4. m3_ParseModule(env, ...)   → parse WASM binary
  5. m3_LoadModule(runtime, ...)→ compile to metacode
  6. m3_LinkRawFunction(...)    → link host imports
  7. m3_LinkLibC(...)           → link libc imports
  8. m3_Call(...)               → execute
  9. m3_FreeRuntime(runtime)    → 64KB free
  10. m3_FreeEnvironment(env)   → 48 bytes free
  11. kfree(wasm_data)          → binary bytes free

AFTER (with pooling):
  1. env = wasm_get_shared_env()          → pointer copy (no alloc)
  2. rt = wasm_pool_acquire()             → already allocated
     wasm_reset_runtime(rt)               → zero memory + unload modules
  3. data = wasm_cache_load(path)         → pointer if cached, I/O on miss
  4. m3_ParseModule(env, &mod, data, sz)  → parse (still needed)
  5. m3_LoadModule(rt, mod)               → compile (still needed)
  6. m3_LinkRawFunction(...)              → link (still needed)
  7. m3_LinkLibC(...)                     → link (still needed)
  8. m3_Call(...)                         → execute
  9. wasm_pool_release(rt)                → reset internal state, mark available
  10. wasm_cache_release(path)            → decrement refcount
```

**Hot path savings:**
- Environment alloc/free: **eliminated** (permanent)
- Runtime alloc/free: **eliminated** (pooled, just reset)
- File I/O: **eliminated on cache hit** (biggest win)
- Memory fragmentation: **reduced** (pool avoids alloc/free churn)

**Still per-spawn:**
- Module parsing + compilation (wasm3 internal, unavoidable)
- Host function linking (per-module state)

### 5.4 Reset Runtime — Practical Implementation

Since `m3_FreeModule` explicitly says not to call it after successful `m3_LoadModule`, the practical reset approach is:

```c
void wasm_reset_runtime(IM3Runtime rt) {
    // wasm3's internal: M3Module is a linked list in M3Runtime
    // After m3_LoadModule succeeds, the runtime owns the module.
    // There is no public API to unload a module.
    //
    // Strategy: Don't reuse runtimes. Instead, pool the environment
    // and binary cache, but create a fresh runtime each time.
    // m3_NewRuntime is cheap (~50µs for the alloc + zero).
    //
    // This avoids the module ownership problem entirely.
}
```

**Verdict**: Runtime pooling is high-risk due to wasm3's module ownership model. The safer, still-performant approach is:

| Layer | Pool? | Why |
|-------|-------|-----|
| Environment | ✅ Yes | Stateless, wasm3 confirms it's shareable |
| WASM binary | ✅ Yes | Cached bytes avoid file I/O |
| Runtime | ❌ No | Module ownership model makes safe reset difficult |

### 5.5 Revised Pooling (Practical)

```c
// ┌─────────────────────────────────────────────────┐
// │ shared_env (created once, lives forever)        │
// ├─────────────────────────────────────────────────┤
// │ wasm_cache[16] (keyed by path)                 │
// │   ┌──────────┐  ┌──────────┐  ┌──────────┐     │
// │   │add_test  │  │file_test │  │lsr.wasm  │     │
// │   │  .wasm   │  │  .wasm   │  │          │     │
// │   └──────────┘  └──────────┘  └──────────┘     │
// ├─────────────────────────────────────────────────┤
// │ Each spawn:                                     │
// │   rt = m3_NewRuntime(shared_env, 64KB, NULL)    │
// │   mod = ParseModule(shared_env, cached_bytes)   │
// │   LoadModule(rt, mod)                           │
// │   Link + Call                                   │
// │   m3_FreeRuntime(rt)                            │
// └─────────────────────────────────────────────────┘
```

This gives us:
- **~1 alloc/free saved** (environment is permanent)
- **~0 file I/O** on cache hit (all cache-resident WASM modules)
- **No module ownership issues** (fresh runtime each time)

---

## 6. Caching Parsed Modules (Boot-Time Pre-Parse)

### 6.1 The Problem

Even with the binary cache (raw .wasm bytes in RAM), `m3_ParseModule` must iterate every byte of the WASM binary each time:
- It reads the magic + version
- It iterates all sections (Type, Import, Function, Memory, Global, Export, Start, Element, Code, Data)
- For each section, it allocates arrays and fills structs

The `M3Module` struct it produces is **heavily dependent on the raw WASM bytes** — many fields (`wasmStart`, `wasmEnd`, `elementSection`, function `wasm` pointers, data segment `data` pointers, global `initExpr` pointers) reference locations within the loaded binary.

### 6.2 Why a Single Parsed Module Can't Be Reused

`m3_LoadModule` sets `module->runtime` and checks for `moduleAlreadyLinked`:

```c
// m3_env.h
typedef struct M3Module {
    struct M3Runtime *      runtime;     // ← SET by m3_LoadModule
    // ... internal structs with pointers into raw .wasm bytes ...
};
```

```c
// wasm3.h — error constant
moduleAlreadyLinked = "attempting to bind module to multiple runtimes"
```

A parsed module can only be loaded into **one** runtime. After that, the runtime owns it.

### 6.3 Solution: Parse Once, Clone + Load

The approach: 
1. **At boot**, pre-parse common WASM files into `M3Module` structs (stored in a cache keyed by path)
2. **On each spawn**, deep-copy the cached module into a fresh `M3Module` clone
3. **Load the clone** into the new runtime
4. **Clone is freed** with the runtime (standard flow)
5. **Original cached module** stays alive for future spawns

### 6.4 Module Cloning Logic

```c
// Clone a parsed M3Module so it can be loaded into a fresh runtime.
// Returns a deep copy with runtime = NULL.
IM3Module wasm_clone_module(IM3Module src, u8 *wasm_copy) {
    // 1. Clone the base struct
    IM3Module dst = kmalloc(sizeof(M3Module));
    mem_copy((u8*)dst, (u8*)src, sizeof(M3Module));
    
    // 2. Fix up pointers that reference the raw WASM bytes
    //    src->wasmStart  is an absolute ptr into the original cached bytes
    //    dst->wasmStart  should point into wasm_copy
    u64 wasm_offset = (u64)src->wasmStart;
    
    // Helper: relocate a pointer from original memory to new memory
    #define RELOC(ptr)  ((void*)(((u8*)ptr) - wasm_offset + (u8*)wasm_copy))
    
    dst->wasmStart        = RELOC(src->wasmStart);
    dst->wasmEnd          = RELOC(src->wasmEnd);
    dst->elementSection   = RELOC(src->elementSection);
    dst->elementSectionEnd = RELOC(src->elementSectionEnd);
    
    // 3. Deep-copy the functions array
    //    Each M3Function has .wasm, .wasmEnd → into WASM bytes
    dst->functions = kmalloc(src->numFunctions * sizeof(M3Function));
    mem_copy((u8*)dst->functions, (u8*)src->functions,
             src->numFunctions * sizeof(M3Function));
    for (u32 i = 0; i < src->numFunctions; i++) {
        dst->functions[i].wasm    = RELOC(src->functions[i].wasm);
        dst->functions[i].wasmEnd = RELOC(src->functions[i].wasmEnd);
    }
    
    // 4. Deep-copy data segments
    dst->dataSegments = kmalloc(src->numDataSegments * sizeof(M3DataSegment));
    mem_copy((u8*)dst->dataSegments, (u8*)src->dataSegments,
             src->numDataSegments * sizeof(M3DataSegment));
    for (u32 i = 0; i < src->numDataSegments; i++) {
        dst->dataSegments[i].initExpr = RELOC(src->dataSegments[i].initExpr);
        dst->dataSegments[i].data     = RELOC(src->dataSegments[i].data);
    }
    
    // 5. funcTypes are pointers into the environment's linked list.
    //    These are SHARED across all clones — just copy the pointer array.
    dst->funcTypes = kmalloc(src->numFuncTypes * sizeof(IM3FuncType));
    mem_copy((u8*)dst->funcTypes, (u8*)src->funcTypes,
             src->numFuncTypes * sizeof(IM3FuncType));
    
    // 6. Deep-copy globals (each has initExpr → WASM bytes, and name strings)
    dst->globals = kmalloc(src->numGlobals * sizeof(M3Global));
    mem_copy((u8*)dst->globals, (u8*)src->globals,
             src->numGlobals * sizeof(M3Global));
    for (u32 i = 0; i < src->numGlobals; i++) {
        dst->globals[i].initExpr = RELOC(src->globals[i].initExpr);
        // name strings live in the parsed module — clone them too
        if (src->globals[i].name) {
            u32 len = str_len(src->globals[i].name);
            dst->globals[i].name = kmalloc(len + 1);
            mem_copy((u8*)dst->globals[i].name,
                     (u8*)src->globals[i].name, len + 1);
        }
    }
    
    // 7. Clear runtime pointer — this is the KEY fix
    dst->runtime = NULL;
    
    #undef RELOC
    return dst;
}
```

### 6.5 Boot-Time Pre-Parse Cache

```c
#define MAX_CACHED_MODULES 16

typedef struct {
    char      path[64];          // e.g. "lsr.wasm"
    IM3Module module;            // parsed module (the MASTER copy)
    u8       *wasm_bytes;        // raw WASM bytes (must outlive the module!)
    u32       size;
} parsed_module_cache_t;

static parsed_module_cache_t g_module_cache[MAX_CACHED_MODULES];
static u32 g_module_cache_count = 0;

// Pre-parse a WASM file and add to cache
IM3Module wasm_cache_parse(const char *path) {
    // 1. Read file into memory
    i32 fd = fs_open(path);
    u32 size = fs_size(fd);
    u8 *bytes = kmalloc(size);
    fs_read(fd, bytes, size);
    fs_close(fd);
    
    // 2. Parse
    IM3Module module = NULL;
    M3Result r = m3_ParseModule(wasm_get_env(), &module, bytes, size);
    if (r || !module) {
        kfree(bytes);
        return NULL;
    }
    
    // 3. Cache
    u32 idx = g_module_cache_count++;
    str_copy(g_module_cache[idx].path, path, 63);
    g_module_cache[idx].module     = module;
    g_module_cache[idx].wasm_bytes = bytes;
    g_module_cache[idx].size       = size;
    
    serial_outsf("WASM: Pre-parsed %s (%d bytes, %d funcs, %d globals)\n",
                 path, size, module->numFunctions, module->numGlobals);
    return module;
}

// Retrieve a clone of a cached module for a fresh runtime
// Returns NULL on cache miss (caller falls back to normal path)
IM3Module wasm_cache_get_clone(const char *path) {
    for (u32 i = 0; i < g_module_cache_count; i++) {
        if (str_eql(g_module_cache[i].path, path, 63)) {
            return wasm_clone_module(
                g_module_cache[i].module,
                g_module_cache[i].wasm_bytes
            );
        }
    }
    return NULL;
}
```

### 6.6 What Gets Pre-Parsed (Boot-Time)

In the test-mode boot path (`main.c` with `TEST_MODE`), after the filesystem is initialized:

```c
#ifdef ENABLE_WASM_CACHE
    // Pre-parse system WASM modules for fast spawn
    wasm_cache_parse("lsr.wasm");
    wasm_cache_parse("cat.wasm");
    wasm_cache_parse("add_test.wasm");
    wasm_cache_parse("file_test.wasm");
#endif
```

### 6.7 On-Disk Serialization (Future)

For **across-boot** caching, the parsed module can be serialized to a `.wpc` (Wasm Pre-parse Cache) file:

```
sandfleaOS disk image:
  /boot/lsr.wasm          ← original .wasm
  /boot/lsr.wasm.wpc      ← pre-parsed cache (generated at build time)
  /boot/cat.wasm
  /boot/cat.wasm.wpc
```

The `.wpc` format converts all pointer fields to offsets (relative to the WASM binary start):

```c
// Serialized format (per module):
struct wpc_header {
    u32 magic;          // "WPC1"
    u32 version;        // 1
    u32 wasm_hash;      // djb2 hash of original .wasm (for validation)
    u32 num_functions;
    u32 num_datasegments;
    u32 num_globals;
    u32 num_functypes;
    u32 element_offset; // offset to element section in .wasm bytes
    u32 element_size;
    // followed by: arrays with offsets instead of pointers
};
```

Then at boot:
1. Check for `lsr.wasm.wpc`
2. Read the header
3. Hash the actual `lsr.wasm` and compare against `wasm_hash`
4. If match: deserialize the wpc file → produce an `IM3Module` without parsing
5. If mismatch (WASM was rebuilt): fall back to normal parsing, update the wpc

**Why this is deferred:**
- Requires a custom serializer + deserializer that stays in sync with wasm3's internal struct layout
- The deserializer must re-register funcTypes with the environment (they use pointer-equality dedup)
- For the small system modules, boot-time parsing is fast enough (< 1ms each)
- Doom is loaded once and lives for the session — no repeated spawns

### 6.8 The Full Caching Hierarchy

```
┌────────────────────────────────────────────────────────────┐
│                    WASM SPAWN REQUEST                       │
└────────────────────────┬───────────────────────────────────┘
                         │
                         ▼
  ┌──────────────────────────────────────┐
  │  Tier 1: Pre-Parsed Module Cache    │
  │  (boot-time, RAM, path-keyed)       │  ← HIT: clone + skip parse
  │  lsr.wasm → [M3Module]              │
  │  cat.wasm → [M3Module]              │
  └──────────────┬───────────────────────┘
                 │ MISS
                 ▼
  ┌──────────────────────────────────────┐
  │  Tier 2: WASM Binary Cache          │
  │  (on-demand, RAM, path+mtime)       │  ← HIT: re-parse from RAM
  │  myapp.wasm → [raw bytes]            │
  └──────────────┬───────────────────────┘
                 │ MISS
                 ▼
  ┌──────────────────────────────────────┐
  │  Tier 3: File I/O                   │
  │  fs_open + fs_read → cache          │  ← SLOW: disk read
  └──────────────────────────────────────┘
```

**Hottest path** (system modules like lsr): Tier 1 → clone → load → run → free clone
**Warm path** (custom modules run twice): Tier 2 → parse → load → run → free module
**Cold path** (first ever load): Tier 3 → file I/O → parse → load → run → cache

---

## 7. Memory Impact

| Resource | Without Pooling | With Pooling |
|----------|----------------|--------------|
| Environment | Freed after each spawn | **~48 bytes, permanent** |
| Runtime | Freed after each spawn | Pool creates **N × 64KB** |
| WASM binary cache | Freed after each spawn | **Sum of cached WASM sizes** |
| Pre-parsed modules | Not cached | **Sum of M3Module sizes (~200 bytes each for small WASMs)** |
| **Total** | ~64KB per spawn (temporary) | **~256KB (pool) + ~1.5KB (small WASMs) + ~1MB (Doom)** |

For the default pool of 4, with 64KB stacks: **256KB of permanently reserved runtime memory**. This is negligible on a system with 2GB of RAM.

---

## 8. Next Steps: What to Actually Implement

### Phase 1: Shared Environment (Easy, Safe)
```c
// wasm_spawn.c — change from per-spawn to shared
static IM3Environment g_wasm_env = NULL;

IM3Environment wasm_get_env(void) {
    if (!g_wasm_env)
        g_wasm_env = m3_NewEnvironment();
    return g_wasm_env;
}
```

### Phase 2: WASM Binary Cache (Medium, Safe)
```c
// wasm_spawn.c — add a simple LRU cache
// Keyed by path string, invalidated by filesystem timestamp
// Max 16 entries, LRU eviction
```

### Phase 3: Module Cloning (Medium, Safe)
```c
// wasm_spawn.c — implement wasm_clone_module()
// Boot-time pre-parse of system modules
// Cache lookup → clone → load into fresh runtime
```

### Phase 4: Introspection Hooks (Tests)
```c
// Needed to verify clean lifecycle:
u64 wasm_get_env_alloc_count(void);   // how many environments exist
u64 wasm_cache_hit_count(void);       // cache effectiveness
u64 wasm_cache_miss_count(void);
```

### Phase 5: Runtime Pool (Complex, Optional)
```c
// Only after confirming the module ownership model can be worked around.
// Would need a wasm3 patch to add a "reset runtime" API.
```

---

## 9. Summary

| Layer | Cache | What It Saves | Complexity | Risk |
|-------|-------|--------------|------------|------|
| Environment | Shared singleton | ~48 bytes alloc/free | Trivial | None |
| WASM binary cache | Sparse, path-keyed | File I/O per spawn | Low | Low |
| Pre-parsed module cache | Boot-time, clone per spawn | `m3_ParseModule` | Medium | Low (clone handles pointer fixup) |
| On-disk serialization | Build-time `.wpc` file | `m3_ParseModule` across boots | High | High (wasm3 struct compatibility) |
| Runtime pool | N pre-warmed | `m3_NewRuntime` alloc/free | High | High (module ownership) |

**Recommendation**: Implement Phases 1 → 3 now. Phase 5 only if profiling shows `m3_NewRuntime` as a bottleneck. Phase 4 (on-disk) deferred until needed.

