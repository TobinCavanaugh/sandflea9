#!/bin/bash
# build/clean.sh — remove all build artifacts.
# Safe to run at any time. Run as `bash build/clean.sh`.
set -e

cd "$(cd "$(dirname "$0")" && pwd)/.."

echo "Removing obj/, iso_root/, *.img, *.iso, wabt/wasm2c output..."
rm -rf obj iso_root sandfleaOS.iso disk.img data.img
rm -rf build/wabt_wasi build/wasm2c_out
rm -f  build/.wabt_build_flags obj/.build_flags
echo "clean."
