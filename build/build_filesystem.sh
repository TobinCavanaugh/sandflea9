#!/bin/bash
# build/build_filesystem.sh -- build the ext2 filesystem images.
#
# disk.img is a volatile 32MB boot volume (label='A') rebuilt fresh on
# every build so that runtime modifications from previous QEMU sessions
# do not persist.
#
# data.img is preserved-by-existence -- created once on first build,
# never overwritten, so user data survives qemu sessions.
#
# Safe to run in parallel with build_wabt.sh (no shared resources).
set -e
. "$(dirname "$0")/lib.sh"

# ---- disk.img (boot volume) --------------------------------------------
log "Creating boot filesystem (disk.img, 64MB, ext2, label='A')"
# truncate is faster than `dd if=/dev/zero` because the kernel
# allocates sparse pages; only the ext2 metadata blocks need to be
# zeroed, which mkfs.ext2 does anyway.
truncate -s 64M "$ISO_DIR/disk.img"
mkfs.ext2 -F -L A "$ISO_DIR/disk.img"
ok "disk.img formatted"

{
    echo "write src/blob/testfile.txt testfile.txt"
    echo "write src/blob/a.txt a.txt"
    echo "write src/blob/b.txt b.txt"
    echo "write src/blob/c.txt c.txt"
    echo "write src/blob/utf8.txt utf8"
    echo "write src/blob/DOOM1.WAD DOOM1.WAD"
    echo "write src/blob/quake.wasm quake.wasm"
    echo "mkdir id1"
    [ -f src/blob/id1/pak0.pak ] && echo "write src/blob/id1/pak0.pak id1/pak0.pak"
    [ -f src/blob/id1/pak1.pak ] && echo "write src/blob/id1/pak1.pak id1/pak1.pak"
    echo "mkdir folder"
    echo "write src/blob/c.txt folder/a.txt"
    echo "write src/wasm/wat/hello.wat hello.wat"
    echo "write src/wasm/wat/add_test.wat add_test.wat"
    echo "write src/wasm/wat/cat.wat cat.wat"
    echo "write src/wasm/wat/lsr.wat lsr.wat"
    echo "write src/wasm/wat/file_test.wat file_test.wat"
    echo "write src/wasm/wat/crashme.wat crashme.wat"
    echo "write src/wasm/wat/ipc_receiver.wat ipc_receiver.wat"
    echo "write src/wasm/wat/ipc_sender.wat ipc_sender.wat"

    # Auto-include all user-app .wasm files compiled by wb.bat. Filter
    # out the in-kernel tool so it doesn't redundantly end up in /A/.
    for wasm_path in obj/wasm/*.wasm; do
        [ -f "$wasm_path" ] || continue
        [ "$(basename "$wasm_path")" = "wat2wasm.wasm" ] && continue
        wasm_name=$(basename "$wasm_path")
        echo "write $wasm_path $wasm_name"
    done
    if [ -f "obj/wasm/file_test.wasm" ]; then
        echo "write obj/wasm/file_test.wasm w"
    fi
} | debugfs -w "$ISO_DIR/disk.img"
log "Boot files written to disk.img"
ok "disk.img: $(stat -c%s "$ISO_DIR/disk.img") bytes"

# ---- data.img (persistent volume) --------------------------------------
# Created only once on first build; never overwritten in subsequent
# builds (preserves user data across qemu sessions).
if [ ! -f "data.img" ]; then
    log "Creating persistent data drive (data.img, 64MB, ext2, label='data')"
    truncate -s 64M data.img
    mkfs.ext2 -F -L data data.img
    ok "data.img created (preserved across future builds)"
else
    log "data.img already exists; preserved"
fi
