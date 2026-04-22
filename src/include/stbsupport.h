//
// Created by tobin on 2025-11-30.
//

#ifndef STBSUPPORT_H
#define STBSUPPORT_H

#include "../include/dialect.h"

typedef __builtin_va_list va_list;

#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_copy(dest, src)  __builtin_va_copy(dest, src)

#define size_t u64
#define ptrdiff_t i64

#include "../include/stb_sprintf.h"

#endif //STBSUPPORT_H
