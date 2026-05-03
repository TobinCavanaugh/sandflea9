@echo off
@REM Compile WASM files if tools are available
set WAT2WASM="C:\bin\wabt\bin\wat2wasm.exe"
if exist %WAT2WASM% (
    echo --- Compiling WASM ---
    if not exist "src\blob" mkdir "src\blob"
    %WAT2WASM% src\wasm\add_test.wat -o src\blob\add_test.wasm
    %WAT2WASM% src\wasm\file_test.wat -o src\blob\file_test.wasm
) else (
    echo WARNING: wat2wasm not found at %WAT2WASM%, skipping WASM compilation.
)

dos2unix build.sh
wsl ./build.sh