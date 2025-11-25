//
// Created by tobin on 2025-11-24.
//

#ifndef KERN_SERIAL_H
#define KERN_SERIAL_H

#include "dialect.h"
#include "kern_asmstubs.h"

#define SERIAL_PORT 0x3f8
u8 init_serial();

u0 serial_outc(char c);

u0 serial_outs(char *str);
#endif //KERN_SERIAL_H
