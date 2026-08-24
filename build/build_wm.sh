#!/bin/bash
# build/build_wm.sh — compile the WASM window manager (wm.c → obj/wasm/wm.wasm)
# Uses the same wasi-sdk clang as build_wabt.sh, but with -nostdlib
# since wm.c is freestanding (imports from kernel host functions only).
set -e
. "$(dirname "$0")/lib.sh"

# ---- Locate wasi-sdk -----------------------------------------------------
WASI_CC=""
for candidate in \
    /opt/wasi-sdk/bin/clang \
    /usr/lib/wasi-sdk/bin/clang \
    /usr/share/wasi-sdk/bin/clang \
    /mnt/c/bin/wasi/bin/clang \
    /c/bin/wasi/bin/clang \
    /mnt/c/bin/wasi/bin/clang.exe \
    /c/bin/wasi/bin/clang.exe
do
    if [ -f "$candidate" ]; then
        WASI_CC="$candidate"
        break
    fi
done
if [ -z "$WASI_CC" ]; then
    WASI_CC="clang"
    warn "wasi-sdk clang not found at known locations — falling back to '$WASI_CC' on PATH"
fi
log "Using clang: $WASI_CC"

WM_SRC="src/wasm/wm/wm.c"
WM_OUT="$WASM_DIR/wm.wasm"

mkdir -p "$WASM_DIR"

# Check if rebuild needed
SHOULD_REBUILD=0
[ ! -f "$WM_OUT" ] && SHOULD_REBUILD=1
[ "$WM_SRC" -nt "$WM_OUT" ] && SHOULD_REBUILD=1

if [ $SHOULD_REBUILD -eq 0 ]; then
    log "wm.wasm up to date — skipping"
    exit 0
fi

log "Compiling wm.wasm"

# Flags: MVP-compatible wasm32 (no bulk-memory, no sign-ext, no simd).
# -nostdlib because wm.c has no libc deps — all imports are kernel host fns.
# -Wl,--no-entry because _start is exported manually via __attribute__((export_name)).
# Strip debug info for size.
$WASI_CC \
    --target=wasm32-wasi \
    -mno-bulk-memory -mno-sign-ext -mno-mutable-globals \
    -mno-nontrapping-fptoint -mno-simd128 -mno-reference-types \
    -Os -fno-exceptions -nostdlib -ffreestanding \
    -Wl,--strip-all -Wl,--allow-undefined -Wl,--export=_start \
    "$WM_SRC" -o "$WM_OUT" 2>&1

WM_SIZE=$(stat -c%s "$WM_OUT" 2>/dev/null || stat -f%z "$WM_OUT" 2>/dev/null)
ok "wm.wasm: $WM_SIZE bytes"