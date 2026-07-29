//
// kern_profile.h — Profiling framework for sandfleaOS
//
// Writes Chrome Trace Event compatible JSON/line records to the PROFILE
// serial channel (COM3).  Events can be post-processed into flame graphs
// via a Python sidecar (profile_to_flame.py).
//
// Event format (one per line):
//   <timestamp_us>:<type>:<name>[:<detail>]\n
//
//   B  — Begin (duration start)
//   E  — End   (duration end)
//   I  — Instant (single point in time)
//
// Timestamps are microseconds derived from the x86 RDTSC instruction
// calibrated against the PIT at init time (the TSC epoch is CPU reset,
// not kernel boot, so raw timestamps are offset by ~seconds).  Because
// QEMU passes the host TSC through to the guest, these timestamps track
// actual wall-clock time, not virtual-machine time.  This keeps flame-
// graph durations meaningful even when QEMU runs at variable speed.
// Resolution is ~1 µs (tsc_per_us ≈ 3000 on a 3 GHz host).
//
// Usage:
//   PROFILE_BEGIN("fs_read");    // emit B event with µs timestamp
//   // ... work ...
//   PROFILE_END("fs_read");      // emit E event
//
//   PROFILE_INSTANT("page_fault addr=0x%x", addr);
//
//   PROFILE_SCOPE("render");     // B now, E at enclosing-scope exit
//

#ifndef KERN_PROFILE_H
#define KERN_PROFILE_H

#include "dialect.h"
#include "kern_serial.h"

// ── Compile-time toggle ─────────────────────────────────────────────────────
// Define PROFILE_ENABLED=0 to strip all profiling at compile time.
#ifndef PROFILE_ENABLED
# define PROFILE_ENABLED 1
#endif

// ── Time type ───────────────────────────────────────────────────────────────
// µs since boot, computed by profile_now_us() via APIC timer interpolation.
typedef u64 profile_time_t;

// ── Initialisation ──────────────────────────────────────────────────────────

u0 profile_init(void);

// Return µs-precision timestamp (available even when PROFILE_ENABLED == 0;
// useful for ad-hoc timing in application code).
profile_time_t profile_now_us(void);

// ── Core macros ─────────────────────────────────────────────────────────────

#if PROFILE_ENABLED

// Begin / End pairs (manual).
#define PROFILE_BEGIN(name) \
    serial_outsf_ch(SERIAL_CH_PROFILE, "%llu:B:%s\n", \
                    (u64)profile_now_us(), name)

#define PROFILE_END(name) \
    serial_outsf_ch(SERIAL_CH_PROFILE, "%llu:E:%s\n", \
                    (u64)profile_now_us(), name)

#define PROFILE_AUTO_BEGIN PROFILE_BEGIN(__func__)
#define PROFILE_AUTO_END PROFILE_END(__func__)

// Instant event (no duration).
// Usage: PROFILE_INSTANT("page_fault addr=0x%x", addr)
// Note: the first argument is the format-string tail (%% specifiers allowed)
// and the remaining varargs fill those specifiers.
#define PROFILE_INSTANT(fmt, ...) \
    serial_outsf_ch(SERIAL_CH_PROFILE, "%llu:I:" fmt "\n", \
                    (u64)profile_now_us(), ##__VA_ARGS__)

// RAII scope profiler — emits B on construction, E on scope exit.
// Example:  PROFILE_SCOPE("doom_tick");
#define PROFILE_SCOPE_CAT_(a, b)  a##b
#define PROFILE_SCOPE_VAR_(n)     PROFILE_SCOPE_CAT_(_prof_scope_, n)
#define PROFILE_SCOPE(name) \
    profile_scope_t PROFILE_SCOPE_VAR_(__LINE__) \
        __attribute__((cleanup(profile_scope_cleanup))) = { name, 0 }; \
    profile_scope_begin(&PROFILE_SCOPE_VAR_(__LINE__))

// ── Scope helper (used by PROFILE_SCOPE macro) ──────────────────────────

typedef struct {
    const char *name;
    u8          started;
} profile_scope_t;

static u0 profile_scope_begin(profile_scope_t *s) {
    s->started = 1;
    PROFILE_BEGIN(s->name);
}

static u0 profile_scope_cleanup(profile_scope_t *s) {
    if (s->started) PROFILE_END(s->name);
}

#else  // PROFILE_ENABLED == 0

#define PROFILE_BEGIN(name)       ((void)0)
#define PROFILE_END(name)         ((void)0)
#define PROFILE_INSTANT(...)      ((void)0)
#define PROFILE_SCOPE(name)       ((void)0)

#endif // PROFILE_ENABLED

#endif // KERN_PROFILE_H
