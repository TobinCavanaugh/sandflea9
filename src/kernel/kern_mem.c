//
// Created by tobin on 2025-03-10.
//

#include "../include/kern_mem.h"

u0 *mem_move(u0 *dest, const u0 *src, u64 n) {
    u8 *d = dest;
    const u8 *s = src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *(--d) = *(--s);
        }
    }
    return dest;
}

u0 mem_set(void *dest, u32 val, u64 count) {
    u8 *ptr = (u8 *) dest;
    u8 v = (u8) val;
    while (count--) {
        *ptr++ = v;
    }
}

u0 mem_copy(u8 *dest, const u8 *src, u64 n) {
    u8 *d = dest;
    const u8 *s = src;
    while (n--) {
        *d++ = *s++;
    }
}

u0 mem_set32(u32 *dest, u32 val, u64 count) {
    while (count--) {
        *dest++ = val;
    }
}