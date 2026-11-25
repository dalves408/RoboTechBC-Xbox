"""Package a private test build: exe + Visual C++ runtime + game_files + launcher.

    python scripts\\package_release.py --version 1.0

Writes dist\\RobotechBC_Recomp_v<version>_test.zip. The zip contains the extracted
disc (game_files\\) and is for private testing on machines that own the game only.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "RelWithDebInfo"
GAME = ROOT / "game_files"
SYS32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"

# Bring-up leftovers that are not part of the disc.
EXCLUDE_SUFFIXES = (".cine_full", ".cine_short")
EXCLUDE_NAMES = {"default_analysis.json"}
EXCLUDE_DIRS = {".skipped_for_bringup"}
RUNTIME_DATA_DIRS = {"udata", "tdata", "cache"}
MOVIE_STASH = GAME / "Media" / "vcResources" / "xbox" / "vcMovie" / ".skipped_for_bringup"

PLAY_BAT = r"""@echo off
setlocal EnableExtensions
rem Robotech: Battlecry (Xbox recomp) - test release {version}
cd /d "%~dp0"

set "RECOMP_USB=1"
set "RECOMP_CONTIG_AVOID_IMAGE=1"
set "RECOMP_KEYBOARD=1"
set "RECOMP_KEYBOARD_FOCUS=0"
set "RECOMP_FB_WINDOW=1"
set "RECOMP_D3D_PRESENT=1"
set "RECOMP_SWM5_PACE_MS=0"
set "RECOMP_FLIP_PACING=0"
set "RECOMP_INPUT_DIAG=1"
set "RECOMP_KEY_TRACE=1"
set "RECOMP_USB_STATS=1"

rem Never inherit these (they break input or menus for this title).
for %%V in (RECOMP_USB_SYNTH_TD RECOMP_USB_XAPI_TD RECOMP_PAD_SCRIPT RECOMP_RBX_FRAME_PAD RECOMP_PAD_LIVE RECOMP_PAD_PRESS RECOMP_KEY_SCRIPT RECOMP_WATCHDOG_SECS RECOMP_FMV_DUMP RECOMP_SWM5_PACE_ENABLE RECOMP_SWM5_PACE_THREAD RECOMP_VBLANK_THREAD RECOMP_PAD_HOLD_MASK RECOMP_PAD_HOLD_ENABLE RECOMP_GPU_SKIPBLACK RECOMP_WATCH RECOMP_KERNEL_WATCH RECOMP_PEEK RECOMP_TIMER_DPC_MS) do set "%%V="

set "RECOMP_SAVE_DIR="
set "RECOMP_FB_SHOT_DIR=%CD%\screenshots"
if not exist "%CD%\logs" mkdir "%CD%\logs"
if not exist "%RECOMP_FB_SHOT_DIR%" mkdir "%RECOMP_FB_SHOT_DIR%"
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%T"
set "LOG=%CD%\logs\run_%TS%.log"

echo Robotech: Battlecry recomp - test release {version} ({commit})
echo Log: %LOG%
echo Close the game window to quit. Press Delete in-game for a screenshot.
"%CD%\robotech_bc_recomp.exe" 2> "%LOG%"
set "EC=%ERRORLEVEL%"
echo Exit code: %EC%
if not "%EC%"=="0" pause
exit /b %EC%
"""

README = """Robotech: Battlecry - Xbox static recompilation for Windows
Test release {version}  (commit {commit}, built {date})
PRIVATE TEST BUILD - contains game data from an owned disc. Do not distribute.

RUN
  1. Unzip to a writable folder, e.g. C:\\Games\\RobotechBC.
  2. Double-click Play.bat.
  Config.exe changes the video settings outside the game (same settings.ini as F1).
  Intros can be skipped with Enter / Space once the pad is live (~20-40 s in).

REQUIREMENTS
  - Windows 10/11 x64, a DirectX 11 GPU.
  - Media Foundation (standard Windows; on "N" editions install the Media Feature Pack).
  - vcruntime140.dll and SDL3.dll are bundled. D3DCompiler_47.dll comes from Windows.

CONTROLS
  Versus supports two SDL3 controllers. If Player 2 disconnects, reconnect
  the controller to resume input. Keyboard and mouse control Player 1.
  Arrows        D-pad              WASD / keypad 8462  Left stick
  Enter / Z     A                  IJKL                Right stick
  X             B                  1 / 3               Left / right trigger
  C / V         X / Y              Shift / Ctrl        Stick clicks
  Q / E         White / Black      Backspace           BACK
  Space         START (pause)      Delete              Screenshot -> screenshots\\
  F1            Video settings    Esc                 Quit confirmation
  F11           Fullscreen
  Controller 1: hold Back and press Start to open settings.

FILES
  SaveData\\     Save data and Xbox filesystem data
  settings.ini   Video settings (created when settings change; Config.exe or F1 edits it)
  texture_pack\\ Optional high-resolution texture replacements
  logs\\         One log per run - include it with any bug report
  screenshots\\  Delete-key screenshots
  robotech_bc_recomp.pdb  Debug symbols (keep for crash reports)

KNOWN ISSUES
  - Rarely (~1 in 13 boots) the main menu stalls. Close and relaunch.
"""


def git_commit():
    try:
        r = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True, check=True)
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--", "src", "CMakeLists.txt"],
                               capture_output=True, text=True).stdout.strip()
        return r.stdout.strip() + ("-dirty" if dirty else "")
    except Exception:
        return "unknown"


def game_files():
    for dirpath, dirnames, filenames in os.walk(GAME):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        if Path(dirpath) == GAME:
            dirnames[:] = [d for d in dirnames if d.lower() not in RUNTIME_DATA_DIRS]
        for f in filenames:
            if f in EXCLUDE_NAMES or f.endswith(EXCLUDE_SUFFIXES):
                continue
            yield Path(dirpath) / f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.0")
    ap.add_argument("--out", default=str(ROOT / "dist"))
    ap.add_argument("--level", type=int, default=6, help="deflate level (0 = store)")
    a = ap.parse_args()

    exe = BUILD / "robotech_bc_recomp.exe"
    if not exe.exists():
        sys.exit(f"missing {exe}; build first")
    if not (GAME / "default.xbe").exists():
        sys.exit(f"missing {GAME}\\default.xbe; extract the disc first")
    if MOVIE_STASH.exists() and any(MOVIE_STASH.iterdir()):
        sys.exit("intro movies are stashed; run scripts\\skip_boot_movies.ps1 -Restore")

    commit = git_commit()
    name = f"RobotechBC_Recomp_v{a.version}_test"
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    zpath = out / f"{name}.zip"
    fmt = dict(version=a.version, commit=commit, date=time.strftime("%Y-%m-%d"))

    items = [(exe, "robotech_bc_recomp.exe")]
    pdb = BUILD / "robotech_bc_recomp.pdb"
    if pdb.exists():
        items.append((pdb, pdb.name))
    runtime = SYS32 / "vcruntime140.dll"
    if not runtime.exists():
        sys.exit(f"missing {runtime}")
    items.append((runtime, runtime.name))
    sdl = BUILD / "SDL3.dll"
    if not sdl.exists():
        sys.exit(f"missing {sdl}; rebuild the game")
    items.append((sdl, sdl.name))
    config = BUILD / "Config.exe"
    if not config.exists():
        sys.exit(f"missing {config}; rebuild the game (Config.exe is built with it)")
    items.append((config, config.name))
    items += [(p, "game_files/" + p.relative_to(GAME).as_posix()) for p in game_files()]

    comp = zipfile.ZIP_DEFLATED if a.level else zipfile.ZIP_STORED
    total = sum(p.stat().st_size for p, _ in items)
    done = 0
    t0 = time.time()
    tmp = zpath.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", comp, compresslevel=a.level or None, allowZip64=True) as z:
        for p, arc in items:
            z.write(p, f"{name}/{arc}")
            done += p.stat().st_size
            if done * 20 // total != (done - p.stat().st_size) * 20 // total:
                print(f"  {done * 100 // total:3d}%  {time.time() - t0:5.0f}s", flush=True)
        z.writestr(f"{name}/Play.bat", PLAY_BAT.format(**fmt).replace("\n", "\r\n"))
        z.writestr(f"{name}/SaveData/", b"")
        z.writestr(f"{name}/README.txt", README.format(**fmt).replace("\n", "\r\n"))
        z.writestr(f"{name}/VERSION.txt", f"{a.version} {commit} {fmt['date']}\r\n")
    with zipfile.ZipFile(tmp) as z:
        bad = z.testzip()
        if bad:
            sys.exit(f"zip CRC check failed at {bad}")
        n = len(z.namelist())
    os.replace(tmp, zpath)

    h = hashlib.sha256()
    with zpath.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    (out / f"{name}.zip.sha256").write_text(f"{h.hexdigest()}  {zpath.name}\n")
    print(f"{zpath}  {zpath.stat().st_size / 2**20:,.0f} MB  {n} entries  commit {commit}")
    print(f"sha256 {h.hexdigest()}")


if __name__ == "__main__":
    main()
