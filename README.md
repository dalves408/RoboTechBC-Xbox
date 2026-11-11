# Robotech: Battlecry (Xbox → Windows)

Windows recompilation / port of the original Xbox **Robotech: Battlecry** disc, built with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and a DX11 present path.

This repository holds the **game project** sources, scripts, and pinned USB patches. It does **not** ship the commercial disc, extracted `game_files\`, or the shared xboxrecomp toolkit tree (on the build PC that toolkit is a local junction at `tools\xboxrecomp` → `<tools>\tools\xboxrecomp`).

## Current status

| Area | State |
| --- | --- |
| Title / menus | Working (DX11, letterboxed 4:3). Menu flashing fixed (BlockOnFence spurious-wake recheck). 130+ fps. |
| Controls | Working (bak OHCI, keyboard + USB pad); verified in-mission (stick, START pause menu). |
| Ch1 mission | Plays live: world, HUD, radar, enemies, weapons. 60–80 fps (min 59 over a 100 s soak), no hangs. The previous "frozen cinematic" was the unresolved stub at 0x1166C (see `src/recomp_manual.c`), so the `RECOMP_M101_CINE_SHORT` overlay is no longer needed — it was a workaround, not a fix. |
| Audio | Music, SFX and VO play; no underruns or clipping in the mix. |
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
| Esc / X | B |
| Space | START (skip intro / Attract) |
| WASD | Left stick |
| Delete | Screenshot → `screenshots\shot_*.bmp` |

Keys work even when the console keeps focus (raw INPUTSINK + async). Enter/Space/Z skip guest Bink/Attract once the pad map is open (~20–40s in). Wait through logos to the title menu.

### Agent / bring-up: short Ch1 cinematic

For faster Ch1 HUD entry without dirtying the level:

```powershell
cd "<repo>"
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_m101_cine_overlay.ps1 -Mode Mild
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\drive_ch1_overlay.ps1 -Mode Mild
```

## Build from scratch

Needs Visual Studio 2022 Build Tools (MSVC), CMake, Python 3.12, Git, `extract-xiso`, and a local xboxrecomp checkout linked or copied as `tools\xboxrecomp`.

```powershell
cd "<repo>"
.\scripts\setup.ps1
.\scripts\extract_disc.ps1
.\.venv\Scripts\python.exe scripts\pipeline.py --lift
.\scripts\build.ps1 -Configuration RelWithDebInfo
.\test_game.bat
```

## Diagnostics

```powershell
# Boot, skip intros, play into Ch1; screenshots, fps/CPU timeline, log anomalies, hang stacks
$keys=((6..30 | ? { $_ % 2 -eq 0 } | % { "${_}:enter:150" })) -join ','
python scripts\diag\rbx_diag.py --secs 100 --shot-every 4 --audio --name run1 --keys $keys
```

Output goes to `analysis\diag\<name>\` (`report.json`, `shots\`, `stderr.log`, `hang_t*_stacks.txt`).

| Tool | Use |
| --- | --- |
| `scripts\diag\rbx_diag.py` | Harness: timed keys, fps/CPU/threads, frozen-frame + hang detection, audio WAV analysis. |
| `scripts\diag\stackdump.py <pid>` | Symbolized stacks of every thread (guest functions show as `sub_XXXXXXXX`). |
| `scripts\diag\flash_scan.py` | Finds flashing frames in a `RECOMP_GPU_PRESDUMP` capture. |
| `-DRBX_ABI_CHECK=ON` (CMake) | Diagnostic build that reports lifted functions which fail to restore ebx/esi/edi/esp. |
| `RECOMP_STUB_LOG=1` | Logs when an unresolved lifter stub is reached. |
| `RECOMP_PROF=lo-hi` | Samples the busiest thread between those seconds and prints hot source lines. |

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
| `scripts\` | Setup, extract, lift/build, Ch1 drive helpers |
| `patches\` | Pinned bak OHCI (`ohci.c.dx11bak`, `ohci_rbx_pinned.c`) |
| `docs\` | Milestone notes checked into this tree |
| `test_game.bat` | Interactive play launcher |

## License / ownership

Robotech: Battlecry is a commercial game. Provide your own legally obtained Xbox ISO. This repo publishes only recomp/port engineering work — not game assets or binaries.
