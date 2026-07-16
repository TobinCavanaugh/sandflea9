# Multi-Serial Port Logging & Profiling Architecture

## Overview

sandfleaOS currently uses a single COM port (COM1 at 0x3F8) for all serial output. This
document describes a three-channel serial architecture with integrated profiling.

### Goals

| Channel | COM Port | QEMU Backend         | Purpose                                  |
|---------|----------|----------------------|------------------------------------------|
| PRIMARY | COM1     | `stdio`              | General kernel logging (boot, errors)    |
| TEST    | COM2     | `file:test_log.txt`  | Test statuses, unit test output, asserts |
| PROFILE | COM3     | `file:profile.log`   | Structured profiling events (flame graph)|

---

## 1. Kernel Serial Layer

### 1.1 Channel Abstraction

```c
typedef enum {
    SERIAL_CH_PRIMARY = 0,   // COM1 @ 0x3F8
    SERIAL_CH_TEST    = 1,   // COM2 @ 0x2F8
    SERIAL_CH_PROFILE = 2,   // COM3 @ 0x3E8
    SERIAL_CH_COUNT   = 3
} serial_channel_t;
```

Three standard PC COM port I/O bases:
- COM1: `0x3F8` (IRQ 4)
- COM2: `0x2F8` (IRQ 3)
- COM3: `0x3E8` (IRQ 4)

### 1.2 New API

```c
u0  serial_init_all();                                    // Init all 3 ports
u0  serial_outc_ch (serial_channel_t ch, char c);
u0  serial_outs_ch (serial_channel_t ch, const char *str);
u0  serial_outsl_ch(serial_channel_t ch, const char *str);
u0  serial_outsf_ch(serial_channel_t ch, const char *fmt, ...);
```

### 1.3 Backward Compatibility

All existing `serial_outc/outs/outsl/outsf` functions become inline wrappers
that delegate to the `_ch` variants with `SERIAL_CH_PRIMARY`. Zero changes needed
in the ~104 existing call sites across 15+ files.

---

## 2. Profiling Framework

### 2.1 Event Format

Chrome Trace Event compatible, written to COM3:

```
<timestamp_us>:<type>:<name>[:<detail>]\n
```

| Type | Meaning                 | Example                          |
|------|-------------------------|----------------------------------|
| `B`  | Begin (duration start)  | `12345678:B:fs_read`             |
| `E`  | End (duration end)      | `12345901:E:fs_read`             |
| `I`  | Instant (single point)  | `12345000:I:boot:vmm_done`       |

Timestamps are **microseconds since boot**, computed by `profile_now_us()`:
- Base: `sw * 10000` (10ms tick × 1000 = 10000µs per tick)
- Interpolation: reads `LAPIC_TIMER_CURR` (APIC down-counter) for sub-tick precision
- Resolution: typically ~1µs (1000 / apic_ticks_per_ms)
- Falls back to 10ms coarse ticks during early boot (before `apic_init()`)

### 2.2 Time Type

```c
typedef u64 profile_time_t;       // µs since boot
profile_time_t profile_now_us();  // sub-ms precision via APIC interpolation
```

### 2.3 Macros

```c
PROFILE_BEGIN(name);             // Emit B event with µs timestamp
PROFILE_END(name);               // Emit E event
PROFILE_SCOPE(name);             // RAII-style: BEGIN now, END at scope exit
PROFILE_INSTANT(name, ...);      // Emit I event with µs timestamp
```

All macros invoke `profile_now_us()` internally — no manual timestamp passing needed.

### 2.3 Future: Function Instrumentation

With `-finstrument-functions`, every function call can emit B/E events
via `__cyg_profile_func_enter` / `__cyg_profile_func_exit`. This will be
gated behind a compile-time flag (`PROFILE_FUNCTIONS`) since it adds
significant overhead.

---

## 3. QEMU Configuration

### 3.1 `wr.bat` changes

```batch
qemu-system-x86_64.exe -cdrom sandfleaOS.iso ^
    -m 2G ^
    -machine pc ^
    -bios ovmf/DEBUGX64_OVMF.fd ^
    -display sdl ^
    -vga std ^
    -cpu Skylake-Server ^
    -drive file=disk.img,format=raw,index=0,media=disk ^
    -serial stdio ^
    -serial file:test_log.txt ^
    -serial file:profile.log ^
    -accel whpx,kernel-irqchip=off
```

COM1 → stdio (interactive), COM2 → `test_log.txt`, COM3 → `profile.log`.

### 3.2 Auto-clear on restart

The `wr.bat` already truncates `serial_output.log` via `break > serial_output.log`.
We'll do the same for the two new log files:

```batch
break > test_log.txt
break > profile.log
```

---

## 4. Python Sidecar & Web Viewer (Future)

### 4.1 Web Server

A Python script (`log_viewer.py`) serves:
- Static HTML with 3 tabs (Primary, Test, Profile)
- SSE (Server-Sent Events) for live log streaming
- Auto-clear detection: watches file mtimes, resets when QEMU restarts
- Color-coded log levels

### 4.2 Flame Graph Converter

A Python script (`profile_to_flame.py`) reads `profile.log`, matches B/E pairs,
computes durations, and outputs the format consumed by `flamegraph.pl` or
`speedscope.app`.

```
# Flame graph format (folded stacks):
func_a;func_b;func_c 42
func_a;func_d 15
```

---

## 5. Implementation Phases

| Phase | Task                                          | Status |
|-------|-----------------------------------------------|--------|
| 1     | Multi-channel serial layer                    | now    |
| 2     | QEMU config (wr.bat 3x `-serial`)             | now    |
| 3     | Profiling macros + kern_profile.h/c            | now    |
| 4     | Migrate test output to TEST channel           | later  |
| 5     | Python log viewer (3-tab web UI with SSE)     | later  |
| 6     | Python flame graph converter                  | later  |
| 7     | Function-level instrumentation (`-finstrument`)| later  |
| 8     | Scheduler event profiling                     | later  |
