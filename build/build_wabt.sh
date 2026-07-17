#!/bin/bash
# build/build_wabt.sh — build the wabt "wat2wasm" tool to wasm32,
# producing obj/wasm/wat2wasm.wasm which is embedded into the kernel as
# an "in-kernel WAT compiler" (run by kernel.wat2wasm_native).
#
# Toolchain: wasi-sdk's clang targeting wasm32-wasi. Output is wasm MVP
# (no bulk-memory, sign-ext, mutable-globals, etc.) so that wasm3 0.5.0
# can load and execute it.
#
# wasi-sdk locations, in priority order:
#   1. /opt/wasi-sdk/bin/clang++           (native WSL install)
#   2. /usr/lib/wasi-sdk/bin/clang++        (apt-installed wasi-sdk)
#   3. /mnt/c/bin/wasi/bin/clang++         (Windows install via WSL)
#   4. clang++                              (PATH fallback — unlikely to work
#                                            for wasm targets)
set -e
. "$(dirname "$0")/lib.sh"

# ---- Locate wasi-sdk -----------------------------------------------------
WASI_CC=""
for candidate in \
    /opt/wasi-sdk/bin/clang++ \
    /usr/lib/wasi-sdk/bin/clang++ \
    /usr/share/wasi-sdk/bin/clang++ \
    /mnt/c/bin/wasi/bin/clang++ \
    /c/bin/wasi/bin/clang++ \
    /mnt/c/bin/wasi/bin/clang++.exe \
    /c/bin/wasi/bin/clang++.exe
do
    if [ -f "$candidate" ]; then
        WASI_CC="$candidate"
        break
    fi
done
if [ -z "$WASI_CC" ]; then
    WASI_CC="clang++"
    warn "wasi-sdk not found at known locations — falling back to '$WASI_CC' on PATH"
    warn "If your build fails, install wasi-sdk or set WASI_CC explicitly."
fi
log "Using clang++: $WASI_CC"

WABT_DIR="src/external/wabt-1.0.41"

# Generate wasi-target config.h for wabt (matches upstream's autoconf output
# for a clang/wasi target).
mkdir -p "$WABT_OUT_DIR/wabt"
cat > "$WABT_OUT_DIR/wabt/config.h" << 'CONFIG_H'
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

# ---- Build flags ----------------------------------------------------------
# MVP-targeting flags. NO -flto (LTO bakes wasi-libc's non-MVP features
# into our code; without LTO, libc functions stay as extern calls).
WABT_CFLAGS="\
    --target=wasm32-wasi \
    -mno-bulk-memory -mno-sign-ext -mno-mutable-globals \
    -mno-nontrapping-fptoint -mno-simd128 -mno-reference-types \
    -Os -fno-exceptions \
    -I $WABT_DIR/include \
    -I $WABT_OUT_DIR \
    -I $WABT_DIR/third_party/picosha2 \
    -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-unknown-attributes \
"
WABT_LDFLAGS="\
    --target=wasm32-wasi \
    -mno-bulk-memory -mno-sign-ext -mno-mutable-globals \
    -mno-nontrapping-fptoint -mno-simd128 -mno-reference-types \
    -Os -fno-exceptions \
    -Wl,--strip-all -Wl,--no-entry -Wl,--export=_start -Wl,--compress-relocations \
"

# Flags-change detection for wabt
FLAGS_FILE="$WABT_OUT_DIR/.wabt_build_flags"
CURRENT_WABT_FLAGS="$WABT_CFLAGS|$WABT_LDFLAGS"
if [ -f "$FLAGS_FILE" ]; then
    OLD_FLAGS=$(cat "$FLAGS_FILE")
    if [ "$OLD_FLAGS" != "$CURRENT_WABT_FLAGS" ]; then
        warn "wabt build flags changed — clearing wabt .o cache"
        rm -f "$WABT_OUT_DIR"/*.o
    fi
else
    log "Initializing wabt flag tracking"
    rm -f "$WABT_OUT_DIR"/*.o
fi
echo "$CURRENT_WABT_FLAGS" > "$FLAGS_FILE"

# ---- wabt sources --------------------------------------------------------
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

# ---- Custom memcpy/memset/memmove/memcmp ---------------------------------
# Override wasi-libc's bulk-memory versions so we don't emit
# memory.copy/memory.fill opcodes.
cat > "$WABT_OUT_DIR/wasm_memcpy.cc" << 'WASM_MEMCPY'
#include <stddef.h>
extern "C" {
void* memcpy(void* dest, const void* src, size_t n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    for (size_t i = 0; i < n; i++) { d[i] = s[i]; }
    return dest;
}
void* memset(void* s, int c, size_t n) {
    char* p = (char*)s;
    for (size_t i = 0; i < n; i++) { p[i] = (char)c; }
    return s;
}
void* memmove(void* dest, const void* src, size_t n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) { d[i] = s[i]; }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) { d[i - 1] = s[i - 1]; }
    }
    return dest;
}
int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) { return p1[i] - p2[i]; }
    }
    return 0;
}
}
WASM_MEMCPY

# ---- Compile wasm_memcpy.cc ----------------------------------------------
log "Building wabt memcpy shim"
MEMCPY_OBJ="$WABT_OUT_DIR/wasm_memcpy.o"
$WASI_CC -c $WABT_CFLAGS "$WABT_OUT_DIR/wasm_memcpy.cc" -o "$MEMCPY_OBJ" 2>&1

# ---- Compile wabt library ------------------------------------------------
log "Building wabt library"
OBJS="$MEMCPY_OBJ"
for src in "${WABT_SOURCES[@]}"; do
    basename=$(echo "$src" | sed 's|/|_|g' | sed 's|\.cc$|.o|' | sed 's|\.c$|.o|')
    obj="$WABT_OUT_DIR/$basename"
    if [ ! -f "$obj" ] || [ "$WABT_DIR/$src" -nt "$obj" ]; then
        log "  CC  $src"
        $WASI_CC -c $WABT_CFLAGS "$WABT_DIR/$src" -o "$obj" 2>&1
    fi
    OBJS="$OBJS $obj"
done

# ---- wat2wasm tool -------------------------------------------------------
log "Building wat2wasm tool"
TOOL_OBJ="$WABT_OUT_DIR/tools_wat2wasm.o"
$WASI_CC -c $WABT_CFLAGS "$WABT_DIR/src/tools/wat2wasm.cc" -o "$TOOL_OBJ" 2>&1

# ---- Link → wat2wasm.wasm ------------------------------------------------
log "Linking wat2wasm.wasm"
$WASI_CC \
    $WABT_LDFLAGS \
    "$TOOL_OBJ" $OBJS \
    -o "$WASM_DIR/wat2wasm.wasm" 2>&1

# ---- Strip custom sections ----------------------------------------------
if command -v wasm-strip >/dev/null 2>&1; then
    log "Stripping debug/target_features sections"
    wasm-strip "$WASM_DIR/wat2wasm.wasm"
elif [ -x "/mnt/c/bin/wabt/bin/wasm-strip" ]; then
    /mnt/c/bin/wabt/bin/wasm-strip "$WASM_DIR/wat2wasm.wasm"
fi

WASM_SIZE=$(stat -c%s "$WASM_DIR/wat2wasm.wasm" 2>/dev/null \
          || stat -f%z "$WASM_DIR/wat2wasm.wasm" 2>/dev/null)
ok "wat2wasm.wasm: $WASM_SIZE bytes"
