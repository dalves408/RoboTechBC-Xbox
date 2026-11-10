@echo off
setlocal EnableExtensions
rem Interactive playtest for the current RelWithDebInfo build.
rem Double-click this file from Explorer, or run it from a cmd prompt.
rem Full boot — logo / intro Binks play (no movie skip).
rem Screenshots: focus the FB window, press Delete → screenshots\shot_*.bmp
rem
rem Controls (keyboard → pad; works even if the launch console has focus):
rem   Arrows = D-pad     Enter/Z = A (confirm)     Esc/X = B (back)
rem   Space = START      Backspace = BACK
rem   WASD = left stick  IJKL = right stick
rem   C/V = X/Y          Q/E = White/Black         1/3 = L/R trigger
rem   Delete = screenshot (FB window). Prefer clicking the game window.

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

rem main.c defaults DX11 / present / PB_EXEC / etc. Explicit input for interactive play:
rem keyboard-as-pad + USB pad 0 (bak OHCI). Clear drive-script leftovers.
set "RECOMP_WATCHDOG_SECS="
set "RECOMP_PAD_LIVE="
set "RECOMP_PAD_SCRIPT="
set "RECOMP_PAD_PRESS="
set "RECOMP_KEY_SCRIPT="
set "RECOMP_USB_XAPI_TD="
set "RECOMP_KEYBOARD_FOCUS="
set "RECOMP_KEYBOARD=1"
set "RECOMP_USB=1"
set "RECOMP_USB_PADS=1"
set "RECOMP_CONTIG_AVOID_IMAGE=1"

rem Delete dumps via fb_present RECOMP_FB_SHOT_DIR (BMP).
set "RECOMP_FB_SHOT_DIR=%CD%\screenshots"
if not exist "%RECOMP_FB_SHOT_DIR%" mkdir "%RECOMP_FB_SHOT_DIR%"

echo.
echo Launching (full intros — no movie skip):
echo   %EXE%
echo Controls: Arrows=D-pad  Enter/Z=A  Esc/X=B  Space=START
echo   WASD=left stick  IJKL=right stick  C/V=X/Y  Q/E=White/Black  1/3=triggers
echo   ^(keyboard works even if this console has focus; click game window preferred^)
echo Screenshots: Delete → %RECOMP_FB_SHOT_DIR%
echo Logs stay in this window. Close the game window or press Ctrl+C to stop.
echo.
"%EXE%"
set "EC=%ERRORLEVEL%"
echo.
echo Exit code: %EC%
pause
exit /b %EC%
