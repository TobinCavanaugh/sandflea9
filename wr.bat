@echo off
REM wr.bat — build + boot the OS in qemu (Windows side).
REM   - Builds via wb.bat (which delegates to WSL).
REM   - Drives disk.img + data.img attached as raw IDE drives.
REM   - Three serial ports: COM1=primary, COM2=test, COM3=profile.

setlocal

call wb.bat
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed with exit code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo --- Running qemu ---

REM Truncate serial log files on each run.
break > serial_output.log
break > test_log.txt
break > profile.log

qemu-system-x86_64.exe -cdrom sandfleaOS.iso ^
    -m 2G ^
    -machine pc ^
    -bios ovmf\DEBUGX64_OVMF.fd ^
    -display sdl ^
    -vga std ^
    -cpu Skylake-Server ^
    -drive file=disk.img,format=raw,index=0,media=disk ^
    -drive file=data.img,format=raw,index=1,media=disk ^
    -serial file:serial_output.log ^
    -serial file:test_log.txt ^
    -serial file:profile.log ^
    -accel whpx
