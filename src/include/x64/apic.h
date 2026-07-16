#include "../../include/dialect.h"

#define LAPIC_SVR 0xF0
#define LAPIC_EOI 0xB0
#define APIC_ENABLE 0x100

#define LAPIC_TIMER_LVT 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CURR 0x390
#define LAPIC_TIMER_DIV 0x3e0

#define LAPIC_LVT_MASKED 0x10000
#define LAPIC_TIMER_PERIODIC 0x20000

#define IOAPIC_PHYS_BASE 0xFEC00000
#define IOPACID 0x00
#define IOAPICVER 0x01
#define IOAPICARB 0x02
#define IOREDTBL 0x10

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_MASK 0xFFFFF000

u0 pit_prepare_sleep(u16 ms);
u0 pit_perform_sleep() ;
u0 apic_init();
u0 apic_eoi(u64 lapic_virtual_base);

// ── APIC timer calibration globals (exported for the profiler) ──────────────
// Set by apic_timer_init(), read by profile_now_us() to compute µs-precision
// timestamps by reading LAPIC_TIMER_CURR for sub-tick interpolation.
extern u64 apic_lapic_base;        // virtual address of local APIC registers
extern u32 apic_ticks_per_ms;     // APIC timer ticks per millisecond
extern u32 apic_period_ticks;     // total ticks in one full 10ms period
