@echo off
echo ==============================================
echo   sandfleaOS Dashboard (Logs + Test Harness)
echo ==============================================
echo.
echo  - log dashboard on  http://localhost:8080
echo  - harness UI on     http://localhost:8080/harness
echo  - QEMU must be launched with:
echo      -vnc :0 -qmp tcp:127.0.0.1:4444,server,nowait
echo    (use WR-HARNESS.BAT as a sibling to WR.BAT)
echo.
echo The harness auto-reconnects every 1-5 seconds if QEMU isn't
echo running yet, so launch order does not matter:
echo   1) WR-HARNESS.BAT  (build + boot QEMU)
echo   2) RUN.BAT         (this file)
echo or vice versa - the dashboard will pick up once QMP binds :4444.
echo.
echo The harness uses Pillow to convert QEMU PPM screendumps to PNG.
echo If /api/qmp/screendump returns errors, run:   pip install Pillow
echo Press Ctrl+C to stop.
echo.

REM Single unified server — log dashboard + harness on one port.
python harness.py %*

pause
