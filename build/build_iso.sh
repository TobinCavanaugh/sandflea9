#!/bin/bash
# build/build_iso.sh — package the bootable UEFI ISO.
# Reads iso_root/kernel.elf + limine/limine-uefi-cd.bin, produces
# sandfleaOS.iso at the project root. The bootloader is shipped pre-built
# from upstream's v8.x-binary branch (no build of `limine` itself).
set -e
. "$(dirname "$0")/lib.sh"

log "Packaging UEFI ISO"

# Build only if a) the .iso doesn't exist yet, b) the kernel changed, or
# c) the limine bootloader binary changed. Saves ~1-2s on no-op builds
# where both inputs are stable.
SHOULD_REBUILD=0
[ ! -f sandfleaOS.iso ]                              && SHOULD_REBUILD=1
[ "$ISO_DIR/kernel.elf"          -nt sandfleaOS.iso ] && SHOULD_REBUILD=1
[ "$LIMINE_DIR/limine-uefi-cd.bin" -nt sandfleaOS.iso ] && SHOULD_REBUILD=1

if [ $SHOULD_REBUILD -eq 1 ]; then
    log "Rebuilding ISO (kernel.elf or bootloader changed)"
    cp "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_DIR/"
    xorriso -as mkisofs \
        --efi-boot limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        -o sandfleaOS.iso "$ISO_DIR"
else
    log "sandfleaOS.iso: input unchanged, preserving mtime"
fi

ok "sandfleaOS.iso: $(stat -c%s sandfleaOS.iso 2>/dev/null || echo "?") bytes"
