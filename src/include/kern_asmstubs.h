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

#endif //KERN_ASMSTUBS_H
