#include "../include/stdint.h"

// Via https://github.com/llvm-mirror/compiler-rt/blob/master/lib/builtins/popcountdi2.c
int __popcountdi2(u64 x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

// Via https://github.com/microsoft/compiler-rt/blob/master/lib/builtins/popcountsi2.c
int __popcountsi2(u32 x) {
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return (int)(x & 0x0000003F);
}
