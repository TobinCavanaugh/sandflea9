#!/bin/bash
set -e
# Build wabt to WASM using wasi-sdk (32.0 at /c/bin/wasi/)
# Output: obj/wasm/wat2wasm.wasm (auto-included in the disk image by build.sh)
#
# NOTE: wasm3 0.5.0 only supports WASM MVP. wasi-sdk 22+ produces non-MVP
# instructions (bulk-memory, reference-types, sign-ext, etc.) by default.
# The -mno-* flags don't work on the wasm target. The workaround:
#   1. Build WITHOUT -flto (LTO bakes wasi-libc's non-MVP features into our code)
#   2. wasi-libc's pre-compiled .a uses a few non-MVP instructions internally,
#      but they're in rarely-used code paths
#   3. Use wasm-strip to remove custom sections (debug info, target_features)

if [ -f "/mnt/c/bin/wasi/bin/clang++" ]; then
    WASI_CC="/mnt/c/bin/wasi/bin/clang++"
elif [ -f "/mnt/c/bin/wasi/bin/clang++.exe" ]; then
    WASI_CC="/mnt/c/bin/wasi/bin/clang++.exe"
elif [ -f "/c/bin/wasi/bin/clang++" ]; then
    WASI_CC="/c/bin/wasi/bin/clang++"
else
    WASI_CC="clang++"
fi
WABT_DIR="src/external/wabt-1.0.41"
BUILD_DIR="build/wabt_wasi"
OUT_DIR="obj/wasm"

mkdir -p "$BUILD_DIR"
mkdir -p "$OUT_DIR"

# Generate a complete config.h for wasm32-wasi target.
mkdir -p "$BUILD_DIR/wabt"
cat > "$BUILD_DIR/wabt/config.h" << 'CONFIG_H'
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
CONFIG_H

echo "--- Generated config.h ---"

# Build flags:
# -target wasm32-wasi (MVP; wasip1 would pull in preview1 features)
# -Os: optimize for size
# NO -flto: LTO bakes wasi-libc's non-MVP features (memory.copy, etc.)
#           into our code. Without LTO, libc functions are external calls.
# -Wl,--strip-all: remove debug sections and custom sections
CFLAGS="\
    --target=wasm32-wasi \
    -mno-bulk-memory \
    -mno-sign-ext \
    -mno-mutable-globals \
    -mno-nontrapping-fptoint \
    -mno-simd128 \
    -mno-reference-types \
    -Os \
    -fno-exceptions \
    -I $WABT_DIR/include \
    -I $BUILD_DIR \
    -I $WABT_DIR/third_party/picosha2 \
    -Wno-unused-parameter \
    -Wno-implicit-fallthrough \
    -Wno-unknown-attributes \
"

LDFLAGS="\
    --target=wasm32-wasi \
    -mno-bulk-memory \
    -mno-sign-ext \
    -mno-mutable-globals \
    -mno-nontrapping-fptoint \
    -mno-simd128 \
    -mno-reference-types \
    -Os \
    -fno-exceptions \
    -Wl,--strip-all \
    -Wl,--no-entry \
    -Wl,--export=_start \
    -Wl,--compress-relocations \
"

# Flags change detection for WABT
FLAGS_FILE="$BUILD_DIR/.wabt_build_flags"
CURRENT_FLAGS="$CFLAGS|$LDFLAGS"
if [ -f "$FLAGS_FILE" ]; then
    OLD_FLAGS=$(cat "$FLAGS_FILE")
    if [ "$OLD_FLAGS" != "$CURRENT_FLAGS" ]; then
        echo "--- WABT build flags changed, forcing rebuild of object files ---"
        rm -f "$BUILD_DIR"/*.o
    fi
else
    echo "--- Initializing flag tracking, clearing old WABT objects ---"
    rm -f "$BUILD_DIR"/*.o
fi
echo "$CURRENT_FLAGS" > "$FLAGS_FILE"

# Wabt library source files
WABT_SOURCES=(
    src/apply-names.cc
    src/binary-reader-ir.cc
    src/binary-reader-logging.cc
    src/binary-reader.cc
    src/binary-writer-spec.cc
    src/binary-writer.cc
    src/binary.cc
    src/binding-hash.cc
    src/color.cc
    src/common.cc
    src/config.cc
    src/error-formatter.cc
    src/expr-visitor.cc
    src/feature.cc
    src/filenames.cc
    src/generate-names.cc
    src/ir-util.cc
    src/ir.cc
    src/leb128.cc
    src/lexer-source-line-finder.cc
    src/lexer-source.cc
    src/literal.cc
    src/opcode-code-table.c
    src/opcode.cc
    src/option-parser.cc
    src/resolve-names.cc
    src/sha256.cc
    src/shared-validator.cc
    src/stream.cc
    src/token.cc
    src/tracing.cc
    src/type-checker.cc
    src/utf8.cc
    src/validator.cc
    src/wast-lexer.cc
    src/wast-parser.cc
    src/wat-writer.cc
)

# Generate custom memcpy/memset to override wasi-libc's bulk-memory versions
cat > "$BUILD_DIR/wasm_memcpy.cc" << 'WASM_MEMCPY'
#include <stddef.h>
extern "C" {
void* memcpy(void* dest, const void* src, size_t n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}
void* memset(void* s, int c, size_t n) {
    char* p = (char*)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (char)c;
    }
    return s;
}
void* memmove(void* dest, const void* src, size_t n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}
int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}
}
WASM_MEMCPY

echo "--- Compiling custom wasm_memcpy ---"
MEMCPY_OBJ="$BUILD_DIR/wasm_memcpy.o"
$WASI_CC -c $CFLAGS "$BUILD_DIR/wasm_memcpy.cc" -o "$MEMCPY_OBJ" 2>&1

echo "--- Compiling wabt library ---"
OBJS="$MEMCPY_OBJ"
for src in "${WABT_SOURCES[@]}"; do
    basename=$(echo "$src" | sed 's|/|_|g' | sed 's|\.cc$|.o|' | sed 's|\.c$|.o|')
    obj="$BUILD_DIR/$basename"
    if [ ! -f "$obj" ] || [ "$WABT_DIR/$src" -nt "$obj" ]; then
        echo "  CC  $src"
        $WASI_CC -c $CFLAGS "$WABT_DIR/$src" -o "$obj" 2>&1
    fi
    OBJS="$OBJS $obj"
done

echo "--- Compiling wat2wasm tool ---"
TOOL_OBJ="$BUILD_DIR/tools_wat2wasm.o"
$WASI_CC -c $CFLAGS "$WABT_DIR/src/tools/wat2wasm.cc" -o "$TOOL_OBJ" 2>&1

echo "--- Linking wat2wasm.wasm ---"
$WASI_CC \
    $LDFLAGS \
    $TOOL_OBJ $OBJS \
    -o "$OUT_DIR/wat2wasm.wasm" 2>&1

if command -v wasm-strip >/dev/null 2>&1; then
    echo "--- Stripping wat2wasm.wasm ---"
    wasm-strip "$OUT_DIR/wat2wasm.wasm"
elif [ -f "/mnt/c/bin/wabt/bin/wasm-strip" ]; then
    echo "--- Stripping wat2wasm.wasm via Windows path mount ---"
    /mnt/c/bin/wabt/bin/wasm-strip "$OUT_DIR/wat2wasm.wasm"
fi

# Check size
WASM_SIZE=$(stat -c%s "$OUT_DIR/wat2wasm.wasm" 2>/dev/null || stat -f%z "$OUT_DIR/wat2wasm.wasm" 2>/dev/null)
echo "--- Done: $OUT_DIR/wat2wasm.wasm ($WASM_SIZE bytes) ---"
echo "Run './build.sh' to include it in the disk image."
