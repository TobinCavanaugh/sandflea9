#!/bin/bash
# build/build_filesystem.sh -- build the ext2 filesystem images from drives/
#
# Scans all directories under drives/*/ and uses their .driveinfo configuration:
#   label: Volume label (defaults to drive name, e.g. A, data)
#   size: Image size (defaults to 64M)
#   mode: volatile | persistent (defaults to volatile for A, persistent for others)
#   output: Output image path (defaults to iso_root/disk.img for A, data.img for B)
#
# Volatile drives are formatted fresh on every build.
# Persistent drives are created once and preserved across builds.
# Uses `mke2fs -F -F -d` for single-pass filesystem generation (<0.3s).
#
# Safe to run in parallel with build_wabt.sh (no shared resources).
set -e
. "$(dirname "$0")/lib.sh"

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

if [ ! -d "drives" ]; then
    warn "No drives/ directory found; skipping filesystem creation."
    exit 0
fi

for drive_dir in drives/*/; do
    [ -d "$drive_dir" ] || continue
    drive_name="$(basename "$drive_dir")"

    # Default settings based on drive name
    label="$drive_name"
    size="64M"
    if [ "$drive_name" = "A" ]; then
        mode="volatile"
        output="$ISO_DIR/disk.img"
    elif [ "$drive_name" = "B" ]; then
        label="data"
        mode="persistent"
        output="data.img"
    else
        mode="persistent"
        output="${drive_name}.img"
    fi

    # Parse .driveinfo if present
    driveinfo_file="$drive_dir/.driveinfo"
    if [ -f "$driveinfo_file" ]; then
        while IFS='=' read -r key val || [ -n "$key" ]; do
            # Trim whitespace and carriage returns
            key="$(echo "$key" | tr -d '\r ')"
            val="$(echo "$val" | tr -d '\r')"
            # Strip leading/trailing whitespace from value
            val="${val#"${val%%[! ]*}"}"
            val="${val%"${val##*[! ]}"}"
            case "$key" in
                label)  label="$val" ;;
                size)   size="$val" ;;
                mode)   mode="$val" ;;
                output) output="$val" ;;
            esac
        done < "$driveinfo_file"
    fi

    # If persistent and image already exists, skip to preserve data
    if [ "$mode" = "persistent" ] && [ -f "$output" ]; then
        log "Drive $drive_name ($output) already exists; preserved"
        continue
    fi

    log "Building Drive $drive_name ($output, $size, ext2, label='$label', mode=$mode)"

    STAGE_DIR="/tmp/sandflea_stage_${drive_name}_$$"
    rm -rf "$STAGE_DIR"
    mkdir -p "$STAGE_DIR"

    # Copy drive contents to staging area and remove metadata
    cp -r "$drive_dir". "$STAGE_DIR/"
    rm -f "$STAGE_DIR/.driveinfo"

    # For boot drive A, auto-inject wat scripts and compiled user wasm binaries
    if [ "$drive_name" = "A" ]; then
        if [ -d "src/wasm/wat" ]; then
            for wat_file in src/wasm/wat/*.wat; do
                [ -f "$wat_file" ] || continue
                cp "$wat_file" "$STAGE_DIR/"
            done
        fi

        if [ -d "$WASM_DIR" ]; then
            for wasm_path in "$WASM_DIR"/*.wasm; do
                [ -f "$wasm_path" ] || continue
                [ "$(basename "$wasm_path")" = "wat2wasm.wasm" ] && continue
                cp "$wasm_path" "$STAGE_DIR/"
            done
            if [ -f "$WASM_DIR/file_test.wasm" ]; then
                cp "$WASM_DIR/file_test.wasm" "$STAGE_DIR/w"
            fi
        fi
    fi

    mkdir -p "$(dirname "$output")"
    truncate -s "$size" "$output"
    mkfs.ext2 -F -F -L "$label" -d "$STAGE_DIR" "$output"

    rm -rf "$STAGE_DIR"
    ok "Drive $drive_name -> $output ($(stat -c%s "$output" 2>/dev/null || stat -f%z "$output" 2>/dev/null) bytes)"
done
