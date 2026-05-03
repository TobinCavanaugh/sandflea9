#ifndef TOBIN_DIALECT
#define TOBIN_DIALECT

#define u0 void

#define purefn __attribute__((pure))

#define NULL ((void*) 0)
#define null NULL

#define bool _Bool
#define true 1
#define false 0

/* Use GCC's built-in types for fixed width integers */
typedef __UINT8_TYPE__   u8;
typedef __UINT16_TYPE__  u16;
typedef __UINT32_TYPE__  u32;
typedef __UINT64_TYPE__  u64;
typedef __INT8_TYPE__    i8;
typedef __INT16_TYPE__   i16;
typedef __INT32_TYPE__   i32;
typedef __INT64_TYPE__   i64;

typedef u8  uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;

typedef i8  int8_t;
typedef i16 int16_t;
typedef i32 int32_t;
typedef i64 int64_t;

typedef u64 uintptr_t;
typedef i64 intptr_t;

typedef float           f32;
typedef long double     f128;

/* Maximum values for unsigned types */
#define u8_MAX __UINT8_MAX__
#define u16_MAX __UINT16_MAX__
#define u32_MAX __UINT32_MAX__
#define u64_MAX __UINT64_MAX__

#define UINT32_MAX u32_MAX
#define UINT64_MAX u64_MAX

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

typedef u64 size_t;
typedef i64 ssize_t;

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

static u8 i32_min(i32 a, i32 b){
    return min(a, b);
}

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
