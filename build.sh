#!/bin/bash
set -e

# =============================================================================
# CONFIGURATION
# =============================================================================

C_SOURCES=(
    "src/kernel/arith64.c"
    "src/kernel/kern_asmstubs.c"
    "src/kernel/kern_interrupts.c"
    "src/kernel/kern_serial.c"
    "src/kernel/kern_keyboard.c"
    "src/kernel/kern_screen.c"
    "src/kernel/kern_vmm.c"
    "src/kernel/kern_mem.c"
    "src/kernel/main.c"
)

ASM_SOURCES=(
    "src/arch/kernel.asm"
    "src/arch/isrs.asm"
#    "src/arch/irqs.asm"
)

GCC_INCLUDE=$(gcc -print-file-name=include)

CFLAGS="-m64 -c -O0 -ffreestanding -nostdlib -g -nostdinc -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -I $GCC_INCLUDE"
ASMFLAGS="-f elf64 -g"
LDFLAGS="-m elf_x86_64 -T link.ld -build-id=none -z max-page-size=0x1000"

# =============================================================================
# LIMINE SETUP
# =============================================================================

if [ ! -d "limine" ]; then
    echo "Downloading Limine..."
    git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
    # We don't even need to build the 'limine' tool anymore because
    # we aren't doing the bios-install patching step!
fi

# =============================================================================
# BUILD PROCESS
# =============================================================================

if [ ! -d "obj" ]; then mkdir obj; fi
if [ ! -d "iso_root" ]; then mkdir iso_root; fi

rm -f iso_root/kernel.elf
rm -f sandfleaOS.iso

LINK_LIST=""

echo "--- Compiling ---"

for src in "${ASM_SOURCES[@]}"; do
    filename=$(basename "$src" .asm)
    obj_path="obj/${filename}.o"
    nasm $ASMFLAGS "$src" -o "$obj_path"
    LINK_LIST="$LINK_LIST $obj_path"
done

for src in "${C_SOURCES[@]}"; do
    filename=$(basename "$src" .c)
    obj_path="obj/${filename}.o"
    gcc $CFLAGS "$src" -o "$obj_path"
    LINK_LIST="$LINK_LIST $obj_path"
done

echo "--- Processing Font ---"
# Convert the binary font file into an ELF object file
objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
    src/blob/regularfont.sfn obj/font.o
LINK_LIST="$LINK_LIST obj/font.o"

echo "--- Linking ---"
ld $LDFLAGS -o "iso_root/kernel.elf" $LINK_LIST

echo "--- Packaging UEFI ISO ---"

# 1. Copy ONLY the UEFI bootloader binary
cp limine/limine-uefi-cd.bin iso_root/

# 2. Generate Pure UEFI ISO
# Note: We removed '-b' (legacy boot) and 'bios-install'
xorriso -as mkisofs \
        --efi-boot limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        -o sandfleaOS.iso iso_root

echo "--- Done ---"
echo "NOTE: To run this in QEMU, you now need OVMF (UEFI Firmware)."
echo "Run: qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom sandfleaOS.iso"