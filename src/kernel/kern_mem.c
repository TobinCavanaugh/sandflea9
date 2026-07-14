//
// Created by tobin on 2025-03-10.
//

#include "../include/kern_mem.h"

u0 *mem_move(u0 *dest, const u0 *src, u64 n) {
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;

    if (d < s) {
        // Forward copy — use REP MOVSB for large chunks.
        if (n >= 64) {
            asm volatile(
                "rep movsb"
                : "+D"(d), "+S"(s), "+c"(n)
                :
                : "memory"
            );
        } else {
            while (n--) *d++ = *s++;
        }
    } else if (d > s) {
        // Backward copy — ERMSB only optimizes forward REP MOVSB.
        // STD+REP MOVSB falls back to byte-at-a-time microcode,
        // so no advantage over this simple backward loop.
        d += n;
        s += n;
        while (n--) *(--d) = *(--s);
    }
    return dest;
}

u0 mem_set(void *dest, u32 val, u64 count) {
    u8 *ptr = (u8 *)dest;
    u8 v = (u8)val;

    // REP STOSB: fill RCX bytes with AL starting at [RDI].
    // Microcode-optimized like REP MOVSB for large fills.
    if (count >= 64) {
        // Load the fill byte into EAX (REP STOSB uses AL)
        u64 fill = v;
        asm volatile(
            "rep stosb"
            : "+D"(ptr), "+c"(count)
            : "a"(fill)
            : "memory"
        );
    } else {
        while (count--) *ptr++ = v;
    }
}

u0 mem_copy(u8 *dest, const u8 *src, u64 n) {
    // REP MOVSB (ERMSB) — microcode-optimized memcpy on modern Intel/AMD.
    // The CPU handles cache-line splits, alignment, and write-combining
    // better than any explicit loop can, especially for large copies.
    // For tiny copies the microcode startup overhead isn't worth it,
    // so we fall back to a simple byte loop for chunks < 64 bytes.
    if (n >= 64) {
        asm volatile(
            "rep movsb"
            : "+D"(dest), "+S"(src), "+c"(n)
            :
            : "memory"
        );
    } else {
        while (n--) {
            *dest++ = *src++;
        }
    }
}
u0 mem_set32(u32 *dest, u32 val, u64 count) {
    // REP STOSD: fill RCX dwords with EAX starting at [RDI].
    // Processes 4 bytes per iteration — ~4x faster than byte loop.
    if (count >= 16) {  // 16 dwords = 64 bytes
        asm volatile(
            "rep stosl"
            : "+D"(dest), "+c"(count)
            : "a"(val)
            : "memory"
        );
    } else {
        while (count--) *dest++ = val;
    }
}