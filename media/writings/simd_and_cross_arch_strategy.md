# SIMD & Cross-Architecture Vector Strategy for sandfleaOS

## 1. Executive Summary & Core Philosophy

sandfleaOS aims to run efficiently across modern hardware architectures with a focus on simplicity, portability, and zero runtime bloat. This document outlines our unified strategy for SIMD (Single Instruction, Multiple Data) operations and cross-architecture readiness—specifically bridging **x86_64** (today) and **ARM64 / AArch64** (future roadmap).

### Core Principles
1. **Single Universal Binary**: We build a single OS image per target architecture family. No dynamic runtime dispatch tables or multi-variant ISOs.
2. **The 128-Bit Universal Sweet Spot**: Every 64-bit x86 CPU ever manufactured supports SSE/SSE2. Every ARMv8-A CPU guarantees 128-bit NEON. Both map 1:1 to 4×32-bit RGBA pixel pipelines.
3. **Hardware Microcode for Bulk Memory**: For pure memory copying and filling, CPU microcode (`rep movsb` / ERMSB on x86, DC ZVA on ARM) already saturates 100% of DRAM memory bus bandwidth without SIMD power states or cache pollution.
4. **Scanline-Level Abstraction**: Abstractions must operate at the row or block level, never per-pixel, guaranteeing zero function-call overhead.

---

## 2. Hardware Landscape & Why AVX-512 is Avoided

| Feature | x86_64 Baseline (SSE2) | x86_64 Advanced (AVX-512) | ARM64 Baseline (NEON) |
| :--- | :--- | :--- | :--- |
| **Vector Width** | 128-bit (4×32-bit px) | 512-bit (16×32-bit px) | 128-bit (4×32-bit px) |
| **Hardware Guarantee** | 100% of all x64 CPUs | ~15% of desktop/laptop CPUs | 100% of ARMv8-A CPUs |
| **Frequency Penalty** | **0% (Max Turbo)** | **15%–30% Downclock** | **0% (Full Speed)** |
| **Cache-Line Split Risk**| Very Low (16-byte) | High (64-byte boundary) | Very Low (16-byte) |
| **Memory Bandwidth** | Saturates DRAM Bus | Saturates DRAM Bus | Saturates DRAM Bus |

### Why AVX-512 is an Anti-Pattern for sandfleaOS:
* **The "AVX-512 License" Clock Penalty**: On Intel architectures (Skylake-X, Ice Lake, Tiger Lake), executing 512-bit instructions forces the CPU voltage regulator to downclock the entire core by up to 30% to prevent voltage sag. If the kernel only blits a frame for 1ms, scalar code runs throttled for milliseconds afterward.
* **Cache Line Splitting**: A 512-bit vector is exactly one L1 cache line (64 bytes). Any unaligned framebuffer pointer causes a cache-line split, stalling the CPU pipeline.
* **Memory Bottleneck**: Blitting a 1080p/4K framebuffer is bottlenecked by physical DRAM/PCIe bus throughput (~30–50 GB/s), not ALU compute throughput. 128-bit vectors and `rep movsb` already max out the memory bus.

---

## 3. The 128-Bit Unified Portability Layer

Both x86_64 and ARM64 naturally operate on 128-bit vector registers. By defining a thin, header-only inline layer (e.g. `src/include/kern_simd.h`), we write graphics and scaling algorithms once with zero runtime overhead.

### Type Mapping

| Concept | x86_64 (SSE2) | ARM64 (NEON) | Portable C Fallback |
| :--- | :--- | :--- | :--- |
| **4×32-bit Vector** | `__m128i` | `uint32x4_t` | `u32[4]` or GCC `vector_size(16)` |
| **Unaligned Load** | `_mm_loadu_si128` | `vld1q_u32` | `memcpy` / direct pointer |
| **Unaligned Store** | `_mm_storeu_si128` | `vst1q_u32` | `memcpy` / direct pointer |
| **Streaming Store (NT)** | `_mm_stream_si128` | `vst1q_u32` | direct pointer |
| **Interleave / Unpack Lo** | `_mm_unpacklo_epi32`| `vzip1q_u32` | scalar shift/mask |
| **Interleave / Unpack Hi** | `_mm_unpackhi_epi32`| `vzip2q_u32` | scalar shift/mask |

---

## 4. Concrete Cross-Architecture Implementation

Below is the reference pattern for `kern_simd.h` that enables zero-overhead compile-time vectorization across x86_64, ARM64, and scalar platforms:

```c
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
    // Generic fallback for bringup / other architectures
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
        dst0[i * 2] = dst0[i * 2 + 1] = dst1[i * 2] = dst1[i * 2 + 1] = p;
    }
#else
    for (u32 i = 0; i < pixel_count; i++) {
        u32 p = src[i];
        dst0[i * 2] = dst0[i * 2 + 1] = dst1[i * 2] = dst1[i * 2 + 1] = p;
    }
#endif
}

#endif // KERN_SIMD_H
```

---

## 5. Application Targets in sandfleaOS

### 1. Framebuffer Scaling (Doom & Retrogaming)
* Standard retro games render at 320×200 or 640×400/480.
* On modern 1080p, 1440p, and 4K panels, `simd_scale_row_2x` / `simd_scale_row_3x` scales pixel data in real time directly to `disp->trueAddress` in $<0.5\text{ms}$ per frame.

### 2. SSFN Font Rasterizer & Alpha Blending
* When rendering anti-aliased font glyphs, the rasterizer blends source alpha with destination pixels:
  $$\text{Dst} = \frac{\text{Src} \times \alpha + \text{Dst} \times (255 - \alpha)}{255}$$
* 128-bit SIMD blends 4 color channels simultaneously in packed 16-bit integers, making full-screen terminal scrolling smooth even on high-DPI displays.

### 3. Bulk Memory Transfers
* Handled via `mem_copy()` and `mem_set()`.
* On x86_64: `rep movsb` / `rep stosb` leveraging ERMSB silicon microcode.
* On ARM64: `DC ZVA` (Data Cache Zero by Virtual Address) and LDP/STP 128-bit burst pairs.

---

## 6. Summary Checklist for Future Development

- [x] **Universal Baseline**: Commit to 128-bit vectors as the standard cross-platform unit.
- [x] **No Dynamic CPU Dispatch**: Avoid runtime `cpuid` indirect branch tables in performance loops.
- [x] **Header-Only Inlining**: Keep vector helpers in `src/include/kern_simd.h`.
- [x] **ARM64 Ready**: Using standard 128-bit vector operations ensures seamless bringup on Apple Silicon, Raspberry Pi, and ARM servers with zero rewrite needed.
