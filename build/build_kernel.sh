#!/bin/bash
# build/build_kernel.sh — compile + link the sandfleaOS kernel.
set -e
. "$(dirname "$0")/lib.sh"

# ---- Limine bootloader --------------------------------------------------
# We don't build `limine` itself; we only need the pre-built UEFI image.
if [ ! -d "$LIMINE_DIR" ]; then
    log "Cloning limine (v8.x prebuilt)"
    git clone https://github.com/limine-bootloader/limine.git \
        --branch=v8.x-binary --depth=1
fi

# ---- Output dirs ---------------------------------------------------------
mkdir -p "$OBJ_DIR" "$WASM_DIR" "$ISO_DIR"
mkdir -p "$WABT_OUT_DIR" "$WASM2C_OUT_DIR"

# ---- wasm2c: convert wat2wasm.wasm → native C -----------------------------
# The kernel embeds the wat2wasm tool (compiled to wasm in step 1) by
# converting it at build time to native C via `wasm2c`. The resulting C file
# is checked in (tracked) so the kernel can compile even when wasm2c isn't
# present; regular builds regenerate it when obj/wasm/wat2wasm.wasm changes.
# Lookups follow the same convention as build_wabt.sh: prefer WSL-native
# installs (`apt install wabt`, `/opt/wabt`, NixOS) and fall back to the
# Windows cross-mount for users who haven't migrated yet.
WASM2C_BIN="${WASM2C_BIN:-}"
if [ -z "$WASM2C_BIN" ]; then
    for c in \
        /opt/wabt/bin/wasm2c \
        /usr/bin/wasm2c \
        /usr/local/bin/wasm2c \
        /nix/var/nix/profiles/default/bin/wasm2c \
        /mnt/c/bin/wabt/bin/wasm2c \
        /c/bin/wabt/bin/wasm2c; do
        if [ -x "$c" ]; then WASM2C_BIN="$c"; break; fi
    done
fi
if [ -z "$WASM2C_BIN" ]; then
    err "wasm2c not found."
    err "  Install on WSL:   apt install wabt  (or  nix-env -i wabt )"
    err "  Or on Windows:    C:\bin\abt\bin\wasm2c.exe"
    exit 1
fi
log "Using wasm2c: $WASM2C_BIN"
WASM2C_WASM="$WASM_DIR/wat2wasm.wasm"
WASM2C_OUT_C="src/external/wasm2c_wat2wasm.c"
WASM2C_OUT_H="src/external/wasm2c_wat2wasm.h"

if [ -x "$WASM2C_BIN" ] && [ -f "$WASM2C_WASM" ]; then
    # Re-run wasm2c when the .wasm input changed since the intermediate
    # was last generated (cheap — sub-second).
    if [ ! -f "$WASM2C_OUT_DIR/wat2wasm.c" ] || \
       [ "$WASM2C_WASM" -nt "$WASM2C_OUT_DIR/wat2wasm.c" ]; then
        log "wasm2c: $WASM2C_WASM → $WASM2C_OUT_DIR/wat2wasm.c"
        "$WASM2C_BIN" "$WASM2C_WASM" -o "$WASM2C_OUT_DIR/wat2wasm.c" 2>&1
    fi

    # Apples-to-apples preparation: keep a sibling "prepared" intermediate
    # that has the include-name sed applied. We MUST do this in the
    # intermediate (not just the in-tree) before the cmp below — otherwise
    # raw-vs-sed-edited always differs and the cp always runs, which is
    # exactly the cascading-rebuild bug this whole step exists to avoid.
    WASM2C_PREPARED="$WASM2C_OUT_DIR/wat2wasm_prepared.c"
    if [ ! -f "$WASM2C_PREPARED" ] || \
       [ "$WASM2C_OUT_DIR/wat2wasm.c" -nt "$WASM2C_PREPARED" ]; then
        cp "$WASM2C_OUT_DIR/wat2wasm.c" "$WASM2C_PREPARED"
        # Fixes include: rename to our adapted header; force the in-tree
        # wasm-rt.h (kernel-clean, no pthread deps).
        sed -i 's|#include "wat2wasm.h"|#include "wasm2c_wat2wasm.h"|' "$WASM2C_PREPARED"
    fi

    # Refresh in-tree src only if the prepared content actually differs.
    # Otherwise, preserve the mtime so the per-source `-nt` check below
    # can correctly skip the 30-second wasm2c_wat2wasm.c compile on no-op
    # incremental builds.
    if ! cmp -s "$WASM2C_PREPARED" "$WASM2C_OUT_C"; then
        log "wasm2c: new content — refreshing $WASM2C_OUT_C"
        cp "$WASM2C_PREPARED" "$WASM2C_OUT_C"
        cp "$WASM2C_OUT_DIR/wat2wasm.h"  "$WASM2C_OUT_H"
        sed -i 's|#include ".*wabt.*/wasm-rt.h"|#include "wasm-rt.h"|' "$WASM2C_OUT_H"
        sed -i 's|//#include "wasm-rt.h"|#include "wasm-rt.h"|' "$WASM2C_OUT_H"
        ok "wasm2c: $(wc -c < "$WASM2C_OUT_C") bytes C, $(wc -c < "$WASM2C_OUT_H") bytes H"
    else
        log "wasm2c: byte-identical output — preserving $WASM2C_OUT_C mtime"
    fi
else
    if [ ! -f "$WASM2C_OUT_C" ]; then
        err "wasm2c not found at $WASM2C_BIN and no cached $WASM2C_OUT_C"
        err "  Either install wabt (Windows at C:\bin\wabt\ or apt install wabt)"
        err "  or commit a cached $WASM2C_OUT_C to the repo."
        exit 1
    fi
    warn "Using cached wasm2c output (wasm2c binary unavailable or .wasm unchanged)"
fi

# ---- Remove stale artifacts -----------------------------------------------
rm -f "$ISO_DIR/kernel.elf" sandfleaOS.iso

# ---- Flags change detection ----------------------------------------------
# If CFLAGS, ASMFLAGS, or LDFLAGS changed since last build, force a full
# rebuild — per-file timestamps won't catch flag-only changes.
FLAGS_FILE="$OBJ_DIR/.build_flags"
CURRENT_FLAGS="$CFLAGS|$ASMFLAGS|$LDFLAGS"
if [ -f "$FLAGS_FILE" ]; then
    OLD_FLAGS=$(cat "$FLAGS_FILE")
    if [ "$OLD_FLAGS" != "$CURRENT_FLAGS" ]; then
        warn "Build flags changed → full rebuild forced"
        # -type f: only regular files (exclude obj/wasm/*.wasm subdir).
        find "$OBJ_DIR" -maxdepth 1 -name '*.o' -type f -delete
    fi
fi
echo "$CURRENT_FLAGS" > "$FLAGS_FILE"

LINK_LIST=""

# ---- ASM -----------------------------------------------------------------
log "Compiling ASM"
for src in "${ASM_SOURCES[@]}"; do
    filename=$(basename "$src" .asm)
    obj_path="$OBJ_DIR/${filename}.o"

    if [ ! -f "$obj_path" ] || [ "$src" -nt "$obj_path" ]; then
        log "  AS  $src"
        nasm $ASMFLAGS "$src" -o "$obj_path"
    fi
    LINK_LIST="$LINK_LIST $obj_path"
done

# ---- C -------------------------------------------------------------------
log "Compiling C"
# Track newest header to enforce global rebuild on header change. Includes
# src/external/*.h (e.g. wasm-rt.h) so editing those correctly cascades to
# all dependent .c files. Skip vendored wabt upstream so third-party
# headers can't trigger spurious rebuilds.
NEWEST_HEADER=$(find src/include src/external -type f -name "*.h" \
    -not -path "src/external/wabt-1.0.41/*" \
    -printf '%T@\n' 2>/dev/null | sort -n | tail -1)

# Compile at most PARALLEL_MAX gcc invocations concurrently. nproc on
# WSL gives the host's virtual-core count, which we cap (default 4) to
# keep RSS predictable: each gcc -O3 kernel compile peaks at ~200 MB.
# Override via env: PARALLEL_MAX=N bash build.sh
PARALLEL_MAX="${PARALLEL_MAX:-$(nproc 2>/dev/null || echo 4)}"
log "  parallel jobs: $PARALLEL_MAX"

# `set -e` doesn't propagate exit codes from backgrounded workers, so we
# track every PID and reap individually. A failure aborts the whole
# build; in-flight workers become orphaned briefly but that's fine —
# the build is failing anyway. No shared .o files = no corruption.
PIDS=()
SRCS=()

for src in "${C_SOURCES[@]}"; do
    # Flatten nested path for object name (e.g. src_kernel_kern_ext2.o).
    filename=$(echo "$src" | sed 's|/|_|g' | sed 's|\.c$||')
    obj_path="$OBJ_DIR/${filename}.o"

    SHOULD_REBUILD=0
    if [ ! -f "$obj_path" ] || [ "$src" -nt "$obj_path" ]; then
        SHOULD_REBUILD=1
    else
        OBJ_TIME=$(stat -c %Y "$obj_path")
        if [ "${NEWEST_HEADER%.*}" -gt "$OBJ_TIME" ]; then
            SHOULD_REBUILD=1
        fi
    fi

    LINK_LIST="$LINK_LIST $obj_path"

    if [ $SHOULD_REBUILD -eq 1 ]; then
        log "  CC  $src"
        # Throttle: reap one worker before launching the next once we
        # hit the cap, so #PIDS stays <= PARALLEL_MAX throughout.
        gcc $CFLAGS "$src" -o "$obj_path" &
        PIDS+=($!)
        SRCS+=("$src")
        if [ "${#PIDS[@]}" -ge "$PARALLEL_MAX" ]; then
            reap_pid="${PIDS[0]}"
            reap_src="${SRCS[0]}"
            if wait "$reap_pid"; then
                PIDS=("${PIDS[@]:1}")
                SRCS=("${SRCS[@]:1}")
            else
                err "gcc failed (pid $reap_pid processing $reap_src) -- aborting"
                exit 1
            fi
        fi
    fi
done

# Drain remaining workers.
for i in "${!PIDS[@]}"; do
    if ! wait "${PIDS[$i]}"; then
        err "gcc failed (pid ${PIDS[$i]} processing ${SRCS[$i]}) -- aborting"
        exit 1
    fi
done

# ---- Font blob -----------------------------------------------------------
log "Embedding font"
if [ ! -f "$OBJ_DIR/regularfont.o" ] || \
   [ "src/blob/regularfont.sfn" -nt "$OBJ_DIR/regularfont.o" ]; then
    log "  OBJCOPY src/blob/regularfont.sfn"
    objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        src/blob/regularfont.sfn "$OBJ_DIR/regularfont.o"
fi
LINK_LIST="$LINK_LIST $OBJ_DIR/regularfont.o"

# ---- Link ----------------------------------------------------------------
log "Linking"
ld $LDFLAGS -o "$ISO_DIR/kernel.elf" $LINK_LIST
ok "kernel.elf: $(stat -c%s "$ISO_DIR/kernel.elf") bytes"
