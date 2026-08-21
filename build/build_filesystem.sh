#!/bin/bash
# build/build_filesystem.sh -- build the ext2 filesystem images.
#
# disk.img is content-cached: if every input (blobs + .wat sources +
# obj/wasm/<user>.wasm apps + size/label) is byte-identical to last
# build, the entire mkfs+debugfs cycle is skipped. This is the dominant
# saving on no-op builds (mkfs.ext2 + 2x debugfs is ~3-5s of work).
#
# data.img is preserved-by-existence -- created once on first build,
# never overwritten, so user data survives qemu sessions. No input
# fingerprinting for data.img because there are no input dependencies.
#
#   disk.img -- volatile 32MB boot volume (label='A')
#   data.img -- persistent 64MB data volume (label='data')
#
# Safe to run in parallel with build_wabt.sh (no shared resources).
set -e
. "$(dirname "$0")/lib.sh"

# ---- Fingerprint --------------------------------------------------------
# Stamp is keyed on INPUTS, not on disk.img output bytes. mkfs.ext2's
# filesystem UUID comes from /dev/urandom so two cold builds of the
# same inputs are NOT byte-identical -- but we deliberately don't
# compare outputs. The user-facing FS schema (label + file set) is fully
# determined by inputs, so input-keying is sufficient for cache hits.
disk_img_fingerprint() {
    {
        echo "label=A size=32M"
        for pair in \
            "src/blob/testfile.txt:testfile.txt" \
            "src/blob/a.txt:a.txt" \
            "src/blob/b.txt:b.txt" \
            "src/blob/c.txt:c.txt" \
            "src/blob/utf8.txt:utf8" \
            "src/blob/DOOM1.WAD:DOOM1.WAD" \
            "src/blob/c.txt:folder/a.txt" \
            "src/wasm/wat/hello.wat:hello.wat" \
            "src/wasm/wat/add_test.wat:add_test.wat" \
            "src/wasm/wat/cat.wat:cat.wat" \
            "src/wasm/wat/lsr.wat:lsr.wat" \
            "src/wasm/wat/file_test.wat:file_test.wat"
        do
            src_path="${pair%%:*}"
            dst_path="${pair#*:}"
            [ -f "$src_path" ] && sha256sum "$src_path" \
                | awk -v p="$dst_path" '{print $1"  ->  "p}'
        done
        for w in obj/wasm/*.wasm; do
            [ -f "$w" ] || continue
            bn=$(basename "$w")
            # Skip the in-kernel tool -- it's linked into kernel.elf
            # separately, no need to also drop it into /A/.
            [ "$bn" = "wat2wasm.wasm" ] && continue
            if [ "$bn" = "file_test.wasm" ]; then
                sha256sum "$w" | awk '{print $1"  ->  w"}'
            else
                sha256sum "$w" | awk -v b="$bn" '{print $1"  ->  "b}'
            fi
        done
    } | sha256sum | cut -c1-32
}

FS_STAMP="$OBJ_DIR/.filesystem_stamp"
NEW_DISK_STAMP=$(disk_img_fingerprint)

SHOULD_REBUILD_DISK=0
[ ! -f "$ISO_DIR/disk.img" ] && SHOULD_REBUILD_DISK=1
if [ -f "$FS_STAMP" ]; then
    OLD_DISK=$(sed -n '1p' "$FS_STAMP" 2>/dev/null)
    [ "$OLD_DISK" = "$NEW_DISK_STAMP" ] || SHOULD_REBUILD_DISK=1
else
    SHOULD_REBUILD_DISK=1
fi

# ---- disk.img (boot volume) --------------------------------------------
log "disk.img: fingerprint=$NEW_DISK_STAMP"
if [ $SHOULD_REBUILD_DISK -eq 0 ]; then
    log "disk.img: content-cached, skipping mkfs+debugfs"
else
    log "Creating boot filesystem (disk.img, 32MB, ext2, label='A')"
    # truncate is faster than `dd if=/dev/zero` because the kernel
    # allocates sparse pages; only the ext2 metadata blocks need to be
    # zeroed, which mkfs.ext2 does anyway.
    truncate -s 32M "$ISO_DIR/disk.img"
    mkfs.ext2 -F -L A "$ISO_DIR/disk.img"
    ok "disk.img formatted"

    debugfs -w "$ISO_DIR/disk.img" <<EOF
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

    # Auto-include all user-app .wasm files compiled by wb.bat. Filter
    # out the in-kernel tool so it doesn't redundantly end up in /A/.
    {
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
fi

# ---- data.img (persistent volume) --------------------------------------
# Created only once on first build; never overwritten in subsequent
# builds (preserves user data across qemu sessions). No fingerprinting
# needed because there are no input dependencies -- the file is fully
# determined by its own presence.
if [ ! -f "data.img" ]; then
    log "Creating persistent data drive (data.img, 64MB, ext2, label='data')"
    truncate -s 64M data.img
    mkfs.ext2 -F -L data data.img
    ok "data.img created (preserved across future builds)"
else
    log "data.img already exists; preserved"
fi

# Persist stamp only after disk.img is in its desired state.
mkdir -p "$OBJ_DIR"
echo "$NEW_DISK_STAMP" > "$FS_STAMP"
