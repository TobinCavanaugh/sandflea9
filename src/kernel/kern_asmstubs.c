//
// Created by tobin on 2025-11-24.
//

#include "../include/kern_asmstubs.h"

u0 outb(u16 port, u8 val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

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

u0 outl(u16 port, u32 data) {
    asm volatile ("outl %0, %1" : : "a"(data), "d"(port));
}

u32 inl(u16 port) {
    u32 dat;
    asm volatile("inl %1, %0" : "=a"(dat) : "d"(port));
    return dat;
}

u16 inw(u16 port) {
    u16 dat;
    asm volatile("inw %1, %0" : "=a"(dat) : "d"(port));
    return dat;
}

u0 outw(u16 port, u16 val) {
    asm volatile ("outw %0, %1" : : "a"(val), "d"(port));
}

#define PCI_CONFIG_PORT 0xCF8

u32 pci_read_32(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 addr = (u32) ((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((u32) 0x80000000));
    // __asm__ volatile("outl %0, %1" : : "a"(addr), "Nd"((u16) 0xCF8));
    outl(PCI_CONFIG_PORT, addr);

    u32 tmp = 0;
    asm volatile ("inl %1, %0" : "=a"(tmp) : "Nd"((u16) 0xCFC));
    tmp = inl(0xCFC);

    return tmp;
}

u16 pci_read_16(u8 bus, u8 slot, u8 func, u8 offset) {
    u32 addr = (u32) ((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | (u32) 0x80000000);
    outl(PCI_CONFIG_PORT, addr);
    u32 val = inl(PCI_CONFIG_PORT);
    return U16((val >> ((offset & 2) * 8)) & 0xFFff);
}