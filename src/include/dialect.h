#ifndef TOBIN_DIALECT
#define TOBIN_DIALECT

#define u0 void


#define NULL ((void*) 0)
#define null NULL

#define bool _Bool
#define true 1
#define false 0

/* Use GCC's built-in types for fixed width integers */
#define u8 __UINT8_TYPE__
#define u16 __UINT16_TYPE__
#define u32 __UINT32_TYPE__
#define u64 __UINT64_TYPE__
#define i8 __INT8_TYPE__
#define i16 __INT16_TYPE__
#define i32 __INT32_TYPE__
#define i64 __INT64_TYPE__
#define f32 float
#define f128 long double

/* Maximum values for unsigned types */
#define u8_MAX __UINT8_MAX__
#define u16_MAX __UINT16_MAX__
#define u32_MAX __UINT32_MAX__
#define u64_MAX __UINT64_MAX__

/* Maximum and minimum values for signed types */
#define i8_MAX 127
#define i8_MIN (-128)
#define i16_MAX 32767
#define i16_MIN (-32768)
#define i32_MAX 2147483647
#define i32_MIN (-2147483648)
#define i64_MAX 9223372036854775807LL
#define i64_MIN (-9223372036854775807LL - 1LL)

/* Floating-point constants for f32 (float) */
/* CHECKING EQUALITY AGAINST THESE WILL RETURN FALSE*/
#define f32_NaN (__builtin_nanf(""))
#define f32_Inf (__builtin_inff())
#define f32_NegInf (__builtin_huge_valf() * -1.0F)

#define f32_IsNaN(__a) (__builtin_isnan(__a))
#define f32_IsInf(__a) (__builtin_isinf(__a))

/* Casting macros */
#define U8(__a) ((u8)(__a))
#define U16(__a) ((u16)(__a))
#define U32(__a) ((u32)(__a))
#define U64(__a) ((u64)(__a))
#define I8(__a) ((i8)(__a))
#define I16(__a) ((i16)(__a))
#define I32(__a) ((i32)(__a))
#define I64(__a) ((i64)(__a))
#define F32(__a) ((f32)(__a))
#define F128(__a) ((f128)(__a))

#define uint8_t u8
#define uint16_t u16
#define uint32_t u32
#define uint64_t u64

#define int8_t u8
#define int16_t u16
#define int32_t u32
#define int64_t u64

#define iif(__condition, ...) ({ __condition ? ({ __VA_ARGS__; }) : 0; })

#define max(a, b) ({ \
    typeof(a) _a = (a); \
    typeof(b) _b = (b); \
    _a > _b ? _a : _b; \
})

#define min(a, b) ({ \
    typeof(a) _a = (a); \
    typeof(b) _b = (b); \
    _a < _b ? _a : _b; \
})

#define clamp(x, low, high) ({ \
    typeof(x) _x = (x); \
    typeof(low) _low = (low); \
    typeof(high) _high = (high); \
    _x < _low ? _low : (_x > _high ? _high : _x); \
})

#define abs(x) ({ \
    typeof(x) _x = (x); \
    _x < 0 ? -_x : _x; \
})

#define count_digits(x) ({ \
    typeof(x) _x = (x); \
    i32 _count = 0; \
    while(_x > 10) { _x /= 10; ++_count; }\
    /*return*/ _count; \
})

#endif /* TOBIN_DIALECT */
