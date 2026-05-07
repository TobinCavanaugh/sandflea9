
#include "../../include/x64/idt.h"
#include "../../include/dialect.h"


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

u0 idt_set_gate(int n, u64 handler) {
    idt_array[n].offset_low = (u16) (handler & 0xFFFF);
    idt_array[n].selector = (0x28); // set by Limine
    idt_array[n].ist = 0;
    idt_array[n].type_attributes = 0x8E;
    idt_array[n].offset_mid = (u16) ((handler >> 16) & 0xFFFF);
    idt_array[n].offset_high = (u32) ((handler >> 32) & 0xFFFFFFFF);
    idt_array[n].reserved = 0;
}



idt_ptr_t idt_init(){
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

    return idtr;
}