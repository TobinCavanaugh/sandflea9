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
[ "$ISO_DIR/disk.img"            -nt sandfleaOS.iso ] && SHOULD_REBUILD=1
[ "$ISO_DIR/limine.conf"          -nt sandfleaOS.iso ] && SHOULD_REBUILD=1
[ "$LIMINE_DIR/limine-uefi-cd.bin" -nt sandfleaOS.iso ] && SHOULD_REBUILD=1

if [ $SHOULD_REBUILD -eq 1 ]; then
    log "Rebuilding ISO (inputs changed)"
    mkdir -p "$ISO_DIR/EFI/BOOT"
    cp "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_DIR/"
    cp "$LIMINE_DIR/BOOTX64.EFI" "$ISO_DIR/EFI/BOOT/"
    [ -f "$LIMINE_DIR/BOOTIA32.EFI" ] && cp "$LIMINE_DIR/BOOTIA32.EFI" "$ISO_DIR/EFI/BOOT/"
    [ -f "$LIMINE_DIR/limine-bios-cd.bin" ] && cp "$LIMINE_DIR/limine-bios-cd.bin" "$ISO_DIR/"
    [ -f "$LIMINE_DIR/limine-bios.sys" ] && cp "$LIMINE_DIR/limine-bios.sys" "$ISO_DIR/"

    if [ -f "$ISO_DIR/limine-bios-cd.bin" ]; then
        xorriso -as mkisofs \
            -b limine-bios-cd.bin \
            -no-emul-boot -boot-load-size 4 -boot-info-table \
            --efi-boot limine-uefi-cd.bin \
            -efi-boot-part --efi-boot-image --protective-msdos-label \
            -o sandfleaOS.iso "$ISO_DIR"
    else
        xorriso -as mkisofs \
            --efi-boot limine-uefi-cd.bin \
            -efi-boot-part --efi-boot-image --protective-msdos-label \
            -o sandfleaOS.iso "$ISO_DIR"
    fi
else
    log "sandfleaOS.iso: input unchanged, preserving mtime"
fi

ok "sandfleaOS.iso: $(stat -c%s sandfleaOS.iso 2>/dev/null || echo "?") bytes"
