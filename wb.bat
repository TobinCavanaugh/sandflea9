@echo off
@REM Compile WASM files if tools are available
set WAT2WASM="C:\bin\wabt\bin\wat2wasm.exe"
if exist %WAT2WASM% (
    echo --- Compiling WASM ---
    if not exist "obj\wasm" mkdir "obj\wasm"
    for %%f in (src\wasm\wat\*.wat) do (
        echo   %%f
        %WAT2WASM% "%%f" -o "obj\wasm\%%~nf.wasm"
    )
    @REM Copy pre-compiled .wasm files that have no .wat source (e.g. doom-v0.1.0.wasm)
    @REM This avoids overwriting freshly-compiled files from the loop above.
    for %%f in (src\blob\*.wasm) do (
        if not exist "src\wasm\wat\%%~nf.wat" (
            copy "%%f" "obj\wasm\" >nul
        )
    )
) else (
    echo WARNING: wat2wasm not found at %WAT2WASM%, skipping WASM compilation.
)

dos2unix build.sh
wsl ./build.sh
exit /b %ERRORLEVEL%