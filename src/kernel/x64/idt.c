
#include "../../include/x64/idt.h"
#include "../../include/dialect.h"

extern u64 isr_table[256];

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

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, isr_table[i]);
    }

    return idtr;
}