//
// Created by tobin on 2025-03-10.
//

#ifndef KERN_MEM_H
#define KERN_MEM_H

#include "dialect.h"


u0 *mem_move(u0 *dest, const u0 *src, u64 n);

u0 mem_set(void *dest, u32 val, u64 count);

u0 mem_set32(u32 *dest, u32 val, u64 count);

u0 mem_copy(u8 * dest, const u8 * src, u64 n);

#endif //KERN_MEM_H
