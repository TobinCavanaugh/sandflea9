//
// Created by tobin on 2025-11-24.
//

#include "../include/kern_asmstubs.h"

u0 outb(u16 port, u8 val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

//inline can be sus
u8 inb(u16 port) {
    u8 ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

u0 sti() {
    asm volatile ("sti");
}

u0 cli() {
    asm volatile ("cli");
}
