# Robotech: Battlecry (Xbox) → Windows recomp

the build PC working tree for statically recompiling the original Xbox **Robotech: Battlecry** disc with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), following the same layout as `<tools>\other xboxrecomp projects` and `<tools>\Redcard 2003`.

This tree contains **no redistributable game content** in git terms; the local ISO and `game_files\` stay on the build PC.

## Disc (local)

| Item | Path |
| --- | --- |
| XISO | `<repo>\Robotech - Battlecry (USA).xiso.iso` |
| Extracted | `<repo>\game_files\` |
| Main XBE | `game_files\default.xbe` |

Verified XBE facts (`analysis\default_analysis.json`):

| Property | Value |
| --- | --- |
| Title | Robotech: Battlecry |
| Title ID | `544D0002` |
| Entry point | `0x0001319F` |
| XDK libs | `1.0.4721` (D3D8, not LTCG) |
| Sections | 26 (includes D3D, DSOUND, many BINK*, XPP, DOLBY) |
| `default.xbe` SHA-256 | `2fc8745a72d495626307d644e80f36ec48e216e0132c3c0394446f51f94400b3` |

## Toolchain on the build PC

- Visual Studio 2022 Build Tools + MSVC
- CMake, Python 3.12, Git
- `extract-xiso` at `<tools>\tools\xiso\artifacts\extract-xiso.exe`
- Shared toolkit junction: `tools\xboxrecomp` → `<tools>\tools\xboxrecomp` (v0.16.0)

## Commands

```powershell
cd "<repo>"
.\scripts\setup.ps1
.\scripts\extract_disc.ps1
.\.venv\Scripts\python.exe scripts\pipeline.py --lift
.\scripts\build.ps1 -Configuration RelWithDebInfo
.\.venv\Scripts\python.exe scripts\run_game.py --configuration RelWithDebInfo --seconds 60
```

## Milestone status (2026-11-09)

- **Boots** RelWithDebInfo `build\RelWithDebInfo\robotech_bc_recomp.exe`; 640×480 window opens.
- D3D init needed `xbox_Nv2aMirrorFence(0x00166B48, 0x30, 0x34)` in `src/main.c`.
- Logo/intro Binks are slow; use `.\scripts\skip_boot_movies.ps1` for menu bring-up.
- With movies skipped: loads `menu_f.lvl`, skins, theme; menu tick `0x0001FF70` seeded/lifted.
- **Still black on screen** — menu state runs, presented FB dump is empty. Next: present/flip path.

```powershell
.\scripts\skip_boot_movies.ps1
$env:RECOMP_AC97_READY='1'; $env:RECOMP_FB_WINDOW='1'; $env:RECOMP_KEYBOARD='1'
.\.venv\Scripts\python.exe scripts\run_game.py --configuration RelWithDebInfo
```
