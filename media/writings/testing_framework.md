# sandfleaOS Testing Framework Design

## Overview

This document specifies the testing architecture for sandfleaOS. The design addresses the unique challenges of bare-metal kernel testing: no OS underneath, only serial/display IO, QEMU emulation, and the need to test hardware-near code without corrupting the test environment.

---

## 1. Project Structure

```
src/
├── kernel/            # Production kernel — never modified by tests
├── include/           # Shared headers (some test-visible, some not)
│   ├── kern_test.h    # NEW: test runner API header
│   └── ...
└── tests/             # NEW: test modules (compiled only in test builds)
    ├── test_runner.c      # Runner: linker-section iteration, result reporting
    ├── test_util_str.c    # String utility tests (pure C, no hardware deps)
    ├── test_kern_mem.c    # mem_copy / mem_set / mem_move tests (pure C)
    ├── test_kmalloc.c     # Heap allocator: allocate, write patterns, free, stress
    ├── test_ext2.c        # Filesystem: open known files, verify contents
    ├── test_fs.c          # VFS: open/read/seek/tell/close across file types
    ├── test_sched.c       # Scheduler: spawn threads, verify lifecycle
    ├── test_wasm.c        # WASM: load a simple wasm, verify it runs to completion
    ├── test_display.c     # GUI: draw patterns, output pixel data for comparison
    └── test_keyboard.c    # Input: accept serial-fed keystrokes, verify routing

media/
└── writings/
    └── testing_framework.md   # ← This document

test_runner.py           # NEW: host-side Python test orchestrator
golden/                  # NEW: golden reference images for screenshot comparison
└── all_red.ppm
```

---

## 2. Test Runner API (C Side)

### 2.1 Header: `include/kern_test.h`

```c
#ifndef KERN_TEST_H
#define KERN_TEST_H

#include "dialect.h"

// ======================================================================
// Test Registration — Linker-Section Based
// ======================================================================
// Usage: TEST("kmalloc", basic_alloc) { ... body ... }
// The macro creates a function and a struct in .test_registry section.
// The linker gathers all entries into a contiguous array.
// ======================================================================

typedef void (*test_fn_t)(void);

typedef struct {
    const char *suite;     // e.g. "kmalloc"
    const char *name;      // e.g. "basic_alloc_free"
    test_fn_t   fn;
} test_entry_t;

#define TEST(suite, name) \
    static void test_impl_##name(void); \
    __attribute__((used, section(".test_registry"))) \
    static const test_entry_t __test_##name = { \
        .suite = #suite, \
        .name  = #name, \
        .fn    = test_impl_##name \
    }; \
    static void test_impl_##name(void)

// ======================================================================
// Assertion Macros
// ======================================================================
// TEST_ASSERT: fatal — prints failure and returns from the test
// TEST_CHECK:  non-fatal — reports failure but continues (for loop checks)
// All output goes to serial as structured [TEST] lines.
// ======================================================================

extern u32  test_current_failures;
extern char *test_current_suite;
extern char *test_current_name;

// Fatal assertion — aborts the current test on failure
#define TEST_ASSERT(cond, fmt, ...) \
    do { \
        if (!(cond)) { \
            serial_outsf("[TEST] FAIL %s:%s:%d: " fmt "\n", \
                         test_current_suite, test_current_name, \
                         __LINE__, ##__VA_ARGS__); \
            test_current_failures++; \
            return; \
        } \
    } while(0)

// Non-fatal check — reports failure but test continues
#define TEST_CHECK(cond, fmt, ...) \
    do { \
        if (!(cond)) { \
            serial_outsf("[TEST] CHECK_FAIL %s:%s:%d: " fmt "\n", \
                         test_current_suite, test_current_name, \
                         __LINE__, ##__VA_ARGS__); \
            test_current_failures++; \
        } \
    } while(0)

// Convenience wrappers
#define TEST_ASSERT_NOT_NULL(ptr) \
    TEST_ASSERT((ptr) != NULL, "expected non-null")

#define TEST_ASSERT_NULL(ptr) \
    TEST_ASSERT((ptr) == NULL, "expected null")

#define TEST_ASSERT_EQ(a, b) \
    TEST_ASSERT((a) == (b), "expected %lld == %lld", (i64)(a), (i64)(b))

#define TEST_ASSERT_MEM_EQ(ptr1, ptr2, len) \
    TEST_ASSERT(mem_compare((u8*)(ptr1), (u8*)(ptr2), (len)) == 0, \
                "memory mismatch at %s:%d", __FILE__, __LINE__)

// Manual pass/fail (for complex control flow)
#define TEST_PASS() \
    do { \
        serial_outsf("[TEST] PASS %s:%s\n", \
                     test_current_suite, test_current_name); \
        return; \
    } while(0)

#define TEST_FAIL(fmt, ...) \
    do { \
        serial_outsf("[TEST] FAIL %s:%s:%d: " fmt "\n", \
                     test_current_suite, test_current_name, \
                     __LINE__, ##__VA_ARGS__); \
        test_current_failures++; \
        return; \
    } while(0)

// ======================================================================
// Introspection Primitives — for verifying kernel state from tests
// ======================================================================
// These query kernel-internal state so tests can assert on process
// counts, memory usage, file handle leaks, etc.
// Only available in TEST_MODE builds.
// ======================================================================

// Number of active processes (including the test process itself)
u32 test_get_process_count(void);

// Is a specific PID still alive? Returns true if process exists.
bool test_process_exists(i32 pid);

// Free physical memory in bytes (useful for leak detection)
u64 test_get_free_mem(void);

// Bytes currently allocated via kmalloc (tracked by a counter)
u64 test_get_heap_used(void);

// Number of open file handles across all processes
u32 test_get_total_open_fds(void);

// Count of registered kernel threads (for scheduler tests)
u32 test_get_thread_count(void);

// ======================================================================
// Runner API
// ======================================================================

// Run all registered tests. Returns total failures.
u32 test_run_all(void);

// Run a single suite by name. Returns failures in that suite.
u32 test_run_suite(const char *suite_name);

// Run a specific test by fully qualified name "suite:name".
u32 test_run_single(const char *full_name);

// ======================================================================
// Panic/Timeout Handling
// ======================================================================

// Called by exception handlers (page fault, GPF, etc.) when running in
// test mode. Prints structured panic info, then halts.
// The host test runner detects [TEST] PANIC in the serial output.
void test_panic(const char *reason, u64 rip, u64 cr2);

// Called by the watchdog timer if a test takes too long.
// Must be registered before running tests.
void test_timeout_handler(void);

// Register a watchdog timer that fires after 'ms' milliseconds.
// If the timer fires, it prints [TEST] TIMEOUT and halts.
void test_arm_watchdog(u64 ms);

// Disarm the watchdog (called after each test completes).
void test_disarm_watchdog(void);

#endif // KERN_TEST_H
```

### 2.2 Output Format: NDJSON (Newline-Delimited JSON)

Every test runner output line is a complete JSON object. This is machine-parseable by default (just `json.loads()` in Python), extensible (add new fields without breaking parsers), and can carry rich data like stack traces.

#### Protocol

```ndjson
{"t":0,"event":"ready"}
{"t":2,"event":"suite_start","name":"util_str"}
{"t":2,"event":"test_start","suite":"util_str","name":"str_len"}
{"t":3,"event":"test_pass","suite":"util_str","name":"str_len","dur_ms":1}
{"t":3,"event":"test_start","suite":"util_str","name":"str_eql"}
{"t":5,"event":"test_pass","suite":"util_str","name":"str_eql","dur_ms":2}
{"t":7,"event":"suite_end","name":"util_str","passed":2,"failed":0}
{"t":10,"event":"suite_start","name":"kmalloc"}
{"t":10,"event":"test_start","suite":"kmalloc","name":"basic_alloc_free"}
{"t":12,"event":"test_pass","suite":"kmalloc","name":"basic_alloc_free","dur_ms":2}
{"t":14,"event":"test_start","suite":"kmalloc","name":"stress"}
{"t":16,"event":"test_fail","suite":"kmalloc","name":"stress","file":"test_kmalloc.c","line":42,"msg":"kmalloc returned NULL"}
{"t":19,"event":"suite_end","name":"kmalloc","passed":1,"failed":1}
{"t":22,"event":"panic","reason":"Page Fault","rip":"0xFFFFFFFF80001234","cr2":"0x0","frames":["0xFFFF8000123456","0xFFFF8000123789","0xFFFF8000123ABC"]}
{"t":847,"event":"done","passed":17,"failed":2,"panicked":1,"dur_ms":847}
```

#### Event Types

| Event | Fields | Description |
|-------|--------|-------------|
| `ready` | `t` | Boot complete, about to run tests |
| `suite_start` | `t`, `name` | Beginning a test suite |
| `suite_end` | `t`, `name`, `passed`, `failed` | Suite complete with counts |
| `test_start` | `t`, `suite`, `name` | Beginning a single test |
| `test_pass` | `t`, `suite`, `name`, `dur_ms` | Test passed |
| `test_fail` | `t`, `suite`, `name`, `file`, `line`, `msg` | Test failed with details |
| `test_skip` | `t`, `suite`, `name`, `reason` | Test skipped |
| `panic` | `t`, `reason`, `rip`, `cr2`, `frames` | Unhandled exception with stack trace |
| `timeout` | `t` | Watchdog fired, test took too long |
| `screenshot` | `t`, `id` | Signal host to capture screenshot N |
| `info` | `t`, `msg` | Informational message (not an error) |
| `done` | `t`, `passed`, `failed`, `panicked`, `dur_ms` | All tests complete, summary |
| `ready_for_input` | `t` | Kernel waiting for serial input from host |

All events include `t` — the value of the `sw` timer (in milliseconds). This allows reconstructing timing even if lines arrive late.

#### JSON Escaping (C Side)

Strings need minimal escaping — filenames, suite names, and messages don't usually contain control characters. A simple helper:

```c
// Print a JSON-escaped string to serial
static void test_json_str(const char *s) {
    serial_outc('"');
    while (*s) {
        if (*s == '"' || *s == '\\') {
            serial_outc('\\'); serial_outc(*s);
        } else if (*s == '\n') {
            serial_outc('\\'); serial_outc('n');
        } else if (*s == '\t') {
            serial_outc('\\'); serial_outc('t');
        } else {
            serial_outc(*s);
        }
        s++;
    }
    serial_outc('"');
}
```

#### Host-Side Parsing (Python)

```python
import json

for line in serial_output:
    obj = json.loads(line)    # ← no regex, no ambiguity
    
    if obj["event"] == "test_pass":
        passed += 1
    elif obj["event"] == "test_fail":
        failed += 1
        failures.append(f"{obj['suite']}:{obj['name']}: {obj['msg']}")
    elif obj["event"] == "panic":
        panicked += 1
        # Resolve stack frames via addr2line
        frames = resolve_stack(obj["frames"])
```

#### Why NDJSON Over `[TEST]` Markers

| Aspect | `[TEST]` markers | NDJSON |
|--------|-----------------|--------|
| Human-readable raw output | ✅ Great | ⚠️ Decent with short keys |
| Machine-parseable | ⚠️ Regex needed | ✅ `json.loads()` |
| Extensible (add fields) | ⚠️ Fragile regex | ✅ New JSON keys, no breakage |
| Stack traces | ❌ Awkward line-wrapping | ✅ Natural `frames: [...]` |
| Register dumps in panics | ❌ Multiple lines, fragile | ✅ Single object, all fields |
| Kernel code complexity | ✅ `serial_outs("...")` | ⚠️ `test_json_str()` helper (~15 lines) |
| Verbosity per event | 30-50 chars | 60-120 chars |

At 115200 baud (~11.5 KB/s), the extra verbosity costs ~2-5ms per 100 events — irrelevant.

---

## 3. Kernel Stack Tracing — Concrete Implementation

### 3.1 Overview

Kernel stack unwinding requires:
1. **Frame pointers** enabled in the test build (`-fno-omit-frame-pointer`)
2. **A stack walker** that follows the RBP chain
3. **Offline symbol resolution** via `addr2line` on the host

No ELF parsing, no DWARF tables, no kernel-side string symbols needed. The kernel prints raw hex RIPs; the host resolves them.

### 3.2 Prerequisite: Frame Pointer Flag

Current production CFLAGS: `-O3 -m64 -c -ffreestanding ...` (`-O3` omits frame pointers by default)

Test mode CFLAGS need one addition:

```bash
# In test mode only:
-fno-omit-frame-pointer
```

Without this, RBP is used as a general-purpose register and the stack chain is garbage. The 1-2% performance cost from keeping RBP as a frame pointer doesn't matter for tests.

### 3.3 How the Stack Unwinding Works

The x86_64 calling convention (with frame pointers) guarantees every function entry saves RBP:

```
Stack layout (growing downward):
                    ┌──────────────────────┐
     Higher addr    │    local vars        │
                    ├──────────────────────┤
                    │    saved RBP ────────────────→ points to caller's saved RBP
                    ├──────────────────────┤
                    │    return RIP        │ ← faulting code called from here
                    ├──────────────────────┤
                    │    arg 1, 2, ...     │
    Current RBP →   ├──────────────────────┤
                    │    ...               │
                    └──────────────────────┘
     Lower addr
```

The walker follows the chain: `rbp → [saved_rbp] [return_rip] → [saved_rbp2] [return_rip2] → ...`

### 3.4 Files Changed

| File | Change | Lines |
|------|--------|-------|
| `build.sh` | Add `-fno-omit-frame-pointer` to test CFLAGS | 1 |
| `kern_interrupts.c` | Add `#ifdef TEST_MODE` panic handler with stack walk | ~30 |
| `link.ld` | Add `.test_registry` section | 4 |
| `main.c` | Add `#ifdef TEST_MODE` boot path (skip GUI, run tests, halt) | ~15 |
| `include/kern_test.h` | **New:** macros, assertions, runner API, NDJSON helpers | ~100 |
| `tests/test_runner.c` | **New:** linker iteration, NDJSON output, stack walker | ~150 |
| `test_runner.py` | **New:** QEMU orchestrator, addr2line resolution | ~200 |

### 3.5 The Stack Walker (~15 lines)

```c
// Walk the frame pointer chain starting from a given RBP.
static u32 trace_walk(u64 rbp_value, u64 *frames, u32 max_frames) {
    u32 count = 0;
    u64 *rbp = (u64 *)rbp_value;

    for (u32 i = 0; i < max_frames && rbp; i++) {
        // Sanity check: RBP must point to kernel-space memory
        if ((u64)rbp < 0xFFFF800000000000) break;
        if ((u64)rbp >= 0xFFFFFFFFFFFFFFF0) break;

        u64 saved_rip = rbp[1];   // return address at [rbp+8]
        u64 saved_rbp = rbp[0];   // next frame at     [rbp+0]

        // Stop if return address looks invalid
        if (saved_rip < 0xFFFFFFFF80000000) break;

        frames[count++] = saved_rip;
        rbp = (u64 *)saved_rbp;
    }
    return count;
}
```

### 3.6 The Modified Exception Handler (kern_interrupts.c)

This is the only existing kernel file that gets modified. The change is surgical:

```c
// In kern_interrupt_handler(), replace the current panic spin-loop:

#ifdef TEST_MODE
    if (t->int_no <= 31) {
        u64 cr2_val;
        asm volatile("mov %%cr2, %0" : "=r"(cr2_val));

        // Output NDJSON panic event with stack trace
        serial_outs("{\"t\":");
        serial_outi64(sw, BASE_10);
        serial_outs(",\"event\":\"panic\"");
        serial_outs(",\"reason\":\"");
        serial_outs(isr_errors[t->int_no]);
        serial_outsf("\",\"rip\":\"%llX\",\"cr2\":\"%llX\"", t->rip, cr2_val);
        serial_outs(",\"frames\":[");

        // Walk the stack from the faulting context's RBP
        u64 *rbp = (u64 *)t->rbp;
        for (u32 i = 0; i < 32 && rbp && (u64)rbp >= 0xFFFF800000000000; i++) {
            if (i > 0) serial_outs(",");
            serial_outsf("\"%llX\"", rbp[1]);
            rbp = (u64 *)rbp[0];
        }
        serial_outsl("]}");

        // Halt cleanly — host sees the structured panic and kills QEMU
        asm volatile("cli; hlt");
    }
#endif
```

**What changes from current behavior:**
- ❌ `while(1) { toggle_capslock(); }` → ✅ `asm("cli; hlt")` (halts instead of spinning)
- ❌ `panic_draw_status()` needs framebuffer → ✅ serial-only output (works headless)
- ❌ `int3` breakpoint → ✅ clean halt, no debugger needed
- ❌ Plain text dump → ✅ NDJSON with embedded `"frames"` array

### 3.7 Host-Side Symbol Resolution (Python)

```python
import subprocess

def resolve_kernel_stack(kernel_elf: str, hex_frames: list[str]) -> list[str]:
    """
    Convert hex RIPs like '0xFFFF8000123456' to 'function at file:line'.
    Uses addr2line with the kernel's ELF debug info (-g flag in build.sh).
    """
    if not hex_frames:
        return []

    result = subprocess.run(
        ["addr2line", "-e", kernel_elf, "-f", "-p", "-C"] + hex_frames,
        capture_output=True, text=True
    )
    if result.returncode != 0:
        return hex_frames  # fallback to raw addresses

    return [line.strip() for line in result.stdout.strip().split("\n")]

# Example output:
# ['kmalloc at kern_vmm.c:310',
#  'test_kmalloc_stress at test_kmalloc.c:42',
#  'test_run_all at test_runner.c:120',
#  'kern_entry at main.c:85']
```

This works because `build.sh` already passes `-g` (DWARF debug info) for all builds. The `kernel.elf` in `build/test/` contains full line-number tables. `addr2line` reads them — no kernel-side ELF parsing needed.

### 3.8 Developer Experience

When a test panics, the runner prints:

```
═══ PANIC: kmalloc:stress ═══
  Page Fault at RIP=0xFFFFFFFF80001234 (CR2=0x0, Write access)
  
  Stack Trace:
  → kmalloc at kern_vmm.c:310
  → test_kmalloc_stress at test_kmalloc.c:42
  → test_run_all at test_runner.c:120
  → kern_entry at main.c:85
```

### 3.9 Limitations

| Issue | Impact | Mitigation |
|-------|--------|------------|
| Interrupt pushes RIP directly | The faulting function's frame is not on the RBP chain | The `rip` field in the panic event IS the faulting instruction — that's the critical one |
| Tail-call optimization | Callee reuses caller's frame, so caller is missing from the trace | `-fno-optimize-sibling-calls` (optional, adds size) |
| Inline functions | No frame of their own | Acceptable — the non-inlined caller is visible |
| Stack corruption (buffer overflow) | RBP chain leads to garbage | Address range sanity check (`>= 0xFFFF8000...`) stops the walk early |

---

## 4. Build Variants

### 3.1 Build Modes

`build.sh` accepts a mode argument that controls which source files are compiled and what CFLAGS are used:

| Mode | Flag | Sources | Output ISO | Use Case |
|------|------|---------|------------|----------|
| `prod` | (none) | kernel + wasm3 + screen + font | `build/prod/sandfleaOS.iso` | Normal dev boot |
| `test` | `-DTEST_MODE -DNO_GUI` | kernel core + tests (`src/tests/*.c`) | `build/test/sandfleaOS-test.iso` | Automated no-GUI tests |
| `test-gui` | `-DTEST_MODE` | kernel core + screen + tests | `build/test-gui/sandfleaOS-test-gui.iso` | Visual + input tests |
| `test-wasm` | `-DTEST_MODE -DNO_GUI -DWASM_TESTS` | kernel core + wasm3 + tests | `build/test-wasm/sandfleaOS-test-wasm.iso` | WASM integration tests |

### 3.2 Source File Selection by Mode

```
CORE_SOURCES (always compiled):
    kern_asmstubs.c, kern_serial.c, kern_vmm.c, kern_mem.c,
    kern_interrupts.c, kern_sched.c, kern_ext2.c, kern_fs.c,
    kern_ide.c, kern_pci.c, kern_keyboard.c, libgcc_stubs.c,
    main.c, stbsupport.c, x64/idt.c, x64/apic.c
    util_str.c, util_cmd.c

WASM3_SOURCES (optional — compiled only in prod and test-wasm):
    wasm3-0.5.0/source/*.c

EXTRA_SOURCES (mode-dependent):
    prod:  kern_screen.c, ssfn.c, kern_terminal.c, kern_tests.c
    test:  (nothing — NO screen, NO wasm3, NO doom)
    test-gui: kern_screen.c, ssfn.c, kern_terminal.c
    test-wasm: (test sources only, no screen)

TEST_SOURCES (compiled in any test mode):
    test_runner.c, test_util_str.c, test_kern_mem.c,
    test_kmalloc.c, test_ext2.c, test_fs.c, test_sched.c,
    test_wasm.c, test_display.c, test_keyboard.c
```

### 3.3 Build Output Layout

All build artifacts go into mode-specific directories to support simultaneous builds:

```
build/
├── prod/
│   ├── sandfleaOS.iso
│   ├── disk.img
│   ├── kernel.elf
│   └── wasm/               ← compiled .wasm files
├── test/
│   ├── sandfleaOS-test.iso
│   ├── disk-test.img
│   └── kernel.elf
├── test-gui/
│   ├── sandfleaOS-test-gui.iso
│   ├── disk-gui.img
│   └── kernel.elf
└── test-wasm/
    ├── sandfleaOS-test-wasm.iso
    ├── disk-wasm.img
    └── wasm/
```

### 3.4 Linker Section for Test Registry

In `link.ld`, add the `.test_registry` section (zero bytes when no tests are compiled):

```ld
SECTIONS {
    . = 0xffffffff80000000;
    kernel_start_marker = .;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }

    /* NEW: test registration table */
    .test_registry : {
        __start_test_registry = .;
        KEEP(*(.test_registry))
        __stop_test_registry = .;
    }

    .limine_requests : {
        KEEP(*(.limine_requests))
    }
    .bss : { *(.bss .bss.*) *(COMMON) }
    kernel_end_marker = .;
    /DISCARD/ : { *(.eh_frame) *(.note .note*) }
}
```

---

## 4. Boot Path: `main.c` Changes

The entry point gains a `#ifdef TEST_MODE` branch:

```c
void kern_entry(void) {
    // ================================================================
    // Phase 1: Hardware init — shared by all modes
    // ================================================================
    init_serial();
    serial_outsl("--- sandfleaOS Kernel Entry ---");

    enable_sse();
    init_vmm_globals(hhdm_request);
    init_pmm(memmap_request);
    interrupts_init();
    kmalloc_init();
    sched_init();
    system.pci_list_head = pci_init_system();
    ide_init();
    ext2_init();
    fs_init();

#ifdef TEST_MODE
    // ================================================================
    // Phase 2 (test): Skip GUI, run tests, halt
    // ================================================================
    serial_outsl("[TEST] READY");

    // Arm a 30-second watchdog timer:
    // If a test triggers an unhandled page fault and hangs,
    // the watchdog fires, prints [TEST] TIMEOUT, and halts.
    test_arm_watchdog(30000);

    u32 failures = test_run_all();

    serial_outsf("[TEST] DONE: %s\n",
                 failures == 0 ? "ALL PASSED" : "SOME FAILED");

    // Halt — the host has captured all serial output
    serial_outsl("Halting.");
    for(;;) asm("hlt");

#else
    // ================================================================
    // Phase 2 (production): Init GUI, enter shell loop
    // ================================================================

    // Framebuffer init
    display_t *displays = kmalloc(sizeof(display_t) * 32);
    screen_init(framebuffer_request.response, displays, 32);
    display_main = &displays[0];

    // Font loading
    ssfn_src = (ssfn_font_t *) &_binary_src_blob_regularfont_sfn_start;

    // Screen buffer
    screen_lines_init(display_main->surface.width / font_width);

    // Spawn heartbeat threads, register interrupts, enter shell
    sched_create_thread(shimmy, null);
    sched_create_thread(shimmy2, null);
    sched_create_thread(shimmy3, null);
    interrupt_register(32, timer_handler);
    interrupt_register(33, (void (*)(const registers_t *)) keyboard_handle_keypress);

    // Shell loop
    for (;;) {
        // ... keyboard handling, screen_render_shell(), hlt
    }
#endif
}
```

---

## 5. Exception Handling in Test Mode

### 5.1 Problem

Currently, when a page fault or other exception occurs in kernel mode:

```c
// kern_interrupts.c — current behavior
serial_outsf("!!! PAGE FAULT at %llX ...\n", cr2_val, ...);
// ... prints register dump ...
while (1) { toggle_capslock(); }  // ← spins forever, test hangs
```

The QEMU process never exits. The host-side test runner waits forever (timeout).

### 5.2 Solution: Structured Panic + Halt

In test mode, exceptions should:

1. Print a structured `[TEST] PANIC` line with the fault details
2. Halt the CPU cleanly (not spin)

```c
// kern_interrupts.c — TEST_MODE addition
void kern_interrupt_handler(const registers_t *t) {
#ifdef TEST_MODE
    if (t->int_no <= 31) {
        u64 cr2_val;
        asm volatile("mov %%cr2, %0" : "=r"(cr2_val));

        serial_outsf("[TEST] PANIC: %s at RIP=%llX CR2=%llX\n",
                     isr_errors[t->int_no], t->rip, cr2_val);
        serial_outsf("[TEST]   RAX=%llX RBX=%llX RCX=%llX RDX=%llX\n",
                     t->rax, t->rbx, t->rcx, t->rdx);
        serial_outsf("[TEST]   RSP=%llX RBP=%llX ERR=%X\n",
                     t->rsp, t->rbp, t->error_code);

        // Halt — the host has captured everything
        asm volatile("cli; hlt");
    }
#endif
    // ... normal handling if not test mode ...
}
```

The host-side runner detects `[TEST] PANIC` and marks the test as failed.

### 5.3 Watchdog Timer

A test that doesn't panic but also doesn't finish (e.g., waiting forever on a blocking syscall) needs a timeout. The watchdog is a one-shot timer set before each test:

```c
static volatile bool watchdog_fired = false;

void test_arm_watchdog(u64 ms) {
    // Register a one-shot timer via the APIC or PIT
    // When it fires → test_timeout_handler()
    watchdog_fired = false;
    // ... program APIC timer for 'ms' milliseconds ...
}

void test_timeout_handler(void) {
    watchdog_fired = true;
    serial_outsl("[TEST] TIMEOUT: test exceeded time limit");
    serial_outsl("[TEST] DONE: TIMEOUT");
    asm volatile("cli; hlt");
}
```

---

## 6. Host-Side Python Test Runner

### 6.1 Architecture

The Python script (`test_runner.py`) orchestrates the full test lifecycle:

```
┌─────────────────────────────────────────────────────────┐
│                   test_runner.py                        │
│                                                         │
│  ┌─────────┐     ┌──────────┐     ┌───────────────┐   │
│  │  Build   │────▶│  QEMU    │────▶│  Parse        │   │
│  │  ISO     │     │  Execute  │     │  Results      │   │
│  └─────────┘     └──────────┘     └───────────────┘   │
│       │                │                │              │
│       │                │               exit 0/1       │
│       ▼                ▼                              │
│  build.sh        -serial file:log    grep [TEST]      │
│  test-variant    -display none                        │
│                   -qmp for screenshots                 │
└─────────────────────────────────────────────────────────┘
```

### 6.2 Full Script

```python
#!/usr/bin/env python3
"""
sandfleaOS Test Runner

Usage:
    python test_runner.py [--variant test] [--suite kmalloc]
                          [--input "hello"] [--timeout 30]
                          [--record-screenshots] [--golden-dir golden]

Environment:
    PYTHON_QEMU    path to qemu binary (default: qemu-system-x86_64)
    PYTHON_BUILD   path to build script (default: wsl ./build.sh)
"""

import subprocess
import sys
import os
import re
import argparse
import time
import json
from pathlib import Path
from typing import List, Optional

# ====================================================================
# Configuration
# ====================================================================

VARIANTS = {
    "prod": {
        "cflags": "",
        "sources_extra": None,  # use build.sh default
        "iso": "sandfleaOS.iso",
        "display": "sdl",
        "skip_wasm": False,
    },
    "test": {
        "cflags": "-DTEST_MODE -DNO_GUI",
        "sources_extra": "test",  # build.sh knows to add src/tests/
        "iso": "sandfleaOS-test.iso",
        "display": "none",
        "skip_wasm": True,
    },
    "test-gui": {
        "cflags": "-DTEST_MODE",
        "sources_extra": "test-gui",
        "iso": "sandfleaOS-test-gui.iso",
        "display": "gtk",
        "skip_wasm": True,
    },
    "test-wasm": {
        "cflags": "-DTEST_MODE -DNO_GUI -DWASM_TESTS",
        "sources_extra": "test-wasm",
        "iso": "sandfleaOS-test-wasm.iso",
        "display": "none",
        "skip_wasm": False,
    },
}

BUILD_DIR = Path("build")
QEMU_BIN = os.environ.get("PYTHON_QEMU", "qemu-system-x86_64")

QEMU_BASE = [
    "-m", "2G",
    "-machine", "pc",
    "-bios", "ovmf/DEBUGX64_OVMF.fd",
    "-vga", "std",
    "-no-reboot",
    "-accel", "whpx,kernel-irqchip=off",  # Windows Hyper-V; fallback to tcg
]


def build(variant: str) -> bool:
    """Build the specified variant. Returns True on success."""
    info = VARIANTS[variant]
    build_script = os.environ.get("PYTHON_BUILD", "wsl ./build.sh")

    print(f"[BUILD] Building variant '{variant}'...")
    result = subprocess.run(
        build_script.split() + [variant],
        capture_output=True, text=True
    )

    if result.returncode != 0:
        print(f"[BUILD] FAILED:\n{result.stderr}")
        return False

    iso_path = BUILD_DIR / variant / info["iso"]
    if not iso_path.exists():
        print(f"[BUILD] FAILED: {iso_path} not found")
        return False

    print(f"[BUILD] OK: {iso_path}")
    return True


def run_qemu(
    variant: str,
    timeout_secs: int = 30,
    input_data: Optional[str] = None,
    record_screenshots: bool = False,
) -> List[str]:
    """
    Boot QEMU with the variant's ISO. Returns serial output lines.

    If input_data is provided, it is sent to the serial port after
    the kernel outputs [TEST] READY.

    If record_screenshots, a QMP monitor is started and screendump
    is called when the kernel signals a screenshot is ready.
    """
    info = VARIANTS[variant]
    iso_path = BUILD_DIR / variant / info["iso"]
    disk_path = BUILD_DIR / variant / f"disk-{variant.replace('test-', '')}.img"

    # Build QEMU arguments
    qemu_args = [QEMU_BIN] + QEMU_BASE + [
        "-cdrom", str(iso_path),
        "-drive", f"file={disk_path},format=raw,index=0,media=disk",
        "-display", info["display"],
        "-serial", "stdio",        # bidirectional — we can send and receive
    ]

    # If recording screenshots, add QMP monitor
    if record_screenshots:
        qemu_args += ["-qmp", "tcp:localhost:4444,server,nowait"]

    # Start QEMU
    proc = subprocess.Popen(
        qemu_args,
        stdin=subprocess.PIPE if input_data else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    output_lines: List[str] = []
    ready_received = False
    screenshot_count = 0
    start_time = time.time()

    try:
        while True:
            # Timeout check
            elapsed = time.time() - start_time
            if elapsed > timeout_secs:
                print(f"[QEMU] TIMEOUT after {timeout_secs}s")
                break

            # Read a line from serial (non-blocking would be better,
            # but readline with timeout is simpler)
            line = proc.stdout.readline()
            if not line:
                break

            line = line.rstrip()
            output_lines.append(line)
            print(f"  {line}")  # live tail

            # --- State machine ---

            # 1. Ready signal → send input if any
            if "[TEST] READY" in line and input_data and not ready_received:
                ready_received = True
                print(f"[QEMU] Sending input: {input_data!r}")
                proc.stdin.write(input_data + "\n")
                proc.stdin.flush()

            # 2. Screenshot signal → capture via QMP
            if "[TEST] SCREENSHOT" in line and record_screenshots:
                screenshot_count += 1
                screenshot_path = f"screenshot_{screenshot_count}.ppm"
                print(f"[QEMU] Capturing screenshot -> {screenshot_path}")
                # Use QEMU's human monitor to screendump
                hmp_cmd = f"screendump {screenshot_path}\n"
                # Connect to QMP and send HMP command...
                # (simplified: would use a QMP library in practice)

            # 3. Done → exit loop
            if "[TEST] DONE" in line:
                # Brief flush delay
                time.sleep(0.3)
                break

    except Exception as e:
        print(f"[QEMU] Error: {e}")

    finally:
        # Clean up
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except:
            proc.kill()

    return output_lines


def parse_results(lines: List[str]) -> dict:
    """
    Parse [TEST] lines into structured results.

    Returns:
    {
        "passed": int,
        "failed": int,
        "panics": int,
        "timeouts": int,
        "total": int,
        "failures": [str, ...],
        "panics_info": [str, ...],
        "elapsed_ms": int,
        "summary": str,
    }
    """
    results = {
        "passed": 0,
        "failed": 0,
        "panics": 0,
        "timeouts": 0,
        "total": 0,
        "failures": [],
        "panics_info": [],
        "elapsed_ms": 0,
        "summary": "",
    }

    for line in lines:
        if "[TEST] PASS" in line:
            results["passed"] += 1
        elif "[TEST] FAIL" in line:
            results["failed"] += 1
            results["failures"].append(line)
        elif "[TEST] PANIC" in line:
            results["panics"] += 1
            results["panics_info"].append(line)
        elif "[TEST] TIMEOUT" in line:
            results["timeouts"] += 1
        elif "[TEST] DONE" in line:
            results["summary"] = line
        elif "[TEST] TIME:" in line:
            # Extract milliseconds
            m = re.search(r"(\d+)ms", line)
            if m:
                results["elapsed_ms"] = int(m.group(1))

    results["total"] = results["passed"] + results["failed"]
    return results


def compare_screenshot(captured_path: str, golden_path: str,
                       threshold: float = 0.01) -> bool:
    """
    Compare a captured screenshot against a golden reference.
    Returns True if they match within threshold (1% pixel diff).
    """
    try:
        from PIL import Image
        import numpy as np

        captured = Image.open(captured_path).convert("RGB")
        golden = Image.open(golden_path).convert("RGB")

        if captured.size != golden.size:
            print(f"[SCREENSHOT] Size mismatch: {captured.size} vs {golden.size}")
            return False

        arr_cap = np.array(captured, dtype=np.int16)
        arr_gold = np.array(golden, dtype=np.int16)

        diff = np.abs(arr_cap - arr_gold)
        total_pixels = arr_cap.shape[0] * arr_cap.shape[1] * 3
        changed_pixels = np.sum(diff > 10)  # tolerance of 10 per channel
        ratio = changed_pixels / total_pixels

        print(f"[SCREENSHOT] Pixel diff: {ratio:.4%} "
              f"(threshold: {threshold:.2%})")

        return ratio <= threshold

    except ImportError:
        print("[SCREENSHOT] PIL/numpy not installed; falling back to md5")
        import hashlib
        with open(captured_path, "rb") as f:
            h1 = hashlib.md5(f.read()).hexdigest()
        with open(golden_path, "rb") as f:
            h2 = hashlib.md5(f.read()).hexdigest()
        return h1 == h2


def main():
    parser = argparse.ArgumentParser(
        description="sandfleaOS Test Runner"
    )
    parser.add_argument("--variant", default="test",
                        choices=VARIANTS.keys())
    parser.add_argument("--suite", default=None,
                        help="Run only this test suite (e.g. 'kmalloc')")
    parser.add_argument("--input", default=None,
                        help="Input to send after [TEST] READY")
    parser.add_argument("--timeout", type=int, default=30,
                        help="Seconds before QEMU is killed")
    parser.add_argument("--record-screenshots", action="store_true",
                        help="Enable QMP screendump capture")
    parser.add_argument("--golden-dir", default="golden",
                        help="Directory of golden reference images")
    parser.add_argument("--build-only", action="store_true",
                        help="Just build, don't run")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    # 1. Build
    if not build(args.variant):
        sys.exit(1)

    if args.build_only:
        print("Build complete. Skipping run.")
        sys.exit(0)

    # 2. Run
    print(f"\n{'='*60}")
    print(f"Running variant: {args.variant}")
    if args.suite:
        print(f"Suite filter:   {args.suite}")
    print(f"{'='*60}")

    lines = run_qemu(
        variant=args.variant,
        timeout_secs=args.timeout,
        input_data=args.input,
        record_screenshots=args.record_screenshots,
    )

    # 3. Parse
    results = parse_results(lines)

    print(f"\n{'='*60}")
    print(f"RESULTS")
    print(f"{'='*60}")
    print(f"  Passed:  {results['passed']}")
    print(f"  Failed:  {results['failed']}")
    if results['panics'] > 0:
        print(f"  Panics:  {results['panics']}  ← CRASHES")
    if results['timeouts'] > 0:
        print(f"  Timeout: {results['timeouts']}  ← HUNG TESTS")
    print(f"  Total:   {results['total']}")
    print(f"  Time:    {results['elapsed_ms']}ms")

    if results['failures']:
        print(f"\n  Failures:")
        for f in results['failures']:
            print(f"    {f}")

    if results['panics_info']:
        print(f"\n  Panics:")
        for p in results['panics_info']:
            print(f"    {p}")

    # 4. Compare screenshots if recorded
    if args.record_screenshots:
        golden_dir = Path(args.golden_dir)
        if golden_dir.exists():
            for i in range(1, 100):
                cap = Path(f"screenshot_{i}.ppm")
                gold = golden_dir / f"screenshot_{i}.ppm"
                if cap.exists() and gold.exists():
                    match = compare_screenshot(str(cap), str(gold))
                    status = "MATCH" if match else "MISMATCH"
                    print(f"  Screenshot {i}: {status}")
                    if not match:
                        results["failed"] += 1
                elif cap.exists() and not gold.exists():
                    print(f"  Screenshot {i}: NO GOLDEN (save as {gold})")
                    # In record mode, copy it
                    import shutil
                    golden_dir.mkdir(exist_ok=True)
                    shutil.copy(cap, gold)
                    print(f"    -> Saved as golden reference")

    print(f"{'='*60}")

    # Exit code: 0 if all passed, 1 if any failure
    sys.exit(1 if results["failed"] > 0 or results["panics"] > 0 else 0)


if __name__ == "__main__":
    main()
```

### 6.3 Windows Batch Wrapper

```bat
REM test_runner.bat
@echo off
python test_runner.py %*
if %ERRORLEVEL% neq 0 (
    echo Some tests FAILED.
    exit /b 1
)
echo All tests passed.
```

Usage:
```bat
test_runner.bat --variant test --verbose
test_runner.bat --variant test-gui --input "hello\n" --suite kmalloc
test_runner.bat --variant prod --build-only
```

---

## 7. Screenshot-Based Visual Testing

### 7.1 How It Works

The test kernel draws a known pattern (e.g., fill screen red, draw a white rectangle), then outputs `[TEST] SCREENSHOT:N` to serial, where N is an integer identifying the screenshot. The host-side runner:

1. Receives `[TEST] SCREENSHOT:1` on serial
2. Issues `screendump /tmp/screenshot_1.ppm` via QMP
3. Compares the captured PPM against the golden reference at `golden/screenshot_1.ppm`
4. Reports match/mismatch

### 7.2 Test Side

```c
// test_display.c
TEST("display", fill_red) {
    screen_clear(COLOR_RED);
    screen_draw();

    // Signal the host to capture
    serial_outsl("[TEST] SCREENSHOT:1");

    // Wait for the host to capture (small delay)
    delay(100);

    TEST_PASS();
}

TEST("display", draw_rect) {
    screen_clear(COLOR_BLACK);
    screen_draw_box(V2I(100, 100), V2I(300, 300), COLOR_WHITE);
    screen_draw();

    serial_outsl("[TEST] SCREENSHOT:2");
    delay(100);

    TEST_PASS();
}
```

### 7.3 Golden Image Management

```bash
# Record new golden images
python test_runner.py --variant test-gui --record-screenshots

# The runner saves the captured images to golden/
# Manually review and commit:
git add golden/screenshot_1.ppm golden/screenshot_2.ppm
git commit -m "golden: update display test references"
```

---

## 8. Introspection Primitives

The hardest problems in kernel testing are verifying that internal state is correct. These primitives expose enough state for tests to make meaningful assertions.

### 8.1 Process Lifecycle Tests

```c
TEST("sched", create_and_reap) {
    u32 processes_before = test_get_process_count();

    kern_process_t *proc = process_create();
    TEST_ASSERT_NOT_NULL(proc);

    bool created = sched_create_process_thread(proc, dummy_thread, null);
    TEST_ASSERT(created, "thread creation failed");

    // Wait for the thread to exit
    delay(100);

    u32 processes_after = test_get_process_count();
    // Should be back to baseline (process was reaped)
    TEST_ASSERT_EQ(processes_before, processes_after);

    TEST_PASS();
}

TEST("sched", spawn_wasm_twice) {
    // Check that WASM can be spawned, runs, exits, and is reaped
    // before the second spawn attempt.
    i32 pid1 = wasm_spawn(&(wasm_spawn_opts_t){
        .path = "add_test.wasm",
        .wait = true,
    });
    TEST_ASSERT(pid1 > 0, "first spawn failed");

    // Verify process no longer exists
    u32 processes_after_first = test_get_process_count();
    TEST_ASSERT(!test_process_exists(pid1), "process wasn't reaped");

    // Second spawn — should work if first was properly cleaned up
    i32 pid2 = wasm_spawn(&(wasm_spawn_opts_t){
        .path = "add_test.wasm",
        .wait = true,
    });
    TEST_ASSERT(pid2 > 0, "second spawn failed (likely resource leak)");

    TEST_PASS();
}
```

### 8.2 Memory Leak Detection

```c
TEST("kmalloc", no_leak) {
    u64 heap_before = test_get_heap_used();

    for (i32 i = 0; i < 100; i++) {
        void *p = kmalloc(64);
        kfree(p);
    }

    u64 heap_after = test_get_heap_used();
    TEST_ASSERT_EQ(heap_before, heap_after);

    TEST_PASS();
}
```

### 8.3 File Handle Leak Detection

```c
TEST("fs", no_fd_leak) {
    u32 fds_before = test_get_total_open_fds();

    i32 fd = fs_open("testfile.txt");
    TEST_ASSERT(fd >= 0, "could not open testfile.txt");
    fs_close(fd);

    u32 fds_after = test_get_total_open_fds();
    TEST_ASSERT_EQ(fds_before, fds_after);

    TEST_PASS();
}
```

---

## 9. External Input Injection

### 9.1 Mechanism

QEMU's `-serial stdio` is bidirectional. The kernel reads from COM1 via `serial_in()`:

```c
// kern_serial.c — new function (4 lines, no #ifdef guard needed)
u8 serial_in(void) {
    if (inb(SERIAL_PORT + 5) & 1) {  // Data Ready bit (LSR bit 0)
        return inb(SERIAL_PORT);
    }
    return 0;
}
```

### 9.2 Test Pattern — Keyboard Simulation

```c
// test_keyboard.c
TEST("serial", receive_input) {
    // Tell the host we're ready for input
    serial_outsl("[TEST] READY_FOR_INPUT");

    // Read from serial until we get a newline
    char buf[64];
    u32 pos = 0;
    u64 start = sw;

    while (pos < sizeof(buf) - 1) {
        u8 c = serial_in();
        if (c) {
            if (c == '\n' || c == '\r') break;
            buf[pos++] = c;
        }
        // 5-second timeout
        if (sw - start > 500) {
            TEST_FAIL("timeout waiting for serial input");
        }
        sched_yield();
    }
    buf[pos] = 0;

    TEST_ASSERT(str_eql(buf, "HELLO", 5),
                "expected 'HELLO', got '%s'", buf);
    TEST_PASS();
}
```

### 9.3 Host Side

```python
# The runner sends data after [TEST] READY_FOR_INPUT
lines = run_qemu("test", input_data="HELLO\n")
```

### 9.4 Timing Guarantee

The host **never** sends data until it sees `[TEST] READY` or `[TEST] READY_FOR_INPUT`. This is the synchronization mechanism:

```
Time →  Host                            Guest (QEMU)
        │                               │
        │                               │ Boot, init serial, memory, etc.
        │                               │
        │  ← reads serial ───────────── │ [TEST] READY
        │                               │
        │  ← reads serial ───────────── │ [TEST] READY_FOR_INPUT
        │                               │
        │  writes "HELLO\n" ──────────→ │ serial_in() receives 'H','E','L','L','O','\n'
        │                               │ TEST_ASSERT(str_eql(buf, "HELLO"))
        │  ← reads serial ───────────── │ [TEST] PASS ...
        │                               │
        │  ← reads serial ───────────── │ [TEST] DONE
        │     (QEMU halts)              │
```

---

## 10. WASM-Specific Testing

### 10.1 The "Runs Once But Not Twice" Problem

This is the classic WASM resource leak. The test:

```c
TEST("wasm", run_twice) {
    // First run
    i32 pid1 = wasm_spawn(&(wasm_spawn_opts_t){
        .path = "add_test.wasm",
        .wait = true,
    });
    TEST_ASSERT(pid1 > 0, "first spawn failed");
    TEST_ASSERT(!test_process_exists(pid1), "process 1 not reaped");

    u32 fds_after_first = test_get_total_open_fds();
    u64 heap_after_first = test_get_heap_used();

    // Second run
    i32 pid2 = wasm_spawn(&(wasm_spawn_opts_t){
        .path = "add_test.wasm",
        .wait = true,
    });
    TEST_ASSERT(pid2 > 0, "second spawn failed");
    TEST_ASSERT(!test_process_exists(pid2), "process 2 not reaped");

    // Verify cleanup was complete
    u32 fds_after_second = test_get_total_open_fds();
    u64 heap_after_second = test_get_heap_used();

    TEST_ASSERT_EQ(fds_after_first, fds_after_second,
                   "fd leak detected");
    TEST_ASSERT_EQ(heap_after_first, heap_after_second,
                   "heap leak detected");

    TEST_PASS();
}
```

### 10.2 WASM Cleanup Checklist

The test runner should verify these are cleaned up after a WASM process exits:
1. ✓ m3_Runtime freed (via `m3_FreeRuntime`)
2. ✓ m3_Environment freed (via `m3_FreeEnvironment`)
3. ✓ WASM binary data freed (kmalloc'd buffer)
4. ✓ Process struct freed (via `process_exit`)
5. ✓ File descriptors closed (should be back to baseline)
6. ✓ Heap memory returned (no leak)
7. ✓ Task removed from scheduler's task list

---

## 11. Fault Injection Testing (Future)

Long-term, the test framework should support fault injection to test error handling:

```c
// In test mode, the kmalloc family can be told to fail:
test_set_alloc_fail_rate(50);   // 50% of allocations return NULL

// Then test that the filesystem handles OOM gracefully:
TEST("ext2", graceful_oom) {
    test_set_alloc_fail_rate(100);  // all allocations fail

    i32 fd = fs_open("testfile.txt");
    TEST_ASSERT_EQ(fd, -1);  // Should fail gracefully, not crash

    test_set_alloc_fail_rate(0);  // restore
    TEST_PASS();
}
```

This catches NULL-pointer dereferences in error paths that are rarely exercised in normal use.

---

## 12. Summary: Testing Tiers

| Tier | What's Tested | How | Speed | Bloat | 
|------|---------------|-----|-------|-------|
| **Tier 0** | `util_str`, `kern_mem` | Host-compiled (`gcc test_*.c -o test`) | <1s | None (standalone) |
| **Tier 1** | kmalloc, ext2, fs, sched | In-kernel, no-GUI mode, QEMU headless | ~5s | +2KB (.text) |
| **Tier 2** | WASM lifecycle, multiple spawns | In-kernel, no-GUI mode + wasm3 | ~10s | +~200KB (wasm3) |
| **Tier 3** | Display, keyboard, serial input | In-kernel, GUI mode + QEMU serial/QMP | ~15s | +15KB (screen) |
| **Tier 4** | Visual regression testing | Screenshot comparison via QMP | ~30s | Same as Tier 3 |

Tiers 0-1 run on every commit (fast, reliable).
Tiers 2-4 run nightly or before releases (slower, some manual verification needed).

---

## 13. Implementation Roadmap

### Phase 1: Foundation (Week 1)
- [ ] Create `include/kern_test.h` with macros and runner API
- [ ] Create `src/tests/test_runner.c` with linker-section iteration
- [ ] Add `.test_registry` to `link.ld`
- [ ] Add `TEST_MODE` boot path to `main.c`
- [ ] Refactor `build.sh` to accept mode argument and output to `build/{mode}/`
- [ ] Write Tier-0 tests: `test_util_str.c`, `test_kern_mem.c`
- [ ] Create host-compiled test runner for Tier-0 tests

### Phase 2: Kernel Tests (Week 2)
- [ ] Write Tier-1 tests: `test_kmalloc.c`, `test_ext2.c`, `test_fs.c`, `test_sched.c`
- [ ] Implement introspection primitives (`test_get_process_count`, etc.)
- [ ] Add panic handling for test mode (structured halt, not spin)
- [ ] Add watchdog timer support
- [ ] Create `test_runner.py` with build → QEMU → parse → exit code

### Phase 3: Input & Visual Tests (Week 3)
- [ ] Add `serial_in()` to `kern_serial.c`
- [ ] Write Tier-2 tests: `test_wasm.c` (run-twice pattern)
- [ ] Write Tier-3 tests: `test_keyboard.c` (serial input), `test_display.c`
- [ ] Add QMP `screendump` support to `test_runner.py`
- [ ] Create golden reference images
- [ ] Add screenshot comparison to CI pipeline
