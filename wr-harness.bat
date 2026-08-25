@echo off
REM wr-harness.bat — like wr.bat, but with the two extra QEMU flags the
REM dashboard harness (dashboard/harness.py) needs:
REM
REM   -vnc :0                                          so a browser can stream
REM                                                      the framebuffer via the
REM                                                      built-in VNC server
REM                                                      (consumed by noVNC; works
REM                                                      alongside -display sdl).
REM   -qmp tcp:127.0.0.1:45454,server,nowait           so the harness can
REM                                                      hot-plug / un-plug USB
REM                                                      devices via JSON-RPC
REM                                                      (send-key, device_add,
REM                                                      device_del, screendump).
REM
REM Run this from a separate window, then in another window run
REM dashboard\run-harness.bat. Or vice versa — order doesn't matter;
REM the harness auto-reconnects while QEMU comes up.

setlocal

call wb.bat
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed with exit code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo --- Running qemu (with -vnc and -qmp for the harness) ---

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
    -cpu qemu64 ^
    -drive file=iso_root\disk.img,format=raw,index=0,media=disk ^
    -drive file=data.img,format=raw,index=1,media=disk ^
    -serial file:serial_output.log ^
    -serial file:test_log.txt ^
    -serial file:profile.log ^
    -accel whpx ^
    -device qemu-xhci,id=xhci ^
    -vnc :0 ^
    -qmp tcp:127.0.0.1:45454,server,nowait
