@echo off
echo ==============================================
echo   sandfleaOS Log Dashboard
echo ==============================================
echo.
echo Starting log server on http://localhost:8079
echo Press Ctrl+C to stop
echo.
python log_server.py %*
pause
