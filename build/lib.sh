# build/lib.sh — shared helpers for sandfleaOS build scripts.
# Sourced by individual step scripts. NOT executed on its own.

# ---- Logging (color only when stdout is a TTY) -----------------------------
if [ -t 1 ]; then
    _RED='\033[0;31m'; _GRN='\033[0;32m'; _YEL='\033[1;33m'
    _CYA='\033[0;36m'; _NC='\033[0m'
else
    _RED=''; _GRN=''; _YEL=''; _CYA=''; _NC=''
fi

log()  { printf "${_CYA}[%s]${_NC} %s\n" "$(basename "${BASH_SOURCE[1]:-lib}")" "$*"; }
ok()   { printf "${_GRN}[%s] OK${_NC}  %s\n" "$(basename "${BASH_SOURCE[1]:-lib}")" "$*"; }
warn() { printf "${_YEL}[%s] WARN${_NC} %s\n" "$(basename "${BASH_SOURCE[1]:-lib}")" "$*" >&2; }
err()  { printf "${_RED}[%s] ERR${_NC} %s\n" "$(basename "${BASH_SOURCE[1]:-lib}")" "$*" >&2; }

# ---- Canonical output paths (relative to project root) --------------------
: "${OBJ_DIR:=obj}"
: "${ISO_DIR:=iso_root}"
: "${WASM_DIR:=$OBJ_DIR/wasm}"
: "${WABT_OUT_DIR:=build/wabt_wasi}"
: "${WASM2C_OUT_DIR:=build/wasm2c_out}"
: "${LIMINE_DIR:=limine}"

# Kernel C/ASM source list and flags — single source of truth so each step
# script agrees on the same definitions.
C_SOURCES=(
    "src/kernel/kern_asmstubs.c"
    "src/kernel/kern_compositor.c"
    "src/kernel/kern_ext2.c"
    "src/kernel/kern_fs.c"
    "src/kernel/kern_ide.c"
    "src/kernel/kern_ipc.c"
    "src/kernel/kern_interrupts.c"
    "src/kernel/kern_keyboard.c"
    "src/kernel/kern_mem.c"
    "src/kernel/kern_mouse.c"
    "src/kernel/kern_pci.c"
    "src/kernel/kern_sched.c"
    "src/kernel/kern_screen.c"
    "src/kernel/kern_serial.c"
    "src/kernel/kern_terminal.c"
    "src/kernel/kern_profile.c"
    "src/kernel/kern_tests.c"
    "src/kernel/kern_vmm.c"
    "src/kernel/libgcc_stubs.c"
    "src/kernel/main.c"
    "src/kernel/wasm_spawn.c"
    "src/kernel/wat2wasm_wrapper.c"
    "src/external/wasm-rt-impl.c"
    "src/external/wasm2c_wat2wasm.c"
    "src/external/xxhash/xxhash.c"
    "src/kernel/xhci/kern_xhci.c"
    "src/kernel/xhci/kern_xhci_rings.c"
    "src/kernel/xhci/kern_usb_hid.c"
    "src/kernel/ssfn.c"
    "src/kernel/stbsupport.c"
    "src/kernel/x64/idt.c"
    "src/kernel/x64/apic.c"
    "src/util/util_str.c"
    "src/util/util_cmd.c"
    "src/util/str_slice.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_libc.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_meta_wasi.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_tracer.c"
    "src/kernel/wasm3-0.5.0/source/m3_api_uvwasi.c"
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
)

# -I src/include MUST come first.
# `-nostdinc` strips ALL standard include dirs — including GCC's private
# one that ships compiler builtin headers (emmintrin.h, arm_neon.h, ...)
# which src/include/kern_simd.h needs. Re-add only that private dir;
# the kernel's own -I dirs still take precedence over it.
GCC_INCLUDE="$(gcc -print-file-name=include)"
CFLAGS="-m64 -c -O3 -ffreestanding -nostdlib -g -nostdinc -fno-pic -fno-pie \
        -mno-red-zone -mcmodel=kernel -I src/include -I src/external \
        -I$GCC_INCLUDE \
        -I src/kernel/wasm3-0.5.0/source -D d_m3FixedHeap=false \
        -Dd_m3SkipStackCheck=1 -D d_m3HasWASI=1 \
        -Dd_m3VerboseErrorMessages=0 \
        -ffast-math -DNDEBUG"

# Runtime profiling (COM3 serial trace events) is opt-in — it costs ~29% of
# CPU when the timer-ISR scope is active. Enable with:
#   PROFILE=1 bash build/build.sh   (or: set PROFILE=1  then wb.bat)
if [ -n "$PROFILE" ]; then
    CFLAGS="$CFLAGS -DPROFILE_ENABLED=1"
fi
ASMFLAGS="-f elf64 -g"
LDFLAGS="-m elf_x86_64 -T link.ld -build-id=none -z max-page-size=0x1000"
