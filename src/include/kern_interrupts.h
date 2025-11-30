//
// Created by tobin on 2025-11-24.
//

#ifndef KERN_INTERRUPTS_H
#define KERN_INTERRUPTS_H

#include "../include/dialect.h"


#define LAPIC_SVR 0xF0
#define LAPIC_EOI 0xB0
#define APIC_ENABLE 0x100

#define LAPIC_TIMER_LVT 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CURR 0x390
#define LAPIC_TIMER_DIV 0x3e0

#define LAPIC_LVT_MASKED 0x10000
#define LAPIC_TIMER_PERIODIC 0x20000

typedef struct {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attributes;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) idt_ptr_t;

typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_no;
    u64 error_code;
    u64 rip, cs, rflags, rsp, ss; // Pushed automatically by CPU
} registers_t;

//@formatter:off
u0 idt_set_gate(int n, u64 handler);
u0 interrupts_init();
u0 interrupt_register(u8 index, u0 (*handler)(const registers_t *));
//@formatter:on


#endif //KERN_INTERRUPTS_H
