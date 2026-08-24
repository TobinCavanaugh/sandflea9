#!/bin/bash
# build/build.sh — top-level sandfleaOS build orchestrator.
# Run from project root: `bash build/build.sh` (or via wb.bat from Windows).
#
# The pipeline has two independent sub-pipelines that we run concurrently
# in phase 1:
#
#   build_wabt.sh          ─→ build_kernel.sh ─→ build_iso.sh
#      (writes obj/wasm/wat2wasm.wasm)        (reads iso_root/kernel.elf)
#
#   build_filesystem.sh    (independent: only reads obj/wasm/<user>.wasm
#                            produced by wb.bat, never touches kernel.elf)
#
# This gets the build_filesystem step's ~3-5s of dd + mkfs + debugfs work
# to overlap with build_wabt's clang++ compile, instead of waiting until
# the end of the kernel link.

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

step()  { printf "${_CYA}[build]${_NC} %s\n" "$*"; }
done_() { printf "${_GRN}[build]${_NC} %s\n" "$*"; }

step "sandfleaOS build — project root: $PROJECT_ROOT"

# ---- Phase 1: WABT library + filesystem images in parallel --------------
# Two completely-independent step scripts. `wait` reaps both PIDs and we
# capture exit codes individually so the failure message names which
# script failed. set -e is disabled for the wait+check block (re-enabled
# below) so a single failure doesn't tear down the other before it's
# been observed.
step "phase 1/3 — WABT library + WM + filesystem images [parallel]"
set +e
bash "$SCRIPT_DIR/build_wabt.sh" &
PID_WABT=$!
bash "$SCRIPT_DIR/build_wm.sh" &
PID_WM=$!
bash "$SCRIPT_DIR/build_filesystem.sh" &
PID_FS=$!
wait "$PID_WABT"; RC_WABT=$?
wait "$PID_WM";   RC_WM=$?
wait "$PID_FS";   RC_FS=$?
set -e
if [ $RC_WABT -ne 0 ] || [ $RC_WM -ne 0 ] || [ $RC_FS -ne 0 ]; then
    step "ERROR: build_wabt.sh=$RC_WABT build_wm.sh=$RC_WM build_filesystem.sh=$RC_FS"
    exit 1
fi

# ---- Phase 2: kernel build (depends on wat2wasm.wasm from build_wabt) ----
step "phase 2/3 — build kernel .elf"
bash "$SCRIPT_DIR/build_kernel.sh"

# ---- Phase 3: ISO package (depends on iso_root/kernel.elf) --------------
step "phase 3/3 — package UEFI ISO"
bash "$SCRIPT_DIR/build_iso.sh"

done_ "build complete — bootable sandfleaOS.iso ready"
done_ "use wr.bat (or 'wsl bash build/wr.sh' on a Linux box) to launch qemu"
