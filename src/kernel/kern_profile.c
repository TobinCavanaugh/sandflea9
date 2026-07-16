//
// kern_profile.c — Profiling framework implementation.
//
// The profiling system writes structured events to the PROFILE serial
// channel (COM3). At init time we verify the channel is present and emit
// a metadata header.
//
// profile_now_us() uses the x86 RDTSC instruction (Time Stamp Counter).
// The TSC runs at the CPU's base frequency and ticks at a constant rate
// regardless of power state (invariant TSC).  On QEMU the TSC is passed
// through to the host, so timestamps track actual wall-clock time rather
// than virtual-machine time — this keeps flame-graph durations anchored
// to reality even when QEMU runs slower or faster than real time.
//
// Calibration: at init we use the PIT channel-0 one-shot to sleep for
// exactly 10 ms while measuring TSC ticks that elapse.  From that we
// derive tsc_per_us (≈ 3000 on a 3 GHz CPU), giving ~1 µs resolution.
//
// Before calibration completes (early boot), profile_now_us() returns 0.
//

#include "../include/kern_profile.h"
#include "../include/x64/apic.h"    // pit_prepare_sleep, pit_perform_sleep

// TSC ticks per microsecond — set once during profile_init().
static u64 tsc_per_us = 0;

// ── rdtsc helper ────────────────────────────────────────────────────────────

static inline u64 rdtsc(void) {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

// ── profile_now_us ──────────────────────────────────────────────────────────

profile_time_t profile_now_us(void) {
    if (!tsc_per_us)
        return 0;                       // not calibrated yet
    return rdtsc() / tsc_per_us;
}

// ── profile_init ────────────────────────────────────────────────────────────

u0 profile_init(void) {
#if PROFILE_ENABLED
    // Calibrate TSC against the PIT (8254 channel 0, one-shot mode).
    // The PIT runs at 1.193182 MHz; 10 ms = 11930 counts, independent
    // of the APIC timer or any virtualisation speed scaling.
    pit_prepare_sleep(10);
    u64 t0 = rdtsc();
    pit_perform_sleep();
    u64 t1 = rdtsc();
    tsc_per_us = (t1 - t0) / 10000;     // 10 ms = 10 000 µs

    if (!serial_channel_present(SERIAL_CH_PROFILE)) return;

    // Emit a metadata header so post-processors can identify the stream.
    serial_outsf_ch(SERIAL_CH_PROFILE,
        "%llu:I:profile_init:version=2,source=sandfleaOS,unit=us\n",
        (u64)profile_now_us());
#endif
}
