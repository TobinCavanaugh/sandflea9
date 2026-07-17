#!/bin/bash
# build/build_filesystem.sh — build the ext2 filesystem images.
#   disk.img — volatile 32MB boot volume (label='A'), gets regenerated per
#              build so the build never carries stale state across runs.
#   data.img — persistent 64MB data volume (label='data'), preserved across
#              builds so the user's data survives qemu sessions.
set -e
. "$(dirname "$0")/lib.sh"

# ---- disk.img (boot volume) ----------------------------------------------
log "Creating boot filesystem (disk.img, 32MB, ext2, label='A')"
dd if=/dev/zero of=disk.img bs=1M count=32 status=none
mkfs.ext2 -F -L A disk.img
ok "disk.img formatted"

# Seed test files and wasm sources into the boot image.
debugfs -w disk.img <<EOF
write src/blob/testfile.txt testfile.txt
write src/blob/a.txt a.txt
write src/blob/b.txt b.txt
write src/blob/c.txt c.txt
write src/blob/utf8.txt utf8
write src/blob/DOOM1.WAD DOOM1.WAD
mkdir folder
write src/blob/c.txt folder/a.txt
write src/wasm/wat/hello.wat hello.wat
write src/wasm/wat/add_test.wat add_test.wat
write src/wasm/wat/cat.wat cat.wat
write src/wasm/wat/lsr.wat lsr.wat
write src/wasm/wat/file_test.wat file_test.wat
EOF

# Auto-include all .wasm files compiled by wb.bat into the boot image.
{
    for wasm_path in obj/wasm/*.wasm; do
        [ -f "$wasm_path" ] || continue
        wasm_name=$(basename "$wasm_path")
        echo "write $wasm_path $wasm_name"
    done
    if [ -f "obj/wasm/file_test.wasm" ]; then
        echo "write obj/wasm/file_test.wasm w"
    fi
} | debugfs -w disk.img
log "Boot files written to disk.img"

# ---- data.img (persistent volume) ----------------------------------------
# Created only once on first build; never overwritten in subsequent builds.
if [ ! -f "data.img" ]; then
    log "Creating persistent data drive (data.img, 64MB, ext2, label='data')"
    dd if=/dev/zero of=data.img bs=1M count=64 status=none
    mkfs.ext2 -F -L data data.img
    ok "data.img created (preserved across future builds)"
else
    log "data.img already exists; preserved"
fi
