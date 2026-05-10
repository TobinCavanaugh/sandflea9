#!/bin/bash
set -e

# =============================================================================
# CONFIGURATION
# =============================================================================

C_SOURCES=(
    "src/kernel/kern_asmstubs.c"
    "src/kernel/kern_ext2.c"
    "src/kernel/kern_fs.c"
    "src/kernel/kern_ide.c"
    "src/kernel/kern_interrupts.c"
    "src/kernel/kern_keyboard.c"
    "src/kernel/kern_mem.c"
    "src/kernel/kern_pci.c"
    "src/kernel/kern_sched.c"
    "src/kernel/kern_screen.c"
    "src/kernel/kern_serial.c"
    "src/kernel/kern_terminal.c"
    "src/kernel/kern_tests.c"
    "src/kernel/kern_vmm.c"
    "src/kernel/libgcc_stubs.c"
    "src/kernel/main.c"
    "src/kernel/ssfn.c"
    "src/kernel/stbsupport.c"
    "src/kernel/x64/idt.c"
    "src/kernel/x64/apic.c"
    "src/util/util_str.c"
    "src/util/util_cmd.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_libc.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_meta_wasi.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_tracer.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_uvwasi.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_wasi.c"
    "src/kernel/wasm3-0.5.0/source/m3_bind.c"
    "src/kernel/wasm3-0.5.0/source/m3_code.c"
    "src/kernel/wasm3-0.5.0/source/m3_compile.c"
    "src/kernel/wasm3-0.5.0/source/m3_core.c"
    "src/kernel/wasm3-0.5.0/source/m3_emit.c"
    "src/kernel/wasm3-0.5.0/source/m3_env.c"
    "src/kernel/wasm3-0.5.0/source/m3_exec.c"
    "src/kernel/wasm3-0.5.0/source/m3_function.c"
    "src/kernel/wasm3-0.5.0/source/m3_info.c"
    "src/kernel/wasm3-0.5.0/source/m3_module.c"
    "src/kernel/wasm3-0.5.0/source/m3_optimize.c"
    "src/kernel/wasm3-0.5.0/source/m3_parse.c"
)

ASM_SOURCES=(
    "src/arch/kernel.asm"
    "src/arch/isrs.asm"
#    "src/arch/irqs.asm"
)

# GCC_INCLUDE=$(gcc -print-file-name=include)

# -I src/include must come FIRST
CFLAGS="-m64 -c -O3 -ffreestanding -nostdlib -g -nostdinc -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -I src/include -I src/kernel/wasm3-0.5.0/source -D d_m3FixedHeap=false -DDEBUG"
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

# Find the newest header file to handle global dependency changes safely
NEWEST_HEADER=$(find src/include -type f -name "*.h" -printf '%T@\n' | sort -n | tail -1)

for src in "${ASM_SOURCES[@]}"; do
    filename=$(basename "$src" .asm)
    obj_path="obj/${filename}.o"
    
    SHOULD_REBUILD=0
    if [ ! -f "$obj_path" ] || [ "$src" -nt "$obj_path" ]; then
        SHOULD_REBUILD=1
    fi

    if [ $SHOULD_REBUILD -eq 1 ]; then
        echo "  AS  $src"
        nasm $ASMFLAGS "$src" -o "$obj_path"
    fi
    LINK_LIST="$LINK_LIST $obj_path"
done

for src in "${C_SOURCES[@]}"; do
    # For nested directories, use a flattened name for the object file
    filename=$(echo "$src" | sed 's/\//_/g' | sed 's/\.c$//')
    obj_path="obj/${filename}.o"

    SHOULD_REBUILD=0
    if [ ! -f "$obj_path" ] || [ "$src" -nt "$obj_path" ]; then
        SHOULD_REBUILD=1
    else
        # Check if any header is newer than the object file
        OBJ_TIME=$(stat -c %Y "$obj_path")
        if [ "${NEWEST_HEADER%.*}" -gt "$OBJ_TIME" ]; then
            SHOULD_REBUILD=1
        fi
    fi

    if [ $SHOULD_REBUILD -eq 1 ]; then
        echo "  CC  $src"
        gcc $CFLAGS "$src" -o "$obj_path"
    fi
    LINK_LIST="$LINK_LIST $obj_path"
done

echo "--- Processing Fonts ---"
# Only re-process font if it changed or object is missing
if [ ! -f "obj/regularfont.o" ] || [ "src/blob/regularfont.sfn" -nt "obj/regularfont.o" ]; then
    echo "  OBJCOPY src/blob/regularfont.sfn"
    objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
        src/blob/regularfont.sfn obj/regularfont.o
fi
LINK_LIST="$LINK_LIST obj/regularfont.o"

echo "--- Linking ---"
ld $LDFLAGS -o "iso_root/kernel.elf" $LINK_LIST

echo "--- Creating Test Filesystem ---"
# 1. Create a 32MB empty file
dd if=/dev/zero of=disk.img bs=1M count=32

# 2. Format it as ext2 (force it to not complain about it being a file)
/usr/sbin/mkfs.ext2 -F disk.img

# (Optional) Copy a test file into it using debugfs so you don't need to mount it
/usr/sbin/debugfs -w disk.img <<EOF
write src/blob/testfile.txt testfile.txt
write src/blob/a.txt a.txt
write src/blob/b.txt b.txt
write src/blob/c.txt c.txt
write src/blob/add_test.wasm add_test.wasm
write src/blob/file_test.wasm file_test.wasm
write src/blob/file_test.wasm w
write src/blob/utf8.txt utf8
mkdir folder
write src/blob/c.txt folder/a.txt
EOF

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
