#ifndef WABT_CONFIG_H_
#define WABT_CONFIG_H_

#include <stdint.h>
#include <stdlib.h>

#define WABT_VERSION_STRING "1.0.41"
#define HAVE_ALLOCA_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SNPRINTF 1
#define HAVE_SSIZE_T 1
#define HAVE_STRCASECMP 1
#define WABT_BIG_ENDIAN 0
#define COMPILER_IS_CLANG 1
#define SIZEOF_SIZE_T 8

#if HAVE_ALLOCA_H
#include <alloca.h>
#elif COMPILER_IS_MSVC
#include <malloc.h>
#define alloca _alloca
#elif defined(__MINGW32__)
#include <malloc.h>
#endif

#if COMPILER_IS_CLANG || COMPILER_IS_GNU
#define WABT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define WABT_LIKELY(x)   __builtin_expect(!!(x), 1)
#define WABT_VECTORCALL
#if __MINGW32__
#define WABT_PRINTF_FORMAT(format_arg, first_arg) \
  __attribute__((format(gnu_printf, (format_arg), (first_arg))))
#else
#define WABT_PRINTF_FORMAT(format_arg, first_arg) \
  __attribute__((format(printf, (format_arg), (first_arg))))
#endif
#ifdef __cplusplus
#define WABT_STATIC_ASSERT(x) static_assert((x), #x)
#else
#define WABT_STATIC_ASSERT(x) _Static_assert((x), #x)
#endif
#elif COMPILER_IS_MSVC
#include <intrin.h>
#include <string.h>
#define WABT_STATIC_ASSERT(x) _STATIC_ASSERT(x)
#define WABT_UNLIKELY(x) (x)
#define WABT_LIKELY(x) (x)
#define WABT_PRINTF_FORMAT(format_arg, first_arg)
#define WABT_VECTORCALL __vectorcall
#else
#error unknown compiler
#endif

#define WABT_UNREACHABLE abort()

#ifdef __cplusplus
namespace wabt {
#if COMPILER_IS_CLANG || COMPILER_IS_GNU
inline int Clz(unsigned x) { return x ? __builtin_clz(x) : sizeof(x) * 8; }
inline int Clz(unsigned long x) { return x ? __builtin_clzl(x) : sizeof(x) * 8; }
inline int Clz(unsigned long long x) { return x ? __builtin_clzll(x) : sizeof(x) * 8; }
inline int Ctz(unsigned x) { return x ? __builtin_ctz(x) : sizeof(x) * 8; }
inline int Ctz(unsigned long x) { return x ? __builtin_ctzl(x) : sizeof(x) * 8; }
inline int Ctz(unsigned long long x) { return x ? __builtin_ctzll(x) : sizeof(x) * 8; }
inline int Popcount(uint8_t x) { return __builtin_popcount(x); }
inline int Popcount(unsigned x) { return __builtin_popcount(x); }
inline int Popcount(unsigned long x) { return __builtin_popcountl(x); }
inline int Popcount(unsigned long long x) { return __builtin_popcountll(x); }
#else
#error unknown compiler
#endif
}  // namespace wabt

#if COMPILER_IS_MSVC
#if SIZEOF_SIZE_T == 4
#define PRIzd "d"
#define PRIzx "x"
#elif SIZEOF_SIZE_T == 8
#define PRIzd "I64d"
#define PRIzx "I64x"
#endif
#else
#define PRIzd "zd"
#define PRIzx "zx"
#endif

#if HAVE_SNPRINTF
#define wabt_snprintf snprintf
#endif
#define wabt_vsnprintf vsnprintf

#if !HAVE_SSIZE_T
typedef long ssize_t;
#endif

double wabt_convert_uint64_to_double(uint64_t x);
float  wabt_convert_uint64_to_float(uint64_t x);
double wabt_convert_int64_to_double(int64_t x);
float  wabt_convert_int64_to_float(int64_t x);

#endif  // __cplusplus

#endif /* WABT_CONFIG_H_ */
