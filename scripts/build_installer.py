"""Builds the Robotech: Battlecry Recomp player release: the programs, then Setup (after the Halo 2 recomp's
build_installer.py).

    python scripts/build_installer.py --version 1.0.0 [--skip-build] [--iscc <ISCC.exe>]
    python scripts/build_installer.py --images-only        (only redraw the wizard pictures and the icon)

Output in installer/Output, each with a .sha256 (the names follow RecompLauncher's release standard):
  RobotechBC-Recomp-v<version>-Windows-x64.zip        the game folder, recomplauncher.json on top (RecompLauncher's pick)
  RobotechBC-Recomp-v<version>-Windows-x64-Setup.exe  the same programs with the disc import wizard, and the
                                                      hi-res texture pack as an optional download
  RobotechBC-Recomp-HiRes-Textures-r<n>-Windows-x64-Setup.exe   the texture pack alone, for an installed game
The texture pack's pins come from scripts/build_texture_pack.py (installer/texture_payload.iss). Without that file
Setup has no texture task and the texture Setup isn't built. None of these holds game data: the player's own disc
image supplies the game files (installer/rbx_import.c, checked against installer/manifest.h).
The programs' .pdb files (RelWithDebInfo) are kept in out/symbols/<version> for reading crash dumps and never shipped.

Needs: Visual Studio 2022 C++ build tools, CMake, the xboxrecomp toolkit and SDL3 (README, Building), Inno Setup 6.6
or newer, Python with Pillow, and installer/manifest.h (python installer/gen_manifest.py <your disc image>).
"""
import argparse
import hashlib
import json
import zipfile
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build' / 'RelWithDebInfo'   # the configuration the test releases shipped
INSTALLER = ROOT / 'installer'
OUTPUT = INSTALLER / 'Output'
SMALL_SIZES = (55, 64, 80, 96, 128)
PROGRAMS = ('robotech_bc_recomp', 'Config', 'rbx_import', 'rbx_texture_download')

# The F1 overlay's palette (src/rbx_ui.c): the RDF field manual's tactical display
BG_TOP, BG_BOT = (16, 30, 54), (4, 9, 20)
GRID, GRID_HI = (22, 38, 64), (30, 52, 86)
PANEL_TOP, PANEL_BOT = (14, 27, 49), (7, 14, 28)
EDGE, FRAME = (60, 98, 146), (150, 196, 240)
AMBER, AMBER_DK = (255, 168, 38), (122, 76, 14)
SKY, WHITE, TEXT, DIM = (110, 190, 255), (238, 243, 250), (196, 208, 226), (116, 138, 168)


def find_iscc():
    """Inno Setup 6's compiler: --iscc or ISCC_EXE if given, else a per-user install, else a machine-wide one, else
    ISCC on the PATH."""
    places = [os.environ.get('ISCC_EXE', '')]
    if os.environ.get('LOCALAPPDATA'):
        places.append(str(Path(os.environ['LOCALAPPDATA']) / 'Programs' / 'Inno Setup 6' / 'ISCC.exe'))
    for env in ('ProgramFiles(x86)', 'ProgramFiles'):
        if os.environ.get(env):
            places.append(str(Path(os.environ[env]) / 'Inno Setup 6' / 'ISCC.exe'))
    places.append(shutil.which('ISCC') or '')
    for place in places:
        if place and Path(place).is_file():
            return Path(place)
    sys.exit('Inno Setup 6 (ISCC.exe) was not found. Install Inno Setup 6.6 or newer, or pass --iscc <path to ISCC.exe> '
             '(or set ISCC_EXE). Looked in:\n  ' + '\n  '.join(p for p in places if p))


def run(*command):
    print('>', ' '.join(str(c) for c in command), flush=True)
    subprocess.run([str(c) for c in command], check=True, cwd=ROOT)


def set_version(version):
    """The version resources (the game's and Config.exe's .rc) equal the release number: Setup reads the installed
    exe's version to tell an update from a repair."""
    parts = version.split('.')
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        sys.exit('version must look like 1.0.0')
    commas, dots = ','.join(parts + ['0']), '.'.join(parts + ['0'])
    for rc in (ROOT / 'src' / 'robotech_bc.rc', ROOT / 'src' / 'config' / 'rbx_config.rc'):
        text = rc.read_text(encoding='utf-8')
        new = re.sub(r'(FILEVERSION|PRODUCTVERSION) [\d,]+', rf'\1 {commas}', text)
        new = re.sub(r'("(?:FileVersion|ProductVersion)", ")[\d.]+', rf'\g<1>{dots}', new)
        if new != text:
            rc.write_text(new, encoding='utf-8')


# ---- the wizard's pictures and the icon --------------------------------------------------------------------------
def font(size, bold=True):
    windir = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts'
    for path in (windir / 'bahnschrift.ttf', windir / ('segoeuib.ttf' if bold else 'segoeui.ttf'),
                 Path('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf' if bold else
                      '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')):
        if path.is_file():
            f = ImageFont.truetype(str(path), size)
            if path.name == 'bahnschrift.ttf':
                try:
                    f.set_variation_by_name('Bold SemiCondensed' if bold else 'SemiLight SemiCondensed')
                except Exception:
                    pass
            return f
    return ImageFont.load_default()


def spaced(d, xy, text, f, fill, gap):
    """Letter-spaced text, as the overlay draws its headings."""
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + gap
    return x


def spaced_width(d, text, f, gap):
    return sum(d.textlength(ch, font=f) for ch in text) + gap * (len(text) - 1)


def backdrop(w, h, cell=40):
    """rbx_backdrop: the navy gradient and the plotting grid (every fifth line brighter)."""
    img = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3)))
    for i, x in enumerate(range(0, w, cell)):
        d.line([(x, 0), (x, h)], fill=GRID_HI if i % 5 == 0 else GRID, width=1)
    for i, y in enumerate(range(0, h, cell)):
        d.line([(0, y), (w, y)], fill=GRID_HI if i % 5 == 0 else GRID, width=1)
    return img


def panel(img, box, c, rim=FRAME, width=2, bracket=None):
    """rbx_panel: top-left and bottom-right corners cut, the other two marked with amber targeting brackets."""
    x0, y0, x1, y1 = box
    shape = [(x0 + c, y0), (x1, y0), (x1, y1 - c), (x1 - c, y1), (x0, y1), (x0, y0 + c)]
    fill = Image.new('RGB', img.size)
    fd = ImageDraw.Draw(fill)
    for y in range(y0, y1 + 1):
        t = (y - y0) / max(1, y1 - y0)
        fd.line([(x0, y), (x1, y)], fill=tuple(int(PANEL_TOP[i] + (PANEL_BOT[i] - PANEL_TOP[i]) * t) for i in range(3)))
    mask = Image.new('L', img.size, 0)
    ImageDraw.Draw(mask).polygon(shape, fill=255)
    img.paste(fill, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.line(shape + [shape[0]], fill=rim, width=width, joint='curve')
    b = bracket or max(10, c // 2)
    bw = max(2, width + 1)
    d.line([(x1 - b, y0 + bw), (x1 - bw // 2, y0 + bw), (x1 - bw // 2, y0 + b)], fill=AMBER, width=bw)   # top right
    d.line([(x0 + bw // 2, y1 - b), (x0 + bw // 2, y1 - bw // 2), (x0 + b, y1 - bw // 2)], fill=AMBER, width=bw)


def hazard(img, box, stripe):
    """The tag's hazard stripes, clipped to the box."""
    x0, y0, x1, y1 = (int(v) for v in box)
    w, h = x1 - x0, y1 - y0
    tile = Image.new('RGB', (w, h), AMBER_DK)
    td = ImageDraw.Draw(tile)
    for x in range(-h, w + h, stripe * 2):
        td.polygon([(x, h), (x + stripe, h), (x + stripe + h, 0), (x + h, 0)], fill=AMBER)
    img.paste(tile, (x0, y0))


def emblem(size):
    """The icon: an original chevron mark on a chamfered panel, in the overlay's colours."""
    s = size * 4                                  # drawn big, then reduced (antialiasing)
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    c = s // 6
    base = Image.new('RGB', (s, s))
    bd = ImageDraw.Draw(base)
    for y in range(s):
        t = y / (s - 1)
        bd.line([(0, y), (s, y)], fill=tuple(int(PANEL_TOP[i] + (BG_BOT[i] - PANEL_TOP[i]) * t) for i in range(3)))
    for x in range(0, s, s // 8):
        bd.line([(x, 0), (x, s)], fill=GRID, width=max(1, s // 128))
        bd.line([(0, x), (s, x)], fill=GRID, width=max(1, s // 128))
    shape = [(c, 0), (s - 1, 0), (s - 1, s - 1 - c), (s - 1 - c, s - 1), (0, s - 1), (0, c)]
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).polygon(shape, fill=255)
    img.paste(base, (0, 0), mask)
    d = ImageDraw.Draw(img)
    w = max(4, s // 40)
    d.line(shape + [shape[0]], fill=FRAME + (255,), width=w)
    b = s // 4
    d.line([(s - b, w * 2), (s - w * 2, w * 2), (s - w * 2, b)], fill=AMBER + (255,), width=w * 2)
    d.line([(w * 2, s - b), (w * 2, s - w * 2), (b, s - w * 2)], fill=AMBER + (255,), width=w * 2)
    # an original mark: three stacked amber chevrons over a sky-blue crosshair tick
    cx, u = s / 2, s / 100
    t = 9 * u
    for i, y in enumerate((30 * u, 47 * u, 64 * u)):
        col = AMBER if i < 2 else AMBER_DK
        d.polygon([(cx - 30 * u, y + 20 * u), (cx, y), (cx + 30 * u, y + 20 * u),
                   (cx + 30 * u, y + 20 * u + t), (cx, y + t), (cx - 30 * u, y + 20 * u + t)], fill=col + (255,))
    d.line([(cx, 14 * u), (cx, 24 * u)], fill=SKY + (255,), width=max(2, int(3 * u)))
    d.line([(16 * u, 50 * u), (22 * u, 50 * u)], fill=SKY + (255,), width=max(2, int(3 * u)))
    d.line([(78 * u, 50 * u), (84 * u, 50 * u)], fill=SKY + (255,), width=max(2, int(3 * u)))
    return img.resize((size, size), Image.LANCZOS)


def wizard_images():
    # behind every page (497:360): the overlay's backdrop and grid, darker so the page text stays easy to read
    w, h = 1491, 1080
    back = backdrop(w, h, 54)
    back = Image.blend(back, Image.new('RGB', (w, h), (0, 0, 0)), 0.30)
    back.save(INSTALLER / 'wizard_back.png')

    # the Welcome and Finished pages' side picture (164:314): a field-manual panel with the emblem and the name
    w, h = 410, 785
    side = backdrop(w, h, 41)
    panel(side, (14, 14, w - 15, h - 15), 44, bracket=46)
    d = ImageDraw.Draw(side)
    d.rectangle([40, 52, 47, 132], fill=AMBER)                       # the title's amber bar
    title, sub = font(52), font(24)
    spaced(d, (62, 50), 'ROBOTECH', title, WHITE, 2)
    spaced(d, (64, 110), 'BATTLECRY', sub, AMBER, 7)
    d.line([(40, 152), (w - 40, 152)], fill=EDGE, width=2)
    size = 236
    icon = emblem(size)
    side.paste(icon, ((w - size) // 2, 186), icon)
    rf = font(40)
    rw = spaced_width(d, 'RECOMP', rf, 10)
    spaced(d, ((w - rw) / 2, 456), 'RECOMP', rf, SKY, 10)
    d.line([(70, 516), (w - 70, 516)], fill=EDGE, width=2)
    small = font(23, bold=False)
    for i, line in enumerate(('The original Xbox game,', 'recompiled to run', 'natively on Windows')):
        lw = d.textlength(line, font=small)
        d.text(((w - lw) / 2, 540 + i * 33), line, font=small, fill=TEXT)
    tag = font(17)
    tw = spaced_width(d, 'RDF FIELD MANUAL', tag, 3)
    x0 = int((w - tw - 70) // 2)
    d.rectangle([x0, 686, x0 + tw + 22, 714], outline=AMBER, width=2)
    spaced(d, (x0 + 11, 690), 'RDF FIELD MANUAL', tag, AMBER, 3)
    hazard(side, (x0 + tw + 30, 686, x0 + tw + 70, 715), 7)
    side.save(INSTALLER / 'wizard_image.png')

    # top right of every inner page: the emblem
    for size in SMALL_SIZES:
        emblem(size).save(INSTALLER / f'wizard_small_{size}.png')


def make_icon():
    """res/robotech.ico for the game, Config.exe and Setup."""
    path = ROOT / 'res' / 'robotech.ico'
    path.parent.mkdir(exist_ok=True)
    big = emblem(256)
    big.save(path, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
    return path


def keep_symbols(version):
    """Each program and its .pdb, kept on this PC and never shipped: they read a player's crash dump
    (crashes\\RoboTechBC_<date>_<time>.dmp). out/ is git-ignored."""
    keep = ROOT / 'out' / 'symbols' / version
    keep.mkdir(parents=True, exist_ok=True)
    for name in PROGRAMS:
        for ext in ('.exe', '.pdb'):
            src = BUILD / (name + ext)
            if src.is_file():
                shutil.copyfile(src, keep / (name + ext))
            elif ext == '.pdb' and name == 'robotech_bc_recomp':
                sys.exit(f'{src} is missing: the release build makes it')
    print(f'Symbols kept in {keep}', flush=True)


def stage_runtime():
    """Beside the programs for Setup: the Visual C++ runtime the game links against (this PC's, as the test releases
    shipped it) and SDL3's license (zlib)."""
    runtime = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'vcruntime140.dll'
    if not runtime.is_file():
        sys.exit(f'{runtime} is missing')
    shutil.copyfile(runtime, BUILD / runtime.name)
    cache = ROOT / 'build' / 'CMakeCache.txt'
    m = re.search(r'^RBX_SDL3_ROOT:PATH=(.*)$', cache.read_text(errors='ignore'), re.M) if cache.is_file() else None
    if m:
        for name in ('LICENSE.txt', 'LICENSE'):
            lic = Path(m.group(1).strip()) / name
            if lic.is_file():
                shutil.copyfile(lic, BUILD / 'SDL3-LICENSE.txt')
                break


def write_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            h.update(block)
    digest = h.hexdigest()
    path.with_name(path.name + '.sha256').write_text(f'{digest}  {path.name}\n')
    return digest


# The release folder, as Setup installs it (installer/robotech.iss [Files]); rbx_import.exe is Setup's alone.
RELEASE_FILES = ('robotech_bc_recomp.exe', 'Config.exe', 'SDL3.dll', 'SDL3-LICENSE.txt', 'vcruntime140.dll')


def release_zip(version):
    """RecompLauncher installs from this zip: the programs, the readme and recomplauncher.json at the top."""
    json.loads((ROOT / 'recomplauncher.json').read_text(encoding='utf-8'))   # a file the launcher can't read is skipped
    out = OUTPUT / f'RobotechBC-Recomp-v{version}-Windows-x64.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in RELEASE_FILES:
            if (BUILD / name).is_file():
                z.write(BUILD / name, name)
            elif name != 'SDL3-LICENSE.txt':
                sys.exit(f'{BUILD / name} is missing')
        z.write(INSTALLER / 'README.txt', 'README.txt')
        z.write(ROOT / 'recomplauncher.json', 'recomplauncher.json')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version')
    parser.add_argument('--skip-build', action='store_true', help='use the programs already in build\\RelWithDebInfo')
    parser.add_argument('--iscc', help="Inno Setup 6's ISCC.exe, if it is not found by itself")
    parser.add_argument('--images-only', action='store_true', help='only redraw the wizard pictures and the icon')
    args = parser.parse_args()
    if args.images_only:
        wizard_images()
        print(make_icon())
        return
    if not args.version:
        sys.exit('--version is required')
    if args.iscc:
        os.environ['ISCC_EXE'] = args.iscc
    iscc = find_iscc()
    print(f'Inno Setup: {iscc}', flush=True)
    if not (INSTALLER / 'manifest.h').is_file():
        sys.exit('installer/manifest.h is missing: run python installer/gen_manifest.py <your disc image> first')
    cache = ROOT / 'build' / 'CMakeCache.txt'
    if cache.exists() and 'RBX_DEBUG_UNLOCK_ALL_LEVELS:BOOL=ON' in cache.read_text(errors='ignore'):
        sys.exit('this build has the debug level unlock; reconfigure without -UnlockAllLevels first')

    set_version(args.version)
    make_icon()
    if not args.skip_build:
        run('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ROOT / 'scripts' / 'build.ps1',
            '-Configuration', 'RelWithDebInfo')
    for name in PROGRAMS:
        if not (BUILD / f'{name}.exe').is_file():
            sys.exit(f'{BUILD / name}.exe is missing: build first')
    keep_symbols(args.version)
    stage_runtime()
    wizard_images()
    OUTPUT.mkdir(exist_ok=True)
    run(iscc, f'/DAppVersion={args.version}', f'/DBuildDir={BUILD}', INSTALLER / 'robotech.iss')
    setup = OUTPUT / f'RobotechBC-Recomp-v{args.version}-Windows-x64-Setup.exe'
    print(f'Setup: {setup}\nSHA-256: {write_sha256(setup)}')
    package = release_zip(args.version)
    print(f'Zip: {package}\nSHA-256: {write_sha256(package)}')

    # The hi-res texture addon, once scripts/build_texture_pack.py has written texture_payload.iss. Its .bin parts
    # (out/texture-release/revision-<n>) go on the textures-<n> release; the game Setup above offers the same pack as
    # an optional task.
    payload = INSTALLER / 'texture_payload.iss'
    if payload.is_file():
        revision = re.search(r'#define TextureVersion "(\d+)"', payload.read_text(encoding='ascii')).group(1)
        run(iscc, f'/DBuildDir={BUILD}', INSTALLER / 'robotech_textures.iss')
        addon = OUTPUT / f'RobotechBC-Recomp-HiRes-Textures-r{revision}-Windows-x64-Setup.exe'
        print(f'Texture addon: {addon}\nSHA-256: {write_sha256(addon)}')
    else:
        print('No texture_payload.iss: the game Setup has no texture task and no addon was built '
              '(run scripts/build_texture_pack.py first).')


if __name__ == '__main__':
    main()
