//
// Created by tobin on 2025-11-30.
//

#ifndef UTIL_STR_H
#define UTIL_STR_H

#include "../include/dialect.h"


u32 i64_to_sn(i64 val, char *out_buf, u8 base, u32 max_size);

u32 u64_to_sn(u64 val, char *out_buf, u8 base, u32 max_size);

u32 str_len(const char *str);

u8 str_eq(const char * a, const char * b);

u8 str_sw(const char *a , const char *b);

#endif //UTIL_STR_H
