#ifndef SANDFLEA_LIMITS_H
#define SANDFLEA_LIMITS_H

#include "dialect.h"

#define CHAR_BIT 8

#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255

#define SHRT_MIN  (-32768)
#define SHRT_MAX  32767
#define USHRT_MAX 65535

#define INT_MIN   i32_MIN
#define INT_MAX   i32_MAX
#define UINT_MAX  u32_MAX

#define LONG_MIN  i64_MIN
#define LONG_MAX  i64_MAX
#define ULONG_MAX u64_MAX

#define LLONG_MIN  i64_MIN
#define LLONG_MAX  i64_MAX
#define ULLONG_MAX u64_MAX

// Wasm3 also wants these from limits.h sometimes
#define INT32_MIN i32_MIN
#define INT32_MAX i32_MAX
#define UINT32_MAX u32_MAX

#define INT64_MIN i64_MIN
#define INT64_MAX i64_MAX
#define UINT64_MAX u64_MAX

#endif
