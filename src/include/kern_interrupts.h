//
// Created by tobin on 2025-11-24.
//

#ifndef KERN_INTERRUPTS_H
#define KERN_INTERRUPTS_H

#include "../include/dialect.h"



typedef struct {
    u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 int_no;
    u64 error_code;
    u64 rip, cs, rflags, rsp, ss; // Pushed automatically by CPU
} registers_t;

//@formatter:off
u0 idt_set_gate(int n, u64 handler);
u0 interrupts_init();
u0 interrupt_register(u8 index, u0 (*handler)(const registers_t *));
u0 apic_eoi(u64 lapic_virtual_base);
//@formatter:on


#endif //KERN_INTERRUPTS_H
