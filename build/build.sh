#!/bin/bash
# build/build.sh — top-level sandfleaOS build orchestrator.
# Run from project root: `bash build/build.sh` (or via wb.bat from Windows).
set -e

# Always cd to project root (parent of build/ dir).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

if [ -t 1 ]; then
    _CYA='\033[0;36m'; _GRN='\033[0;32m'; _NC='\033[0m'
else
    _CYA=''; _GRN=''; _NC=''
fi

step() { printf "${_CYA}[build]${_NC} %s\n" "$*"; }
done_() { printf "${_GRN}[build]${_NC} %s\n" "$*"; }

step "sandfleaOS build — project root: $PROJECT_ROOT"

step "step 1/4 — build WABT library → obj/wasm/wat2wasm.wasm"
bash "$SCRIPT_DIR/build_wabt.sh"

step "step 2/4 — build kernel .elf"
bash "$SCRIPT_DIR/build_kernel.sh"

step "step 3/4 — build filesystem images (disk.img, data.img)"
bash "$SCRIPT_DIR/build_filesystem.sh"

step "step 4/4 — package UEFI ISO"
bash "$SCRIPT_DIR/build_iso.sh"

done_ "build complete — bootable sandfleaOS.iso ready"
done_ "use wr.bat (or 'wsl bash build/wr.sh' on a Linux box) to launch qemu"
