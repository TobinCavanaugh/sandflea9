//
// Created by tobin on 2025-11-30.
//

#ifndef UTIL_STR_H
#define UTIL_STR_H

#include "../include/dialect.h"


u32 i64_to_sn(i64 val, char *out_buf, u8 base, u32 max_size);

u32 u64_to_sn(u64 val, char *out_buf, u8 base, u32 max_size);

u32 str_len(const char *str);

u8 str_eq(const char *a, const char *b);

u8 str_eql(const char *a, const char *b, u32 len);

u8 str_sw(const char *a, const char *b);

char *str_dup(const char *a, void *(*Alloc_Func)(u64));
char *str_dup_len(const char *a, u32 len, void *(*Alloc_Func)(u64));

i64 sn_to_i64(const char *str, u32 max_len, u8 base);

#endif //UTIL_STR_H
