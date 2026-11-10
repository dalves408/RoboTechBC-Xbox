@echo off
setlocal EnableExtensions
rem Interactive playtest for the current RelWithDebInfo build.
rem Double-click this file from Explorer, or run it from a cmd prompt.
rem Full boot — logo / intro Binks play (no movie skip).

cd /d "%~dp0"

set "EXE=%CD%\build\RelWithDebInfo\robotech_bc_recomp.exe"
if not exist "%EXE%" (
    echo Missing "%EXE%"
    echo Build first:  powershell -File scripts\build.ps1 -Configuration RelWithDebInfo
    pause
    exit /b 1
)

rem If an earlier bring-up pass stashed logos/intros, put them back so boot is complete.
powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\skip_boot_movies.ps1" -Restore >nul 2>&1

rem main.c already defaults DX11 / present / PB_EXEC / keyboard / FFP_INLINE / etc.
rem Only clear a watchdog if a prior drive script left one in this shell.
set "RECOMP_WATCHDOG_SECS="

echo.
echo Launching (full intros — no movie skip):
echo   %EXE%
echo Logs stay in this window. Close the game window or press Ctrl+C to stop.
echo.
"%EXE%"
set "EC=%ERRORLEVEL%"
echo.
echo Exit code: %EC%
pause
exit /b %EC%
