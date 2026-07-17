@echo off
REM wb.bat — build the OS from a Windows shell.
REM   1. Compile .wat -> .wasm via Windows-native wat2wasm (fast, avoids WSL
REM      round-trip for each .wat file). Fallback: copy pre-built .wasm from
REM      src\blob\ if no wat2wasm.exe installed.
REM   2. Convert the Windows cwd to a WSL path and invoke WSL bash build.
REM
REM Usage: wb.bat [extra args forwarded to build.sh]

REM Bail out early if WSL isn't installed — later commands would hang/error
REM confusingly without this check.
where wsl >nul 2>&1
if errorlevel 1 (
    echo [ERROR] WSL is not installed or not on PATH.
    echo         Install WSL ^(Ubuntu recommended^): wsl --install
    echo         Then verify:  wsl --status
    exit /b 1
)

setlocal enabledelayedexpansion

set "WAT2WASM=C:\bin\wabt\bin\wat2wasm.exe"
if exist "%WAT2WASM%" (
    echo --- Compiling WASM ^(Windows wat2wasm.exe^) ---
    if not exist "obj\wasm" mkdir "obj\wasm"
    for %%f in (src\wasm\wat\*.wat) do (
        echo   %%f
        "%WAT2WASM%" "%%f" -o "obj\wasm\%%~nf.wasm"
    )
    REM Pre-compiled .wasm blobs (no matching .wat source): copy unchanged.
    for %%f in (src\blob\*.wasm) do (
        if not exist "src\wasm\wat\%%~nf.wat" (
            copy "%%f" "obj\wasm\" >nul
        )
    )
) else (
    echo WARNING: wat2wasm not found at "%WAT2WASM%", copying pre-built .wasm from src\blob\
    if not exist "obj\wasm" mkdir "obj\wasm"
    for %%f in (src\blob\*.wasm) do (
        if not exist "src\wasm\wat\%%~nf.wat" (
            copy "%%f" "obj\wasm\" >nul
        )
    )
)

REM Translate %CD% (Windows) to a WSL path and run build.sh inside WSL.
REM If the resolved WSL path contains a single quote, fail loudly rather
REM than produce a broken cd command downstream.
for /f "usebackq tokens=*" %%I in (`wsl wslpath -a "%CD%"`) do set "WSL_DIR=%%I"
echo "%WSL_DIR%" | findstr /R /C:"'" >nul
if not errorlevel 1 (
    echo [ERROR] Project path contains a single quote after WSL translation:
    echo         %WSL_DIR%
    echo         WSL bash cannot cd into this path. Rename the offending directory.
    exit /b 1
)
echo Invoking WSL build at "%WSL_DIR%"
wsl -e bash -c "cd '%WSL_DIR%' && bash build/build.sh %*"
exit /b %ERRORLEVEL%
