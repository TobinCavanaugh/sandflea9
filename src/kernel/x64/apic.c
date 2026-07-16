
#include "../../include/dialect.h"
#include "../../include/x64/apic.h"
#include "../../include/kern_vmm.h"
#include "../../include/kern_asmstubs.h"
#include "../../include/kern_serial.h"


u0 apic_timer_init(u64 lapic_base, u8 vector, u32 ms);

// ── Exported calibration globals — read by profile_now_us() ─────────────────
u64 apic_lapic_base   = 0;
u32 apic_ticks_per_ms = 0;
u32 apic_period_ticks = 0;

extern u64 rdmsr(u32 msr);

u64 get_apic_base() {
    u64 msr_val = rdmsr(IA32_APIC_BASE_MSR);
    return msr_val & APIC_BASE_MASK;
}


u0 lapic_write(u64 base, u32 reg, u32 val) {
    volatile u32 *addr = (volatile u32 *) (base + reg);
    *addr = val;
}

u0 enable_apic(u64 lapic_virtual_base) {
    lapic_write(lapic_virtual_base, LAPIC_SVR, APIC_ENABLE | 0xFF);
}


u0 apic_eoi(u64 lapic_virtual_base) {
    lapic_write(lapic_virtual_base, LAPIC_EOI, 0);
}

u0 ioapic_write(u64 ioapic_virt_base, u8 reg, u32 val) {
    volatile u32 *index_reg = (volatile u32 *) ioapic_virt_base;
    *index_reg = reg;

    volatile u32 *data_reg = (volatile u32 *) (ioapic_virt_base + 0x10);
    *data_reg = val;
}

u0 enable_keyboard_ioapic(u64 ioapic_virt_base) {
    u32 low_bits = 0x21;
    u32 high_bits = 0;

    ioapic_write(ioapic_virt_base, 0x10 + (1 * 2), low_bits);
    ioapic_write(ioapic_virt_base, 0x10 + (1 * 2) + 1, high_bits);
}

u0 apic_timer_init(u64 lapic_base, u8 vector, u32 ms) {
    lapic_write(lapic_base, LAPIC_TIMER_DIV, 0x3);
    pit_prepare_sleep(10);
    lapic_write(lapic_base, LAPIC_TIMER_INIT, 0xFFFFffff);
    pit_perform_sleep();
    lapic_write(lapic_base, LAPIC_TIMER_LVT, LAPIC_LVT_MASKED);

    u32 ticks_in_10ms = 0xFFFFffff - *(volatile u32 *) (lapic_base + LAPIC_TIMER_CURR);

    u32 tpms = ticks_in_10ms / 10;
    u32 total_ticks = tpms * ms;

    // Export calibration for the profiler
    apic_lapic_base   = lapic_base;
    apic_ticks_per_ms = tpms;
    apic_period_ticks = total_ticks;

    lapic_write(lapic_base, LAPIC_TIMER_LVT, vector | LAPIC_TIMER_PERIODIC);
    lapic_write(lapic_base, LAPIC_TIMER_DIV, 0x3);
    lapic_write(lapic_base, LAPIC_TIMER_INIT, total_ticks);

    serial_outs("APIC timer initialized\n");
}

u0 apic_init() {
    u64 apic_phys = get_apic_base();
    u64 apic_virt = 0xFFFFFFFF10000000;
    vmm_map_page(apic_phys, apic_virt, PAGE_RW | PAGE_PCD | PAGE_PWT);
    enable_apic(apic_virt);
    apic_timer_init(apic_virt, 32, 10); //10ms

    u64 ioapic_virt = 0xFFFFFFFF10001000;
    vmm_map_page(IOAPIC_PHYS_BASE, ioapic_virt, PAGE_RW | PAGE_PCD);
    enable_keyboard_ioapic(ioapic_virt);

}

u0 pit_prepare_sleep(u16 ms) {
    outb(0x43, 0x30);
    u16 count = 1193 * ms; // 1.193182 Mhz
    outb(0x40, count & 0xFF);
    outb(0x40, (count >> 8) & 0xFF);
}

u0 pit_perform_sleep() {
    u16 last_tick = 0xFFFF; // Start with the max possible value

    while (1) {
        // Send Latch Command to Channel 0 (0x00) to read the current count
        outb(0x43, 0x00);

        // Read Low byte then High byte
        u8 lo = inb(0x40);
        u8 hi = inb(0x40);
        u16 current_tick = lo | (hi << 8);

        // In Mode 0, the counter decrements.
        // 1. If current_tick > last_tick, it wrapped around (reached 0 then 0xFFFF).
        if (current_tick > last_tick) {
            break;
        }

        // 2. If it reaches a very small number (close to 0), we are done.
        // (Some emulators might stop at 0 instead of wrapping immediately)
        if (current_tick < 10) {
            break;
        }

        last_tick = current_tick;
    }
}

