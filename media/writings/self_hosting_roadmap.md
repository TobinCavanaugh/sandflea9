# Self-Hosting Roadmap

## Goal

Build sandfleaOS on sandfleaOS — all user-space programs run as WASM executables. No ELF loading for userspace. The kernel is the only ELF binary.

## Design Philosophy: WASM-First

sandfleaOS's unique angle: **everything userspace is WASM**. The kernel speaks WASM, not ELF. This is truly novel — most OSes are Linux-compatible or POSIX-compatible, not "run your programs as WebAssembly."

This means:
- No ELF loader needed in the kernel
- All programs (editor, compiler, assembler, shell) are `.wasm` files
- The kernel only needs to support WASM3 + a rich set of host functions
- Security model is built-in (WASM modules can't jump to arbitrary memory)

## Key Finding: WABT Can Compile to WASM

WABT (WebAssembly Binary Toolkit) has an active build pipeline that produces **WASM binaries** of all its tools:

**wabt.js** (https://github.com/AssemblyScript/wabt.js) publishes:
- `wat2wasm` — WAT → WASM binary
- `wasm2wat` — WASM binary → WAT text
- `wasm2c` — WASM binary → C source
- `wasm-objdump`, `wasm-validate`, `wasm-strip`, `wasm-stats`, `wasm-interp`

These are full WASM builds of WABT that run in Node.js. They use Emscripten's WASM syscall interface, which means:
- They expect POSIX file I/O (fopen/fread/fwrite)
- They expect standard streams (stdin/stdout/stderr)
- They expect command-line arguments via `__wasm_args_get`

**This is the WASI (WebAssembly System Interface) ABI.** If our kernel speaks WASI, we can run WABT tools natively.

## The Key Enabler: WASI Support in the Kernel

We already have most of the core WASI syscalls. WASI defines ~40 syscalls; we need about 10 of them:

| WASI Syscall | sandfleaOS Status |
|-------------|-------------------|
| `fd_read` | ✅ Already implemented (`wasm_fd_read`) |
| `fd_write` | ✅ Already implemented (`wasm_fd_write`) |
| `fd_close` | ✅ Already implemented (`wasm_fd_close`) |
| `fd_open` (WASI `path_open`) | ✅ Already implemented (`wasm_fd_open`) |
| `fd_seek` | ✅ Kernel has `fs_seek()` but no WASM host function yet |
| `fd_fdstat_get` | ❌ Needed — returns file descriptor type (dir/file/char) |
| `fd_prestat_get` | ❌ Needed — returns pre-opened directory info |
| `fd_prestat_dir_name` | ❌ Needed — returns pre-opened directory path |
| `args_sizes_get` | ⚠️ Have `get_arg_count`, need WASI-compatible wrapper |
| `args_get` | ⚠️ Have `get_arg`, need WASI-compatible wrapper |
| `environ_sizes_get` | ❌ Needed — returns environment variable sizes |
| `environ_get` | ❌ Needed — returns environment variables |
| `proc_exit` | ❌ Needed — terminates the WASM process cleanly |
| `clock_time_get` | ❌ Needed — returns current time for timestamps |

**Total: ~8 new host functions to add**, each one small (~10-30 lines).

WASI modules import from `"wasi_snapshot_preview1"` namespace (not `"env"`). The linker would need:

```c
// Link WASI imports if the module uses them
m3_LinkRawFunction(module, "wasi_snapshot_preview1", "fd_read",  "i(i)",    &wasm_fd_read);
m3_LinkRawFunction(module, "wasi_snapshot_preview1", "fd_write", "i(i)",    &wasm_fd_write);
m3_LinkRawFunction(module, "wasi_snapshot_preview1", "args_get", "i(i)",    &wasm_args_get);
// ... etc
```

Once WASI is supported, **any WASI-compiled program works on sandfleaOS**:
- C programs compiled with `wasm32-wasi` target via wasi-sdk
- Rust programs compiled with `wasm32-wasi` target
- WABT tools compiled to WASM via Emscripten with WASI backend
- Python WASM builds (wasmtime, etc.)

## The Self-Hosting Build Pipeline (WASM-First)

Since we're going WASM-first, the build pipeline doesn't need ELF tools on the OS:

```
sandfleaOS running on hardware/QEMU
│
├── Step 1: Edit source files
│   └── WASM program: editor.wasm
│       Reads/writes files via fd_open/fd_read/fd_write
│
├── Step 2: Compile .wat → .wasm (for the kernel's WASM modules)
│   └── WASM program: wat2wasm.wasm (from WABT, WASI-compiled)
│       Input:  src/wasm/wat/*.wat
│       Output: obj/wasm/*.wasm
│
├── Step 3: Compile kernel C → .wasm (alternative to ELF approach)
│   └── WASM program: wasi-sdk's clang.wasm (or custom)
│       Compiles kernel C sources to WASM that runs on...
│       Wait, the kernel needs to be native x86_64 code.
│       So the kernel must still be compiled to ELF by the HOST.
│
├── But the KERNEL is ELF!
│   The kernel itself boots on bare metal — it must be native x86_64 code.
│   You can't run a WASM kernel (no WASM runtime available at boot).
│
└── So self-hosting means:
    1. Compile kernel C → x86_64 ELF ← this step stays on the host forever
    2. Compile user-space tools → WASM ← this happens ON sandfleaOS
    3. The compiler that does step 2 runs AS WASM on sandfleaOS
```

**The kernel stays as ELF.** That's fine — it's the bootloader's job to load it. The flex is that **every program you interact with** (editor, shell, file manager, games) is WASM.

## Revised Toolchain: What Runs Where

| Tool | Host (Linux/WSL) | sandfleaOS (WASM) |
|------|-----------------|-------------------|
| GCC (kernel → ELF) | ✅ Forever | ❌ Not needed |
| NASM (kernel .asm → .o) | ✅ Forever | ❌ Not needed |
| LD (kernel link) | ✅ Forever | ❌ Not needed |
| **Text Editor** | — | ✅ **editor.wasm** |
| **wat2wasm** (.wat → .wasm) | — | ✅ **wat2wasm.wasm** (WASI-compiled) |
| **C→WASM compiler** (.c → .wasm) | — | ✅ **clang.wasm** (wasi-sdk) or **zig.wasm** |
| **Build orchestrator** | — | ✅ **make.wasm** or shell.wasm |
| **Disk image creator** | — | ✅ **mkimg.wasm** |
| **File manager** | — | ✅ optional WASM tool |

## Current WASM Host Function Interface

Our existing host functions let WAT programs do real work:

```wat
;; Available imports (our "env" namespace):
(import "env" "fd_open"       (func $open (param i32) (result i32)))
(import "env" "fd_read"       (func $read (param i32 i32 i32) (result i32)))
(import "env" "fd_write"      (func $write (param i32 i32 i32 i32) (result i32)))
(import "env" "fd_close"      (func $close (param i32) (result i32)))
(import "env" "lsr"           (func $lsr (param i32) (result i32)))
(import "env" "get_arg_count" (func $get_arg_count (result i32)))
(import "env" "get_arg"       (func $get_arg (param i32 i32 i32) (result i32)))

;; What we need to add for WASI compatibility ("wasi_snapshot_preview1"):
(import "wasi_snapshot_preview1" "fd_seek"     (func ...))
(import "wasi_snapshot_preview1" "fd_fdstat_get" (func ...))
(import "wasi_snapshot_preview1" "fd_prestat_get" (func ...))
(import "wasi_snapshot_preview1" "fd_prestat_dir_name" (func ...))
(import "wasi_snapshot_preview1" "environ_sizes_get" (func ...))
(import "wasi_snapshot_preview1" "environ_get" (func ...))
(import "wasi_snapshot_preview1" "proc_exit"    (func ...))
(import "wasi_snapshot_preview1" "clock_time_get" (func ...))
```

## Implementation Roadmap (WASM-First)

### Phase 0: Foundation (Week 1-2)
- [ ] Testing framework (proves things work)
- [ ] WASM binary cache + shared environment (tools load fast)
- [ ] Fix WASM process cleanup (tools run repeatedly without leaking)

### Phase 1: WASI Support (Week 2-3)
- [ ] Add `fd_seek` WASM host function (kernel has it, just needs wrapping)
- [ ] Add `fd_fdstat_get` (returns file type: directory, character device, regular file)
- [ ] Add `fd_prestat_get` + `fd_prestat_dir_name` (needed for WASI libc startup)
- [ ] Add `environ_sizes_get` + `environ_get` (return empty for now)
- [ ] Add `proc_exit` (calls `sched_thread_exit` cleanly)
- [ ] Add `clock_time_get` (reads `sw` timer, converts to nanoseconds)
- [ ] Link WASI imports alongside existing `"env"` imports
- [ ] Test: `wat2wasm --version` on sandfleaOS

### Phase 2: WABT Toolchain (Week 3)
- [ ] Get `wat2wasm.wasm` running on sandfleaOS (via WASI)
- [ ] Compile a `.wat` file → `.wasm` → run it → verify it works
- [ ] Get `wasm-validate.wasm` running (validate self-produced WASM modules)
- [ ] Write a `Makefile.wasm` equivalent (simple build script program)

### Phase 3: Text Editor (Week 3-4)
- [ ] Write minimal terminal editor in WAT or C (compiled to WASI)
- [ ] Features: cursor movement, insert/delete, save, quit, line numbers
- [ ] Wire editor into the shell as `edit file.txt`

### Phase 4: Self-Editing Loop (Week 4)
- [ ] Edit a `.wat` source file on sandfleaOS
- [ ] Compile it to `.wasm` using `wat2wasm.wasm` on sandfleaOS
- [ ] Run the newly compiled program
- [ ] **Flex achieved**: Full edit → compile → run cycle on the OS itself

### Phase 5: Full Toolchain (Future)
- [ ] C→WASM compiler (wasi-sdk's clang compiled to WASM, or Zig)
- [ ] Write kernel modules in C, compile to WASM on sandfleaOS
- [ ] Package manager for WASM programs
- [ ] Build the entire disk image from within sandfleaOS

## The "Biggest Flex" Checklist (WASM-First)

```
[ ] Write a text file on sandfleaOS          ← editor.wasm
[ ] Edit a .wat source file                  ← editor.wasm
[ ] Compile .wat → .wasm                      ← wat2wasm.wasm
[ ] Run the newly compiled .wasm              ← wasm_spawn
[ ] WASM program can edit WASM program       ← recursion!
[ ] Compile a C file → .wasm                  ← clang.wasm (wasi-sdk)
[ ] Edit, compile, and run a tool entirely   ← the full loop!
    on sandfleaOS without leaving the OS
[ ] Build the disk image from within          ← mkimg.wasm
```

## Quick Wins (Week 1)

| Task | Effort | Impact |
|------|--------|--------|
| Add WASI `fd_seek` host function | 20 lines | Unlocks most WASI programs |
| Add WASI `proc_exit` host function | 10 lines | WASM programs can exit cleanly |
| Add WASI `fd_fdstat_get` | 15 lines | Tells the program "this fd is a file/char device" |
| Link WASI imports in wasm_spawn.c | 10 lines | WASI programs start working |
| Test: `wat2wasm --help` on sandfleaOS | 1 hour | Instant validation |

## Summary

| Aspect | ELF-First Approach | WASM-First Approach (Chosen) |
|--------|-------------------|---------------------------|
| Userspace programs | ELF binaries | WASM modules |
| Kernel loading | ELF loader in kernel | WASM3 (already implemented) |
| C compilation on OS | TCC compiled to WASM → x86_64 ELF | wasi-sdk compiled to WASM → .wasm |
| Assembly on OS | NASM compiled to WASM → .o | WAT is the assembly; wat2wasm → .wasm |
| PR angle | "OS that can compile itself" | "OS that runs everything as WebAssembly" |
| Novelty | Many self-hosting OSes exist | **Genuinely unique** |
| Complexity | Medium (TCC port, ELF linking) | Low (WASI is simple, already have most of it) |

**Verdict**: WASM-first is the right call. Much lower complexity to get running, and the "everything runs as WASM" angle is actually novel — no other OS has made this the primary execution model.
