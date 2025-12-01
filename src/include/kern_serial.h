//
// Created by tobin on 2025-11-24.
//

#ifndef KERN_SERIAL_H
#define KERN_SERIAL_H

#include "dialect.h"
#include "kern_asmstubs.h"

#define SERIAL_PORT 0x3f8

typedef enum : u8 {
    BASE_10 = 10,
    BASE_HEX = 16,
    BASE_OCTAL = 8,
    BASE_DECIMAL = 10,
    BASE_BINARY = 2,
} BASE_FMT;

u8 init_serial();

u0 serial_outc(char c);

u0 serial_outs(char *str);

u0 serial_outi64(i64 val, BASE_FMT base);

u0 serial_outu64(u64 val, BASE_FMT base);

#endif //KERN_SERIAL_H
