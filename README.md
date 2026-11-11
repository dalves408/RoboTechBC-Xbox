# Robotech: Battlecry (Xbox → Windows)

Windows recompilation / port of the original Xbox **Robotech: Battlecry** disc, built with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and a DX11 present path.

This repository holds the **game project** sources, scripts, and pinned USB patches. It does **not** ship the commercial disc, extracted `game_files\`, or the shared xboxrecomp toolkit tree (on the build PC that toolkit is a local junction at `tools\xboxrecomp` → `<tools>\tools\xboxrecomp`).

## Current status (2026-11-10)

| Area | State |
| --- | --- |
| Title / menus | Working on `test_game.bat` (DX11, letterboxed 4:3). |
| Controls | Working (bak OHCI, keyboard + USB pad). |
| Ch1 cinematic → HUD | Non-destructive skip via `RECOMP_M101_CINE_SHORT=1` (serves `m101_f.lvl.cine_short`; pristine `m101_f.lvl` stays untouched). |
| Ch1 flight | HUD + actors; world readable after a brief white flash. **Open:** residual bright water / cyan quads; no long hang-free proof yet. |
| FPS mode | Not started yet. |

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

## Hard constraints (do not regress)

1. **No `RECOMP_USB_SYNTH_TD` / `RECOMP_USB_XAPI_TD`** — SYNTH crashes this title’s XAPI ISR; keep bak OHCI pinned.
2. **`RECOMP_SWM5_PACE_MS=0`** on the tester’s menu play path.
3. **Full intros** for interactive testing unless explicitly asked to skip.
4. **Never leave `m101_f.lvl` dirty** — use the `m101_f.lvl.cine_short` overlay (`RECOMP_M101_CINE_SHORT`).

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
