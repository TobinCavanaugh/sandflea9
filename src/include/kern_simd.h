//
// Cross-Architecture SIMD & Vector Operations for sandfleaOS
// Supports x86_64 (SSE2), ARM64 (NEON), and scalar fallback.
//

#ifndef KERN_SIMD_H
#define KERN_SIMD_H

#include "dialect.h"

#if defined(__x86_64__)
    #include <emmintrin.h> // Standard SSE2 intrinsics (built-in to GCC/Clang)
    typedef __m128i v128_t;

    #define SIMD_LOAD(ptr)          _mm_loadu_si128((const __m128i *)(ptr))
    #define SIMD_STORE(ptr, val)     _mm_storeu_si128((__m128i *)(ptr), (val))
    #define SIMD_STREAM(ptr, val)    _mm_stream_si128((__m128i *)(ptr), (val))
    #define SIMD_DUP_LO_32(val)      _mm_unpacklo_epi32((val), (val))
    #define SIMD_DUP_HI_32(val)      _mm_unpackhi_epi32((val), (val))

#elif defined(__aarch64__)
    #include <arm_neon.h>
    typedef uint32x4_t v128_t;

    #define SIMD_LOAD(ptr)          vld1q_u32((const u32 *)(ptr))
    #define SIMD_STORE(ptr, val)     vst1q_u32((u32 *)(ptr), (val))
    #define SIMD_STREAM(ptr, val)    vst1q_u32((u32 *)(ptr), (val))
    #define SIMD_DUP_LO_32(val)      vzip1q_u32((val), (val))
    #define SIMD_DUP_HI_32(val)      vzip2q_u32((val), (val))

#else
    // Generic scalar fallback for bringup / other architectures
    typedef struct { u32 p[4]; } v128_t;
    #define SIMD_GENERIC_FALLBACK
#endif

// ============================================================================
// Common High-Performance Routines
// ============================================================================

/**
 * 2x Aspect-Correct Row Scaler
 * Duplicates a row of 32-bit RGBA pixels horizontally across two destination scanlines.
 */
static inline void simd_scale_row_2x(u32 *dst0, u32 *dst1, const u32 *src, u32 pixel_count) {
#ifndef SIMD_GENERIC_FALLBACK
    u32 i = 0;
    for (; i + 4 <= pixel_count; i += 4) {
        v128_t in = SIMD_LOAD(src + i);
        v128_t lo = SIMD_DUP_LO_32(in); // [P0, P0, P1, P1]
        v128_t hi = SIMD_DUP_HI_32(in); // [P2, P2, P3, P3]

        SIMD_STORE(dst0 + i * 2,     lo);
        SIMD_STORE(dst0 + i * 2 + 4, hi);
        SIMD_STORE(dst1 + i * 2,     lo);
        SIMD_STORE(dst1 + i * 2 + 4, hi);
    }
    // Tail loop for remaining pixels
    for (; i < pixel_count; i++) {
        u32 p = src[i];
        dst0[i * 2]     = p;
        dst0[i * 2 + 1] = p;
        dst1[i * 2]     = p;
        dst1[i * 2 + 1] = p;
    }
#else
    for (u32 i = 0; i < pixel_count; i++) {
        u32 p = src[i];
        dst0[i * 2]     = p;
        dst0[i * 2 + 1] = p;
        dst1[i * 2]     = p;
        dst1[i * 2 + 1] = p;
    }
#endif
}

#endif // KERN_SIMD_H
