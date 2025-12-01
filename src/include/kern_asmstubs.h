//
// Created by tobin on 2025-11-24.
//

#ifndef KERN_ASMSTUBS_H
#define KERN_ASMSTUBS_H

#include "dialect.h"

u0 outb(u16 port, u8 val);

u8 inb(u16 port);

u0 sti();

u0 cli();

u32 pci_read_32(u8 bus, u8 slot, u8 func, u8 offset);

u16 pci_read_16(u8 bus, u8 slot, u8 func, u8 offset);

#endif //KERN_ASMSTUBS_H
