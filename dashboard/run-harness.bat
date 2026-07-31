@echo off
REM RUN-HARNESS.BAT is now an alias for RUN.BAT — both launch the log
REM server child window plus the harness foreground process. Kept as a
REM sibling so muscle-memory paths keep working.
call "%~dp0run.bat" %*
