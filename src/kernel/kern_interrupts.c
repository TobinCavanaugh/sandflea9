//
// Created by tobin on 2025-11-24.
//

#include "../include/kern_interrupts.h"
#include "../include/kern_vmm.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_screen.h"
#include "../include/kern_serial.h"
#include "../include/stbsupport.h"
#include "../include/kern_sched.h"

u0 apic_timer_init(u64 lapic_base, u8 vector, u32 ms);

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

//@formatter:off
extern void isr0(u0);
extern void isr1(u0);
extern void isr2(u0);
extern void isr3(u0);
extern void isr4(u0);
extern void isr5(u0);
extern void isr6(u0);
extern void isr7(u0);
extern void isr8(u0);
extern void isr9(u0);
extern void isr10(u0);
extern void isr11(u0);
extern void isr12(u0);
extern void isr13(u0);
extern void isr14(u0);
extern void isr15(u0);
extern void isr16(u0);
extern void isr17(u0);
extern void isr18(u0);
extern void isr19(u0);
extern void isr20(u0);
extern void isr21(u0);
extern void isr22(u0);
extern void isr23(u0);
extern void isr24(u0);
extern void isr25(u0);
extern void isr26(u0);
extern void isr27(u0);
extern void isr28(u0);
extern void isr29(u0);
extern void isr30(u0);
extern void isr31(u0);

extern void isr32(u0);
extern void isr33(u0); //<-- Is this actuall an IRQ?
//@formatter:on

static idt_entry_t idt_array[256] = {0};

u0 (*isr_handler[256])(const registers_t *) = {0};

u0 interrupt_register(u8 index, u0 (*handler)(const registers_t *)) {
    isr_handler[index] = handler;
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


char *isr_errors[] = {
        "Divide by Zero",
        "Debug Exception",
        "Non Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "Bound",
        "Invalid Opcode",
        "No FPU",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved Intel",
        "FPU error",
        "Alignment Check",
        "Machine Check",
        "SIMD Exception",
        "Virtualization Exception",
        "Control Protection Exception", // 21
        "Reserved Intel", // 22
        "Reserved Intel", // 23
        "Reserved Intel", // 24
        "Reserved Intel", // 25
        "Reserved Intel", // 26
        "Reserved Intel", // 27
        "Reserved Intel", // 28
        "Reserved Intel", // 29
        "Reserved Intel", // 30
        "Reserved Intel", // 31
};

u0 panic_draw_status(char *msg) {
    char buf[256];
    stbsp_snprintf(buf, 255, " %-28s", msg);
    i32 w = (28 + 1) * font_width + 1;
    display_t *disp = screen_current_display();
    i32 x_pos = disp->surface.width - w;

    screen_puts_r(buf, V2I(x_pos, 0), COLOR_WHITE, COLOR_RED);
    screen_draw();
}

u0 kern_interrupt_handler(const registers_t *t) {
    if (t->int_no == 14) {
        u64 cr2_val;
        asm volatile("mov %%cr2, %0" : "=r"(cr2_val));

        // Lazy sync for kernel mappings (higher half)
        // Skip APIC/IOAPIC range (mapped specifically in each process's PML4 during init)
        if (cr2_val >= 0xFFFF800000000000 && (cr2_val < 0xFFFFFFFF10000000 || cr2_val >= 0xFFFFFFFF10002000)) {
            kern_process_t *kp = sched_get_kernel_process();
            if (kp) {
                u64 current_cr3 = read_cr3();
                if (current_cr3 != kp->cr3) {
                    u64 pml4_idx = PML4_INDEX(cr2_val);
                    u64 *current_pml4 = (u64 *) (current_cr3 + vmm_get_hhdm());
                    u64 *master_pml4 = (u64 *) (kp->cr3 + vmm_get_hhdm());

                    if (!(current_pml4[pml4_idx] & PAGE_PRESENT) && (master_pml4[pml4_idx] & PAGE_PRESENT)) {
                        current_pml4[pml4_idx] = master_pml4[pml4_idx];
                        
                        // Full TLB flush
                        u64 cr3_val;
                        asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
                        asm volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
                        
                        return; // Resume execution
                    }
                }
            }
        }

        // Check if it's a page fault in the kernel heap range
        // DISABLED auto-mapping to catch stack overflows and use-after-free
        /*
        if (cr2_val >= KHEAP_START_ADDR && cr2_val < (KHEAP_START_ADDR + 0x4000000)) { // 64MB heap range
            u64 phys = pmm_alloc_page();
            vmm_map_page(phys, cr2_val & ~0xFFF, PAGE_PRESENT | PAGE_RW);
            return; // Resume execution
        }
        */
        serial_outsf("\n!!! PAGE FAULT at %llX (Access: %s, %s) !!!\n", 
            cr2_val, 
            (t->error_code & 1) ? "Protection violation" : "Not present",
            (t->error_code & 2) ? "Write" : "Read");
    }

    if (t->int_no == 3) {
        goto HANDLE;
    }

    if (t->int_no <= 31) {
        serial_outs("\n!!! EXCEPTION: ");
        char *err = isr_errors[t->int_no];
        serial_outs(err);
        serial_outs(" !!!\n");

        u64 cr2_val;
        asm volatile("mov %%cr2, %0" : "=r"(cr2_val));

        serial_outsf("  INT: %d | ERR: %X\n", (u32) t->int_no, (u32) t->error_code);
        serial_outsf("  RIP: %llX | CR2: %llX\n", t->rip, cr2_val);
        serial_outsf("  RAX: %llX | RBX: %llX | RCX: %llX | RDX: %llX\n", t->rax, t->rbx, t->rcx, t->rdx);
        serial_outsf("  RSP: %llX | RBP: %llX\n", t->rsp, t->rbp);

        __asm__ volatile("int3");

        panic_draw_status(err);
        while (1) {
            for (volatile i64 i = 0; i < 1000000; i++) {
                toggle_capslock();
            }
        }
    }

    HANDLE:;
    if (isr_handler[t->int_no]) {
        isr_handler[t->int_no](t);
    } else {
        inb(0x60); //<-- suboptimal
    }

    apic_eoi(0xFFFFFFFF10000000);
}

u0 idt_set_gate(int n, u64 handler) {
    idt_array[n].offset_low = (u16) (handler & 0xFFFF);
    idt_array[n].selector = (0x28); // set by Limine
    idt_array[n].ist = 0;
    idt_array[n].type_attributes = 0x8E;
    idt_array[n].offset_mid = (u16) ((handler >> 16) & 0xFFFF);
    idt_array[n].offset_high = (u32) ((handler >> 32) & 0xFFFFFFFF);
    idt_array[n].reserved = 0;
}

u0 interrupts_init() {
    // make sure the PIC is dead
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    idt_ptr_t idtr;
    idtr.limit = sizeof(idt_entry_t) * 256 - 1;
    idtr.base = (u64) &idt_array;

    idt_set_gate(0, (u64) isr0);
    idt_set_gate(1, (u64) isr1);
    idt_set_gate(2, (u64) isr2);
    idt_set_gate(3, (u64) isr3);
    idt_set_gate(4, (u64) isr4);
    idt_set_gate(5, (u64) isr5);
    idt_set_gate(6, (u64) isr6);
    idt_set_gate(7, (u64) isr7);
    idt_set_gate(8, (u64) isr8);
    idt_set_gate(9, (u64) isr9);
    idt_set_gate(10, (u64) isr10);
    idt_set_gate(11, (u64) isr11);
    idt_set_gate(12, (u64) isr12);
    idt_set_gate(13, (u64) isr13);
    idt_set_gate(14, (u64) isr14);
    idt_set_gate(15, (u64) isr15);
    idt_set_gate(16, (u64) isr16);
    idt_set_gate(17, (u64) isr17);
    idt_set_gate(18, (u64) isr18);
    idt_set_gate(19, (u64) isr19);
    idt_set_gate(20, (u64) isr20);
    idt_set_gate(21, (u64) isr21);
    idt_set_gate(22, (u64) isr22);
    idt_set_gate(23, (u64) isr23);
    idt_set_gate(24, (u64) isr24);
    idt_set_gate(25, (u64) isr25);
    idt_set_gate(26, (u64) isr26);
    idt_set_gate(27, (u64) isr27);
    idt_set_gate(28, (u64) isr28);
    idt_set_gate(29, (u64) isr29);
    idt_set_gate(30, (u64) isr30);
    idt_set_gate(31, (u64) isr31);
    idt_set_gate(32, (u64) isr32);
    idt_set_gate(33, (u64) isr33);


    u64 apic_phys = get_apic_base();
    u64 apic_virt = 0xFFFFFFFF10000000;
    vmm_map_page(apic_phys, apic_virt, PAGE_RW | PAGE_PCD | PAGE_PWT);
    enable_apic(apic_virt);
    apic_timer_init(apic_virt, 32, 10); //10ms

    u64 ioapic_virt = 0xFFFFFFFF10001000;
    vmm_map_page(IOAPIC_PHYS_BASE, ioapic_virt, PAGE_RW | PAGE_PCD);
    enable_keyboard_ioapic(ioapic_virt);

    asm volatile ("lidt %0" : : "m" (idtr));
    sti();
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

u0 apic_timer_init(u64 lapic_base, u8 vector, u32 ms) {
    lapic_write(lapic_base, LAPIC_TIMER_DIV, 0x3);
    pit_prepare_sleep(10);
    lapic_write(lapic_base, LAPIC_TIMER_INIT, 0xFFFFffff);
    pit_perform_sleep();
    lapic_write(lapic_base, LAPIC_TIMER_LVT, LAPIC_LVT_MASKED);

    u32 ticks_in_10ms = 0xFFFFffff - *(volatile u32 *) (lapic_base + LAPIC_TIMER_CURR);

    u32 tpms = ticks_in_10ms / 10;
    u32 total_ticks = tpms * ms;

    lapic_write(lapic_base, LAPIC_TIMER_LVT, vector | LAPIC_TIMER_PERIODIC);
    lapic_write(lapic_base, LAPIC_TIMER_DIV, 0x3);
    lapic_write(lapic_base, LAPIC_TIMER_INIT, total_ticks);

    serial_outs("APIC timer initialized\n");
}
