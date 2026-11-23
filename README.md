# Robotech: Battlecry (Xbox → Windows)

Windows recompilation / port of the original Xbox **Robotech: Battlecry** disc, built with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and a DX11 present path.

This repository holds the **game project** sources, scripts, and pinned USB patches. It does **not** ship the commercial disc, extracted `game_files\`, or the shared xboxrecomp toolkit tree (on the build PC that toolkit is a local junction at `tools\xboxrecomp` → `<tools>\tools\xboxrecomp`).

## Current status (2026-11-11)

| Area | State |
| --- | --- |
| Title / menus | Working (DX11, letterboxed 4:3). Menu flashing fixed (BlockOnFence spurious-wake recheck). 130+ fps. |
| Controls | Working (bak OHCI, keyboard + USB pad); verified in-mission (stick, START pause menu). |
| Ch1 mission | Plays live: world, HUD, radar, enemies, weapons. 60–80 fps (min 59 over a 100 s soak), no hangs. The previous "frozen cinematic" was the unresolved stub at 0x1166C (see `src/recomp_manual.c`), so the `RECOMP_M101_CINE_SHORT` overlay is no longer needed — it was a workaround, not a fix. |
| Audio | Music, SFX and VO play; no underruns or clipping in the mix. Intro movie (Bink) audio is in sync and clean: the pinned voice processor (`patches\apu_vp_rbx.c`) waits out the title's VOICE_LOCK instead of skipping frames, which previously made the movie stream drift and get overwritten. |
| Open | A rare (~1 in 13 runs) main-menu stall; `rbx_diag.py` now dumps all thread stacks when it recurs. |

## Disc (local only — gitignored)

| Item | Path / name |
| --- | --- |
| XISO | `Robotech - Battlecry (USA).xiso.iso` |
| Extracted tree | `game_files\` |
| Main XBE | `game_files\default.xbe` (Title ID `544D0002`) |

## Play (the build PC)

Double-click or run:

```bat
test_game.bat
```

That launcher:

- Runs `build\RelWithDebInfo\robotech_bc_recomp.exe`
- Keeps **full intros** (restores any stashed Binks)
- Forces `RECOMP_SWM5_PACE_MS=0` (non-zero early-signals the D3D fence and breaks menus into white-box glyphs)
- Pins **bak OHCI** from `patches\ohci.c.dx11bak` and rebuilds if the shared toolkit `ohci.c` drifted
- Clears `RECOMP_USB_SYNTH_TD` / `RECOMP_USB_XAPI_TD` (do **not** enable these for this title)

### Controls

| Input | Pad |
| --- | --- |
| Arrows | D-pad |
| Enter / Z | A (confirm / skip intro) |
| X | B |
| Space | START (skip intro / Attract) |
| WASD / keypad 8462 | Left stick |
| IJKL | Right stick |
| C / V | X / Y |
| Q / E | White / Black |
| 1 / 3 | Left / right trigger |
| Shift / Ctrl | Left / right stick click |
| Backspace | BACK |
| Delete | Screenshot → `screenshots\shot_*.bmp` |
| F1 | Video settings |
| Esc | Quit confirmation or close settings |
| F11 / Alt+Enter | Toggle borderless fullscreen |

Keys work even when the console keeps focus (raw INPUTSINK + async). Enter/Space/Z skip guest Bink/Attract once the pad map is open (~20–40s in). Wait through logos to the title menu.

Versus supports two players. Connect two SDL3 gamepads for separate controls. If Player 2 disconnects, their controls go neutral and resume when SDL3 reconnects the pad. Keyboard and mouse control Player 1. Controller 1 can hold Back and press Start to open Video settings. Use the D-pad or left stick and A to adjust settings, and B or Start to close the overlay.

Video settings include 1×–5× internal resolution, borderless fullscreen, 4:3 or stretched output, VSync, 30–360 FPS presentation limits or uncapped presentation, 2×/4×/8× MSAA where supported, 2×–16× anisotropic filtering, and hi-res texture replacements. The presentation limit leaves the Xbox game clock unchanged. Settings are saved to `settings.ini`.

### Short Ch1 cinematic overlay (obsolete)

`scripts\build_m101_cine_overlay.ps1` / `scripts\drive_ch1_overlay.ps1` (`RECOMP_M101_CINE_SHORT`) were a workaround for the frozen Ch1 cinematic. That freeze is fixed (stub 0x1166C), so the overlay is no longer needed; the scripts are kept for reference only.

## Build from scratch

Needs Visual Studio 2022 Build Tools (MSVC), CMake, Python 3.12, Git, `extract-xiso`, and a local xboxrecomp checkout linked or copied as `tools\xboxrecomp`.

Place the SDL3 Windows x64 development package (3.4.20) beside the xboxrecomp checkout as `SDL3-3.4.20`, or set CMake's `RBX_SDL3_ROOT` to its location. The build copies `SDL3.dll` beside the executable.

```powershell
cd "<repo>"
.\scripts\setup.ps1
.\scripts\extract_disc.ps1
.\.venv\Scripts\python.exe scripts\pipeline.py --lift
.\scripts\build.ps1 -Configuration RelWithDebInfo
.\test_game.bat
```

The packaged game is portable. Keep the executable, DLLs, and `game_files` together. Xbox disk partitions are emulated in memory. Saves and other Xbox filesystem data go in `SaveData`, video settings in `settings.ini`, and optional replacement textures in `texture_pack`. The launcher keeps screenshots and logs in the same installation folder. Move the whole folder to carry your settings and saves with it.

## Private test release

```powershell
python scripts\package_release.py --version 1.0
```

Writes `dist\RobotechBC_Recomp_v<ver>_test.zip` (+ `.sha256`): exe, pdb, `vcruntime140.dll`, `SDL3.dll`, the disc `game_files\` (bring-up leftovers excluded), `Play.bat` (sets the play env; saves to `SaveData\`, logs to `logs\`) and `README.txt`. The game uses the Windows copy of `D3DCompiler_47.dll`. It contains game data: private testing on owned machines only, never commit or share `dist\`. Test an unzipped copy with `rbx_diag.py --exe <dir>\robotech_bc_recomp.exe --cwd <dir>`.

## Diagnostics

```powershell
# Boot, skip intros, play into Ch1; screenshots, fps/CPU timeline, log anomalies, hang stacks
$keys=((6..30 | ? { $_ % 2 -eq 0 } | % { "${_}:enter:150" })) -join ','
python scripts\diag\rbx_diag.py --secs 100 --shot-every 4 --audio --name run1 --keys $keys
```

Output goes to `analysis\diag\<name>\` (`report.json`, `shots\`, `stderr.log`, `hang_t*_stacks.txt`).

For repeatable input timing, use `--frame-pad "f360:a,f540:a,f720:a:18"` instead of `--keys`. Each press starts at the specified guest render flip and lasts eight flips by default. The report lists the first flip where the game polled each press. Use a fresh `--name` for an independent save directory.

Add `--frame-dump-every 240` to capture numbered BMPs. On a second run add `--compare-frames analysis/diag/<first-run>/frames` to report matching frame numbers, missing captures, and mean RGB errors. Guest clocks and audio still run in real time, so matching input numbers alone do not guarantee pixel-identical frames.

| Tool | Use |
| --- | --- |
| `scripts\diag\rbx_diag.py` | Harness: timed keys, fps/CPU/threads, frozen-frame + hang detection, audio WAV analysis. |
| `scripts\diag\stackdump.py <pid>` | Symbolized stacks of every thread (guest functions show as `sub_XXXXXXXX`). |
| `scripts\diag\flash_scan.py` | Finds flashing frames in a `RECOMP_GPU_PRESDUMP` capture. |
| `-DRBX_ABI_CHECK=ON` (CMake) | Diagnostic build that reports lifted functions which fail to restore ebx/esi/edi/esp. |
| `RECOMP_STUB_LOG=1` | Logs when an unresolved lifter stub is reached. |
| `RECOMP_PROF=lo-hi` | Samples the busiest thread between those seconds and prints hot source lines. |
| `RECOMP_XF_THREADS=n` | Overrides the vertex-transform worker count (0 disables workers, max 7). Default: one worker on hosts with at least four logical CPUs. |

## Hard constraints (do not regress)

1. **No `RECOMP_USB_SYNTH_TD` / `RECOMP_USB_XAPI_TD`** — SYNTH crashes this title’s XAPI ISR; keep bak OHCI pinned.
2. **`RECOMP_SWM5_PACE_MS=0`** on the tester’s menu play path.
3. **Full intros** for interactive testing unless explicitly asked to skip.
4. **Never leave `m101_f.lvl` dirty.**
5. **Run `scripts\patch_kickoff_yield.ps1` after every lift** (`build.ps1` does it): it re-applies the hand fixes to the gitignored `src\recomp\gen`.

## Layout

| Path | Role |
| --- | --- |
| `src\` | Host entry (`main.c`), DX11 present (`rbx_d3d11.c`), recomp stubs |
| `scripts\` | Setup, extract, lift/build, gen patcher, Ch1 drive helpers |
| `scripts\diag\` | Diagnostic harness, stack dumper, flash scanner |
| `patches\` | Pinned bak OHCI (`ohci.c.dx11bak`, `ohci_rbx_pinned.c`) |
| `docs\` | Milestone notes checked into this tree |
| `test_game.bat` | Interactive play launcher |

## License / ownership

Robotech: Battlecry is a commercial game. Provide your own legally obtained Xbox ISO. This repo publishes only recomp/port engineering work — not game assets or binaries.
