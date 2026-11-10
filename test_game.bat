@echo off
setlocal EnableExtensions
rem Interactive playtest for the current RelWithDebInfo build.
rem Double-click from Explorer. Full intros. Delete = screenshot.
rem
rem Controls (click the game window first):
rem   Arrows = D-pad     Enter/Z = A (confirm)     Esc/X = B (back)
rem   Space = START      Backspace = BACK
rem   WASD = left stick  IJKL = right stick
rem   C/V = X/Y          Q/E = White/Black         1/3 = L/R trigger

cd /d "%~dp0"

set "EXE=%CD%\build\RelWithDebInfo\robotech_bc_recomp.exe"
if not exist "%EXE%" (
    echo Missing "%EXE%"
    echo Build first:  powershell -File scripts\build.ps1 -Configuration RelWithDebInfo
    pause
    exit /b 1
)

rem Keep bak OHCI linked. Shared toolkit ohci.c often drifts to XAPI_TD WIP
rem (empty interrupt EDs / bad freelist). SYNTH_TD crashes this title's ISR.
set "OHCI=%CD%\tools\xboxrecomp\src\usb\ohci.c"
set "OHCI_BAK=%CD%\patches\ohci.c.dx11bak"
if not exist "%OHCI_BAK%" if exist "%CD%\tools\xboxrecomp\src\usb\ohci.c.dx11bak" (
    if not exist "%CD%\patches" mkdir "%CD%\patches"
    copy /y "%CD%\tools\xboxrecomp\src\usb\ohci.c.dx11bak" "%OHCI_BAK%" >nul
)
if exist "%OHCI%" if exist "%OHCI_BAK%" (
    fc /b "%OHCI%" "%OHCI_BAK%" >nul 2>&1
    if errorlevel 1 (
        echo WARNING: ohci.c drifted from bak — restoring and rebuilding RelWithDebInfo.
        copy /y "%OHCI_BAK%" "%OHCI%" >nul
        powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\build.ps1" -Configuration RelWithDebInfo
        if errorlevel 1 (
            echo Rebuild failed — interactive controls may stay dead until bak OHCI is linked.
            pause
            exit /b 1
        )
    )
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\skip_boot_movies.ps1" -Restore >nul 2>&1

rem Clear scripted/drive leftovers.
set "RECOMP_WATCHDOG_SECS="
set "RECOMP_PAD_LIVE="
set "RECOMP_PAD_SCRIPT="
set "RECOMP_PAD_PRESS="
set "RECOMP_KEY_SCRIPT="
set "RECOMP_KEYBOARD_FOCUS="
set "RECOMP_USB_SYNTH_TD="
set "RECOMP_USB_XAPI_TD="

rem Live keyboard + USB pad 0 (bak OHCI).
set "RECOMP_KEYBOARD=1"
set "RECOMP_USB=1"
set "RECOMP_USB_PADS=1"
set "RECOMP_CONTIG_AVOID_IMAGE=1"

set "RECOMP_FB_SHOT_DIR=%CD%\screenshots"
if not exist "%RECOMP_FB_SHOT_DIR%" mkdir "%RECOMP_FB_SHOT_DIR%"

echo.
echo Launching (full intros — no movie skip):
echo   %EXE%
echo Controls: Arrows=D-pad  Enter/Z=A  Esc/X=B  Space=START
echo   WASD=left stick  IJKL=right stick — click the GAME window, then press keys
echo Screenshots: Delete → %RECOMP_FB_SHOT_DIR%
echo.
"%EXE%"
set "EC=%ERRORLEVEL%"
echo.
echo Exit code: %EC%
pause
exit /b %EC%
