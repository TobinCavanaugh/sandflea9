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

REM Record start timestamp for timing
for /f %%t in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "WB_START_MS=%%t"

set "WAT2WASM=C:\bin\wabt\bin\wat2wasm.exe"
if exist "%WAT2WASM%" (
    echo --- Compiling WASM ^(Windows wat2wasm.exe^) ---
    if not exist "obj\wasm" mkdir "obj\wasm"
    for %%f in (src\wasm\wat\*.wat) do (
        set "DO_COMPILE=0"
        if not exist "obj\wasm\%%~nf.wasm" (
            set "DO_COMPILE=1"
        ) else (
            for %%w in ("obj\wasm\%%~nf.wasm") do (
                if "%%~tf" gtr "%%~tw" set "DO_COMPILE=1"
            )
        )
        if "!DO_COMPILE!"=="1" (
            echo   %%f
            "%WAT2WASM%" "%%f" -o "obj\wasm\%%~nf.wasm"
        )
    )
    REM Pre-compiled .wasm files from drives\A (no matching .wat source)
    if exist "drives\A\*.wasm" (
        for %%f in (drives\A\*.wasm) do (
            if not exist "src\wasm\wat\%%~nf.wat" (
                copy "%%f" "obj\wasm\" >nul
            )
        )
    )
) else (
    echo WARNING: wat2wasm not found at "%WAT2WASM%"
    if not exist "obj\wasm" mkdir "obj\wasm"
    if exist "drives\A\*.wasm" (
        for %%f in (drives\A\*.wasm) do (
            if not exist "src\wasm\wat\%%~nf.wat" (
                copy "%%f" "obj\wasm\" >nul
            )
        )
    )
)

REM Translate %CD% (Windows) to a WSL path instantly in pure batch (avoids slow wslpath process spawn).
set "DRIVE_LETTER=%CD:~0,1%"
for %%a in (a b c d e f g h i j k l m n o p q r s t u v w x y z) do (
    if /i "!DRIVE_LETTER!"=="%%a" set "DRIVE_LOWER=%%a"
)
set "REST_OF_PATH=%CD:~2%"
set "REST_OF_PATH=!REST_OF_PATH:\=/!"
set "WSL_DIR=/mnt/!DRIVE_LOWER!!REST_OF_PATH!"

echo Invoking WSL build at "%WSL_DIR%"
wsl -e bash -c "cd '%WSL_DIR%' && bash build/build.sh %*"
set "BUILD_EXIT_CODE=%ERRORLEVEL%"

REM Output total elapsed time in seconds
if defined WB_START_MS (
    for /f %%t in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "WB_END_MS=%%t"
    for /f %%d in ('powershell -NoProfile -Command "([double](!WB_END_MS! - !WB_START_MS!) / 1000).ToString('0.00')"') do set "WB_ELAPSED=%%d"
    echo [wb.bat] Total build time: !WB_ELAPSED!s
)

exit /b %BUILD_EXIT_CODE%
