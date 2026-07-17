#!/bin/bash
# build/build_iso.sh — package the bootable UEFI ISO.
# Reads iso_root/kernel.elf + limine/limine-uefi-cd.bin, produces
# sandfleaOS.iso at the project root. The bootloader is shipped pre-built
# from upstream's v8.x-binary branch (no build of `limine` itself).
set -e
. "$(dirname "$0")/lib.sh"

log "Packaging UEFI ISO"
cp "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_DIR/"

xorriso -as mkisofs \
    --efi-boot limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    -o sandfleaOS.iso "$ISO_DIR"

ok "sandfleaOS.iso: $(stat -c%s sandfleaOS.iso 2>/dev/null || echo "?") bytes"
