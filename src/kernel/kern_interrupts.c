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
#include "../include/x64/idt.h"
#include "../include/x64/apic.h"



u0 (*isr_handler[256])(const registers_t *) = {0};

u0 interrupt_register(u8 index, u0 (*handler)(const registers_t *)) {
    isr_handler[index] = handler;
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

u0 panic_draw_status(char *msg, const registers_t *t) {
    char buf[256];
    if (t) {
        stbsp_snprintf(buf, 255, " EXCEPTION: %s (RIP: %llX, ERR: %X) ", msg, t->rip, (u32)t->error_code);
    } else {
        stbsp_snprintf(buf, 255, " %-28s", msg);
    }
    display_t *disp = screen_current_display();
    if (disp) {
        screen_puts_r(buf, V2I(10, 10), COLOR_WHITE, COLOR_RED);
        screen_draw();
    }
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
        serial_outs("\n[[[ EXCEPTION: ");
        char *err = isr_errors[t->int_no];
        serial_outs(err);
        serial_outs(" ]]]\n");

        u64 cr2_val;
        asm volatile("mov %%cr2, %0" : "=r"(cr2_val));

        serial_outsf("  INT: %d | ERR: %X\n", (u32) t->int_no, (u32) t->error_code);
        serial_outsf("  RIP: %llX | CR2: %llX\n", t->rip, cr2_val);
        serial_outsf("  RAX: %llX | RBX: %llX | RCX: %llX | RDX: %llX\n", t->rax, t->rbx, t->rcx, t->rdx);
        serial_outsf("  RSP: %llX | RBP: %llX\n", t->rsp, t->rbp);

        panic_draw_status(err, t);
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

u0 interrupts_init() {
    // make sure the PIC is dead
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    idt_ptr_t idtr = idt_init();

    apic_init();

    asm volatile ("lidt %0" : : "m" (idtr));
    // Keep interrupts disabled until timer and keyboard handlers are registered in main.c
}


