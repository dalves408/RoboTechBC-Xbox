@echo off
setlocal EnableExtensions
rem Double-click: cold-boot with full intros + pad/keyboard menu hand-drive.
rem Same idea as other xboxrecomp projects\run_menu_test.bat:
rem   raw keyboard INPUTSINK + async GetAsyncKeyState (console may keep focus)
rem   A/Enter/Space/Z skips guest Bink/Attract and host FMV
rem Delete dumps a screenshot under screenshots\.
cd /d "%~dp0"

set "EXE=%CD%\build\RelWithDebInfo\robotech_bc_recomp.exe"
if not exist "%EXE%" (
    echo Missing "%EXE%"
    echo Build first:  powershell -File scripts\build.ps1 -Configuration RelWithDebInfo
    pause
    exit /b 1
)

rem Pin bak OHCI — shared toolkit ohci.c often drifts to XAPI_TD WIP.
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

rem Full intros (guest .bik). Restore if a bring-up skip left them stashed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\skip_boot_movies.ps1" -Restore >nul 2>&1

rem --- host input / present (required for pad + keyboard menus) ---
set "RECOMP_USB=1"
set "RECOMP_USB_PADS=1"
set "RECOMP_CONTIG_AVOID_IMAGE=1"
set "RECOMP_KEYBOARD=1"
set "RECOMP_KEYBOARD_FOCUS=0"
set "RECOMP_FB_WINDOW=1"
set "RECOMP_D3D_PRESENT=1"
set "RECOMP_INPUT_DIAG=1"
set "RECOMP_KEY_TRACE=1"
set "RECOMP_USB_STATS=1"

rem SYNTH_TD=1 crashes this title's XAPI ISR (NextTD→0x7FFFFFFF) the moment
rem the interrupt ED appears — leaving reports=0 forever if you quit early,
rem or a hard fault if you wait. XAPI queues real TDs itself on bak OHCI.
rem Do NOT set XAPI_TD — freelist address is wrong for this title.
set "RECOMP_USB_SYNTH_TD="
set "RECOMP_USB_XAPI_TD="
set "RECOMP_PAD_SCRIPT="
set "RECOMP_PAD_LIVE="
set "RECOMP_PAD_PRESS="
set "RECOMP_KEY_SCRIPT="
set "RECOMP_WATCHDOG_SECS="
set "RECOMP_FMV_DUMP="

rem Menu-safe: clear Ch1/probe leftovers that Explorer/user env can inherit.
rem Non-zero SWM5_PACE_MS early-signals the D3D fence → white-box glyph menus.
set "RECOMP_SWM5_PACE_MS=0"
set "RECOMP_SWM5_PACE_ENABLE="
set "RECOMP_SWM5_PACE_THREAD="
set "RECOMP_VBLANK_THREAD="
set "RECOMP_PAD_HOLD_MASK="
set "RECOMP_PAD_HOLD_ENABLE="
set "RECOMP_GPU_SKIPBLACK="
set "RECOMP_WATCH="
set "RECOMP_KERNEL_WATCH="
set "RECOMP_PEEK="
set "RECOMP_TIMER_DPC_MS="
set "RECOMP_FLIP_PACING=0"

set "RECOMP_FB_SHOT_DIR=%CD%\screenshots"
set "RECOMP_ERR_LOG=%CD%\screenshots\test_game.err.log"
if not exist "%RECOMP_FB_SHOT_DIR%" mkdir "%RECOMP_FB_SHOT_DIR%"

echo.
echo ============================================================
echo  Robotech Battlecry — interactive play
echo  Full intros ON  ^|  pad + keyboard  ^|  bak OHCI
echo ============================================================
echo.
echo  Controls (console may keep focus — raw + async still work):
echo    Arrows     D-pad
echo    Enter / Z  A ^(confirm / skip intro^)
echo    Esc / X    B
echo    Space      START ^(skip intro / Attract^)
echo    WASD       left stick
echo    Delete     Screenshot -^> %RECOMP_FB_SHOT_DIR%\shot_*.bmp
echo.
echo  Enter/Space/Z skips Bink/Attract once the pad is open ^(~20-40s in^).
echo  Keys work even if this console keeps focus ^(raw + async^).
echo  Wait through logos until the title menu — do not close early.
echo  EXE: %EXE%
echo  Log: %RECOMP_ERR_LOG%
echo.

"%EXE%" 2> "%RECOMP_ERR_LOG%"
set "EC=%ERRORLEVEL%"
echo.
echo Exit code: %EC%
pause
exit /b %EC%
