//
// Created by tobin on 2025-03-10.
//

#include "../include/kern_mem.h"

u0 *mem_move(u0 *dest, const u0 *src, u32 n) {
    u8 *d = dest;
    const u8 *s = src;

    if (d < s) {
        // Non-overlapping or src is before dest
        while (n--) {
            *d++ = *s++;
        }
    } else {
        // Overlapping case
        d += n;
        s += n;
        while (n--) {
            *(--d) = *(--s);
        }
    }
    return dest;
}

u0 mem_set(void *dest, u32 val, u32 count) {
    // AI GENERATED:
    unsigned char *ptr = (unsigned char *) dest;
    unsigned char v = (unsigned char) val;

    // 1. Handle small buffers immediately to avoid overhead
    if (count < 8) {
        while (count--) {
            *ptr++ = v;
        }
        // return dest;
    }

    // 2. Alignment: Write bytes until we hit an 8-byte boundary
    // ((uintptr_t)ptr & 7) gives us the offset from the last 8-byte boundary.
    // We want to move forward until that becomes 0.
    while ((u32) ptr & 7) {
        *ptr++ = v;
        count--;
    }

    // 3. Create the 64-bit fill pattern
    // We multiply the byte by this magic constant to broadcast it to all 8 bytes.
    // e.g., 0x11 * 0x0101... = 0x1111111111111111
    u64 wide_val = (u64) v * 0x0101010101010101ULL;
    u64 *wide_ptr = (u64 *) ptr;

    // 4. The "Unrolled" Bulk Fill
    // Process 32 bytes per iteration (4 x 64-bit writes).
    // This reduces loop condition checks and branching overhead.
    while (count >= 32) {
        wide_ptr[0] = wide_val;
        wide_ptr[1] = wide_val;
        wide_ptr[2] = wide_val;
        wide_ptr[3] = wide_val;

        wide_ptr += 4;
        count -= 32;
    }

    // 5. Handle remaining 8-byte blocks
    while (count >= 8) {
        *wide_ptr++ = wide_val;
        count -= 8;
    }

    // 6. Handle the "Tail" (remaining 0-7 bytes)
    ptr = (unsigned char *) wide_ptr;
    while (count--) {
        *ptr++ = v;
    }

    // return dest;
}

u0 mem_copy(u8 *dest, const u8 *src, u32 n) {
    for (i64 v = 0; v < n; v++) {
        dest[v] = src[v];
    }
}