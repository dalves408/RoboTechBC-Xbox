Robotech: Battlecry Recomp
The original Xbox game, recompiled to run natively on Windows.

PLAY
  Start "Robotech Battlecry Recomp" from the Start menu (or the desktop shortcut).
  "Robotech Battlecry Recomp Settings" (Config.exe) changes the video settings and
  controls outside the game; F1 in the game opens the same settings.
  Intros can be skipped with Enter / Space once the pad is live.
  From the release zip instead of Setup: RecompLauncher sets the game up from your
  disc image by itself. By hand, run once in this folder:
    robotech_bc_recomp.exe --prepare --game="<your disc image, drive or folder>"

REQUIREMENTS
  - Windows 10/11 x64 and a DirectX 11 GPU.
  - Media Foundation (standard Windows; on "N" editions install the Media Feature Pack).
  - Your own copy of the game: Setup copied its files from your disc into game_files.

CONTROLS
  Xbox, PlayStation and other controllers work as soon as they are plugged in.
  Versus supports two controllers. Keyboard and mouse control player 1.
  Arrows        D-pad              WASD / keypad 8462  Left stick
  Enter / Z     A                  IJKL                Right stick
  X             B                  1 / 3               Left / right trigger
  C / V         X / Y              Shift / Ctrl        Stick clicks
  Q / E         White / Black      Backspace           BACK
  Space         START (pause)      Delete              Screenshot -> screenshots\
  F1            Settings           Esc                 Quit confirmation
  F11           Fullscreen
  Controller 1: hold Back and press Start to open settings.
  Keys can be rebound, and A/B or X/Y swapped for Nintendo-style pads, on the
  CONTROLS page of F1 or the Settings program.

FILES (all in this folder: move the folder to move the game, delete it to remove it)
  game_files\    The game's files, copied from your disc
  SaveData\      Saves
  settings.ini   Settings (F1 or the Settings program edit it)
  shader_cache\  Compiled shaders, so effects don't stutter the second time
  texture_pack\  The optional hi-res texture pack: tick it in Setup, or run the
                 Hi-Res Textures Setup (F1 > Hi-res textures turns it off)
  logs\          One log per run (the newest 20 are kept)
  crashes\       A crash report if the game ever crashes
  screenshots\   Delete-key screenshots

REPORTING A PROBLEM
  Include the newest file in logs\ and, after a crash, the newest file in crashes\.

UPDATING OR REPAIRING
  Run a newer Setup and choose this same folder. It replaces the program files
  only: your game files, saves and settings are kept, and the disc isn't needed.
