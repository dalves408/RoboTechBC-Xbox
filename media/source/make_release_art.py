"""Release art for the Robotech: Battlecry Recomp (original artwork, overlay palette).

    python make_release_art.py <out_dir>

Everything here is drawn from scratch: an original chevron mark, original typography in Inter / JetBrains Mono,
procedural skylines and contrails. Nothing is traced from or based on franchise artwork, characters, mechs or logos.
"""
import math
import random
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else 'out')
OUT.mkdir(parents=True, exist_ok=True)

# F1 overlay palette (src/rbx_ui.c / scripts/build_installer.py)
BG_TOP, BG_BOT = (16, 30, 54), (4, 9, 20)
GRID, GRID_HI = (22, 38, 64), (30, 52, 86)
PANEL_TOP, PANEL_BOT = (14, 27, 49), (7, 14, 28)
EDGE, FRAME = (60, 98, 146), (150, 196, 240)
AMBER, AMBER_DK = (255, 168, 38), (122, 76, 14)
SKY, WHITE, TEXT, DIM = (110, 190, 255), (238, 243, 250), (196, 208, 226), (116, 138, 168)
RED = (226, 64, 52)

FONTS = '/usr/share/fonts/opentype/inter/'
MONO = '/usr/share/fonts/truetype/jetbrains-mono/'


def F(name, size):
    path = (MONO if name.startswith('JetBrains') else FONTS) + name
    return ImageFont.truetype(path, int(size))


def rgba(c, a=255):
    return tuple(c[:3]) + (int(a),)


def vgrad(w, h, stops):
    """Vertical gradient from [(t, (r,g,b)), ...] -> RGB image."""
    ts = np.linspace(0, 1, h)
    arr = np.zeros((h, 3))
    pos = [s[0] for s in stops]
    for ch in range(3):
        arr[:, ch] = np.interp(ts, pos, [s[1][ch] for s in stops])
    arr = np.repeat(arr[:, None, :], w, axis=1)
    return Image.fromarray(arr.clip(0, 255).astype(np.uint8), 'RGB')


def radial(w, h, cx, cy, r, color, power=2.0, alpha=255):
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
    a = np.clip(1 - d, 0, 1) ** power * alpha
    layer = np.zeros((h, w, 4), np.uint8)
    layer[..., :3] = color
    layer[..., 3] = a.astype(np.uint8)
    return Image.fromarray(layer, 'RGBA')


def grid_layer(w, h, cell, alpha=40, alpha_hi=70, width=1):
    g = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    for i, x in enumerate(range(0, w, cell)):
        d.line([(x, 0), (x, h)], fill=rgba(GRID_HI if i % 5 == 0 else EDGE, alpha_hi if i % 5 == 0 else alpha), width=width)
    for i, y in enumerate(range(0, h, cell)):
        d.line([(0, y), (w, y)], fill=rgba(GRID_HI if i % 5 == 0 else EDGE, alpha_hi if i % 5 == 0 else alpha), width=width)
    return g


def glow(layer, radius, strength=1.0):
    g = layer.filter(ImageFilter.GaussianBlur(radius))
    if strength != 1.0:
        a = np.array(g)
        a[..., 3] = np.clip(a[..., 3].astype(float) * strength, 0, 255).astype(np.uint8)
        g = Image.fromarray(a, 'RGBA')
    return g


def comp(base, layer, xy=(0, 0)):
    base.alpha_composite(layer, xy)
    return base


def spaced_text(text, font, gap, fill, stroke=0, stroke_fill=None):
    """RGBA image of letter-spaced text, tightly cropped."""
    tmp = ImageDraw.Draw(Image.new('L', (1, 1)))
    widths = [tmp.textlength(ch, font=font) for ch in text]
    w = int(sum(widths) + gap * (len(text) - 1) + stroke * 2 + font.size)
    h = int(font.size * 1.6 + stroke * 2)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x = stroke
    for ch, cw in zip(text, widths):
        d.text((x, stroke), ch, font=font, fill=fill, stroke_width=stroke, stroke_fill=stroke_fill)
        x += cw + gap
    return img.crop(img.getbbox())


def shear(img, k):
    """Italic lean to the right by k (x shift per unit y)."""
    w, h = img.size
    extra = int(abs(k) * h) + 2
    out = img.transform((w + extra, h), Image.AFFINE, (1, k, -extra if k > 0 else 0, 0, 1, 0), Image.BICUBIC)
    return out


def fill_gradient(mask_img, stops):
    """Recolour an RGBA image's alpha with a vertical gradient."""
    w, h = mask_img.size
    g = vgrad(w, h, stops).convert('RGBA')
    g.putalpha(mask_img.split()[3])
    return g


# ---------------------------------------------------------------- the mark

def chevrons(d, cx, top, u, cols, n=3, span=30, rise=20, thick=9, step=17):
    for i in range(n):
        y = top + i * step * u
        col = cols[min(i, len(cols) - 1)]
        d.polygon([(cx - span * u, y + rise * u), (cx, y), (cx + span * u, y + rise * u),
                   (cx + span * u, y + (rise + thick) * u), (cx, y + thick * u),
                   (cx - span * u, y + (rise + thick) * u)], fill=rgba(col))


def chamfer_shape(x0, y0, x1, y1, c):
    return [(x0 + c, y0), (x1, y0), (x1, y1 - c), (x1 - c, y1), (x0, y1), (x0, y0 + c)]


def navy_tile(s, cells=8, top=PANEL_TOP, bot=BG_BOT):
    base = vgrad(s, s, [(0, top), (1, bot)]).convert('RGBA')
    base.alpha_composite(grid_layer(s, s, s // cells, 120, 160, max(1, s // 256)))
    return base


def masked(img, shape_fn):
    s = img.size[0]
    m = Image.new('L', img.size, 0)
    shape_fn(ImageDraw.Draw(m), s)
    out = Image.new('RGBA', img.size, (0, 0, 0, 0))
    out.paste(img, (0, 0), m)
    return out


# ---------------------------------------------------------------- ten icons

def icon_01_classic(s):
    """The installer's mark: three chevrons on a chamfered navy panel, amber brackets."""
    c = s // 6
    shape = chamfer_shape(0, 0, s - 1, s - 1, c)
    img = masked(navy_tile(s), lambda d, s_: d.polygon(shape, fill=255))
    d = ImageDraw.Draw(img)
    w = max(4, s // 40)
    d.line(shape + [shape[0]], fill=rgba(FRAME), width=w)
    b = s // 4
    d.line([(s - b, w * 2), (s - w * 2, w * 2), (s - w * 2, b)], fill=rgba(AMBER), width=w * 2)
    d.line([(w * 2, s - b), (w * 2, s - w * 2), (b, s - w * 2)], fill=rgba(AMBER), width=w * 2)
    u = s / 100
    chevrons(d, s / 2, 30 * u, u, [AMBER, AMBER, AMBER_DK])
    lw = max(2, int(3 * u))
    d.line([(s / 2, 14 * u), (s / 2, 24 * u)], fill=rgba(SKY), width=lw)
    d.line([(16 * u, 50 * u), (22 * u, 50 * u)], fill=rgba(SKY), width=lw)
    d.line([(78 * u, 50 * u), (84 * u, 50 * u)], fill=rgba(SKY), width=lw)
    return img


def icon_02_roundel(s):
    """Squadron roundel: concentric rings, a single big chevron, four tick marks."""
    u = s / 100
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    tile = navy_tile(s, 10)
    img = masked(tile, lambda d, s_: d.ellipse([2 * u, 2 * u, 98 * u, 98 * u], fill=255))
    d = ImageDraw.Draw(img)
    d.ellipse([2 * u, 2 * u, 98 * u, 98 * u], outline=rgba(FRAME), width=int(3 * u))
    d.ellipse([11 * u, 11 * u, 89 * u, 89 * u], outline=rgba(AMBER), width=int(4 * u))
    d.ellipse([19 * u, 19 * u, 81 * u, 81 * u], outline=rgba(EDGE), width=int(1.2 * u))
    for a in range(0, 360, 90):
        r0, r1 = 2 * u + 4 * u, 11 * u
        x, y = math.cos(math.radians(a)), math.sin(math.radians(a))
        d.line([(50 * u + x * (48 * u - 4 * u), 50 * u + y * (48 * u - 4 * u)),
                (50 * u + x * (39 * u - 2 * u), 50 * u + y * (39 * u - 2 * u))], fill=rgba(SKY), width=int(2.5 * u))
    chevrons(d, 50 * u, 33 * u, u * 1.05, [WHITE, AMBER], n=2, span=24, rise=17, thick=10, step=19)
    return img


def icon_03_shield(s):
    """Unit crest: a shield split navy / pale blue with chevrons and three stars."""
    u = s / 100
    shape = [(14 * u, 6 * u), (86 * u, 6 * u), (86 * u, 52 * u), (50 * u, 95 * u), (14 * u, 52 * u)]
    img = masked(navy_tile(s, 8), lambda d, s_: d.polygon(shape, fill=255))
    d = ImageDraw.Draw(img)
    d.polygon([(14 * u, 6 * u), (86 * u, 6 * u), (86 * u, 24 * u), (14 * u, 24 * u)], fill=rgba(FRAME))
    for i, x in enumerate((32, 50, 68)):
        star(d, x * u, 15 * u, 5.2 * u, rgba(BG_BOT))
    d.line(shape + [shape[0]], fill=rgba(WHITE), width=int(3 * u), joint='curve')
    chevrons(d, 50 * u, 30 * u, u * 0.78, [AMBER, AMBER, AMBER_DK], span=24, rise=16, thick=8, step=14)
    return img


def star(d, cx, cy, r, fill, points=5):
    pts = []
    for i in range(points * 2):
        rr = r if i % 2 == 0 else r * 0.42
        a = -math.pi / 2 + i * math.pi / points
        pts.append((cx + math.cos(a) * rr, cy + math.sin(a) * rr))
    d.polygon(pts, fill=fill)


def icon_04_hex(s):
    """Targeting hex: hexagon frame, reticle ring and a chevron at its centre."""
    u = s / 100
    hexpts = [(50 * u + 47 * u * math.cos(math.radians(a)), 50 * u + 47 * u * math.sin(math.radians(a)))
              for a in range(-90, 270, 60)]
    img = masked(navy_tile(s, 9), lambda d, s_: d.polygon(hexpts, fill=255))
    d = ImageDraw.Draw(img)
    d.line(hexpts + [hexpts[0]], fill=rgba(SKY), width=int(3 * u), joint='curve')
    d.ellipse([22 * u, 22 * u, 78 * u, 78 * u], outline=rgba(FRAME), width=int(2 * u))
    for a in range(0, 360, 30):
        x, y = math.cos(math.radians(a)), math.sin(math.radians(a))
        r1 = 25 * u if a % 90 else 22 * u
        d.line([(50 * u + x * 28 * u, 50 * u + y * 28 * u), (50 * u + x * r1, 50 * u + y * r1)],
               fill=rgba(FRAME), width=int(1.4 * u))
    for a in (0, 90, 180, 270):
        x, y = math.cos(math.radians(a)), math.sin(math.radians(a))
        d.line([(50 * u + x * 30 * u, 50 * u + y * 30 * u), (50 * u + x * 40 * u, 50 * u + y * 40 * u)],
               fill=rgba(AMBER), width=int(3 * u))
    chevrons(d, 50 * u, 38 * u, u * 0.6, [AMBER, WHITE], n=2, span=26, rise=18, thick=10, step=18)
    return img


def icon_05_ladder(s):
    """Flight HUD: pitch ladder bars and a flight-path marker, amber on navy."""
    u = s / 100
    r = 18 * u
    img = masked(navy_tile(s, 8), lambda d, s_: d.rounded_rectangle([0, 0, s - 1, s - 1], r, fill=255))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([1.5 * u, 1.5 * u, 98.5 * u, 98.5 * u], r, outline=rgba(EDGE), width=int(2.5 * u))
    lw = int(2.6 * u)
    for i, y in enumerate((22, 36, 64, 78)):
        col = FRAME if i in (0, 3) else SKY
        dash = y > 50
        for side in (-1, 1):
            x0, x1 = 50 * u + side * 14 * u, 50 * u + side * 36 * u
            if dash:
                for k in range(3):
                    a = x0 + (x1 - x0) * k / 3
                    b = x0 + (x1 - x0) * (k + 0.6) / 3
                    d.line([(a, y * u), (b, y * u)], fill=rgba(col), width=lw)
            else:
                d.line([(x0, y * u), (x1, y * u)], fill=rgba(col), width=lw)
            d.line([(x1, y * u), (x1, (y + (4 if y < 50 else -4)) * u)], fill=rgba(col), width=lw)
    # horizon + flight path marker
    d.line([(8 * u, 50 * u), (36 * u, 50 * u)], fill=rgba(AMBER), width=int(3.4 * u))
    d.line([(64 * u, 50 * u), (92 * u, 50 * u)], fill=rgba(AMBER), width=int(3.4 * u))
    d.ellipse([43 * u, 43 * u, 57 * u, 57 * u], outline=rgba(AMBER), width=int(3.4 * u))
    d.line([(50 * u, 36 * u), (50 * u, 43 * u)], fill=rgba(AMBER), width=int(3.4 * u))
    return img


def icon_06_monogram(s):
    """Stencil monogram BC in a chamfered amber plate."""
    u = s / 100
    shape = chamfer_shape(3 * u, 3 * u, 97 * u, 97 * u, 18 * u)
    plate = vgrad(s, s, [(0, (255, 186, 70)), (1, (214, 128, 20))]).convert('RGBA')
    img = masked(plate, lambda d, s_: d.polygon(shape, fill=255))
    d = ImageDraw.Draw(img)
    d.line(shape + [shape[0]], fill=rgba(BG_BOT), width=int(2.5 * u))
    inner = chamfer_shape(9 * u, 9 * u, 91 * u, 91 * u, 14 * u)
    d.line(inner + [inner[0]], fill=rgba(AMBER_DK), width=int(1.4 * u))
    t = spaced_text('BC', F('InterDisplay-BlackItalic.otf', 56 * u), int(1 * u), rgba(BG_BOT))
    # stencil bridges
    td = ImageDraw.Draw(t)
    for fx in (0.22, 0.73):
        td.rectangle([t.width * fx, 0, t.width * fx + 2.2 * u, t.height], fill=(0, 0, 0, 0))
    td.rectangle([0, t.height * 0.47, t.width, t.height * 0.47 + 2.2 * u], fill=(0, 0, 0, 0))
    img.alpha_composite(t, (int((s - t.width) / 2), int(s * 0.50 - t.height / 2 - 4 * u)))
    f = F('JetBrainsMono-ExtraBold.ttf', 8.5 * u)
    tag = spaced_text('RECOMP', f, int(1.6 * u), rgba(BG_BOT))
    img.alpha_composite(tag, (int((s - tag.width) / 2), int(79 * u)))
    return img


def icon_07_swarm(s):
    """Missile swarm: curling contrails converging on a bright point, on a navy disc."""
    u = s / 100
    disc = vgrad(s, s, [(0, (24, 44, 78)), (1, BG_BOT)]).convert('RGBA')
    disc.alpha_composite(grid_layer(s, s, s // 10, 60, 90, max(1, s // 300)))
    img = masked(disc, lambda d, s_: d.ellipse([1 * u, 1 * u, 99 * u, 99 * u], fill=255))
    trails = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    td = ImageDraw.Draw(trails)
    rnd = random.Random(7)
    target = (66 * u, 30 * u)
    for i in range(7):
        start = (rnd.uniform(6, 40) * u, rnd.uniform(70, 96) * u)
        ctrl = (rnd.uniform(10, 95) * u, rnd.uniform(30, 95) * u)
        pts = bezier([start, ctrl, target], 60)
        for k in range(len(pts) - 1):
            t = k / len(pts)
            td.line([pts[k], pts[k + 1]], fill=rgba(WHITE, 60 + 180 * t), width=max(1, int((0.6 + 2.2 * t) * u)))
    img.alpha_composite(glow(trails, 2 * u, 1.2))
    img.alpha_composite(trails)
    img.alpha_composite(radial(s, s, target[0], target[1], 22 * u, AMBER, 1.6, 255))
    img.alpha_composite(radial(s, s, target[0], target[1], 7 * u, WHITE, 1.2, 255))
    d = ImageDraw.Draw(img)
    d.ellipse([1 * u, 1 * u, 99 * u, 99 * u], outline=rgba(FRAME), width=int(2.6 * u))
    m = Image.new('L', (s, s), 0)
    ImageDraw.Draw(m).ellipse([1 * u, 1 * u, 99 * u, 99 * u], fill=255)
    img.putalpha(ImageChops.multiply(img.split()[3], m))
    return img


def bezier(p, n):
    out = []
    for i in range(n + 1):
        t = i / n
        if len(p) == 3:
            x = (1 - t) ** 2 * p[0][0] + 2 * (1 - t) * t * p[1][0] + t * t * p[2][0]
            y = (1 - t) ** 2 * p[0][1] + 2 * (1 - t) * t * p[1][1] + t * t * p[2][1]
        else:
            x = ((1 - t) ** 3 * p[0][0] + 3 * (1 - t) ** 2 * t * p[1][0] + 3 * (1 - t) * t * t * p[2][0]
                 + t ** 3 * p[3][0])
            y = ((1 - t) ** 3 * p[0][1] + 3 * (1 - t) ** 2 * t * p[1][1] + 3 * (1 - t) * t * t * p[2][1]
                 + t ** 3 * p[3][1])
        out.append((x, y))
    return out


def icon_08_hazard(s):
    """Hazard tag: navy square, amber hazard band across the bottom, big chevrons above."""
    u = s / 100
    r = 10 * u
    img = masked(navy_tile(s, 8), lambda d, s_: d.rounded_rectangle([0, 0, s - 1, s - 1], r, fill=255))
    band = Image.new('RGBA', (s, int(22 * u)), rgba(AMBER_DK))
    bd = ImageDraw.Draw(band)
    h = band.height
    for x in range(-h, s + h, int(16 * u)):
        bd.polygon([(x, h), (x + 8 * u, h), (x + 8 * u + h, 0), (x + h, 0)], fill=rgba(AMBER))
    img.alpha_composite(band, (0, int(74 * u)))
    d = ImageDraw.Draw(img)
    d.line([(0, 74 * u), (s, 74 * u)], fill=rgba(BG_BOT), width=int(2 * u))
    chevrons(d, 50 * u, 12 * u, u * 1.15, [WHITE, SKY, EDGE], span=30, rise=20, thick=9, step=16)
    m = Image.new('L', (s, s), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, s - 1, s - 1], r, fill=255)
    img.putalpha(ImageChops.multiply(img.split()[3], m))
    ImageDraw.Draw(img).rounded_rectangle([1 * u, 1 * u, 99 * u, 99 * u], r, outline=rgba(FRAME), width=int(2 * u))
    return img


def icon_09_rank(s):
    """Minimal flat: one wide amber chevron over two pale bars, on a deep navy squircle."""
    u = s / 100
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    base = vgrad(s, s, [(0, (20, 38, 68)), (1, (6, 12, 26))]).convert('RGBA')
    img = masked(base, lambda d, s_: d.rounded_rectangle([0, 0, s - 1, s - 1], 22 * u, fill=255))
    d = ImageDraw.Draw(img)
    chevrons(d, 50 * u, 22 * u, u, [AMBER], n=1, span=34, rise=24, thick=13, step=0)
    d.rectangle([20 * u, 64 * u, 80 * u, 71 * u], fill=rgba(FRAME))
    d.rectangle([20 * u, 76 * u, 80 * u, 83 * u], fill=rgba(EDGE))
    return img


def icon_10_skyline(s):
    """Dusk skyline: original procedural city under an amber sky, chevron mark in the sky."""
    u = s / 100
    scene = render_scene(s, s, seed=4, n_trails=5, trail_target=(0.70, 0.22), horizon=0.70, cell=int(10 * u),
                         contrail_width=1.2, small=True)
    img = masked(scene, lambda d, s_: d.rounded_rectangle([0, 0, s - 1, s - 1], 18 * u, fill=255))
    d = ImageDraw.Draw(img)
    chevrons(d, 30 * u, 18 * u, u * 0.55, [AMBER, AMBER, AMBER_DK])
    d.rounded_rectangle([1 * u, 1 * u, 99 * u, 99 * u], 18 * u, outline=rgba(FRAME), width=int(2.4 * u))
    return img


ICONS = [
    ('01_field_mark', icon_01_classic, 'Field mark: the installer chevrons on a chamfered panel (matches Setup)'),
    ('02_roundel', icon_02_roundel, 'Squadron roundel: rings and one big chevron'),
    ('03_crest', icon_03_shield, 'Unit crest: shield with chevrons and three stars'),
    ('04_target_hex', icon_04_hex, 'Target hex: reticle ring around a small chevron'),
    ('05_pitch_ladder', icon_05_ladder, 'Pitch ladder: flight HUD bars and flight-path marker'),
    ('06_bc_stencil', icon_06_monogram, 'BC stencil: monogram on an amber plate'),
    ('07_missile_swarm', icon_07_swarm, 'Missile swarm: contrails converging on a flash'),
    ('08_hazard_tag', icon_08_hazard, 'Hazard tag: chevrons over a hazard-stripe band'),
    ('09_rank_flat', icon_09_rank, 'Rank flat: minimal chevron and two bars (reads best tiny)'),
    ('10_dusk_skyline', icon_10_skyline, 'Dusk skyline: original city at dusk with contrails'),
]


# ---------------------------------------------------------------- scene (backdrop, box art)

def skyline(w, h, horizon_y, rnd, layers, scale):
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    for li, (col, hmin, hmax, wmin, wmax, lit) in enumerate(layers):
        lay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(lay)
        x = -rnd.uniform(0, wmax * scale)
        base = horizon_y + li * 0.018 * h
        while x < w:
            bw = rnd.uniform(wmin, wmax) * scale
            bh = rnd.uniform(hmin, hmax) * scale
            top = base - bh
            d.rectangle([x, top, x + bw, h], fill=rgba(col))
            # stepped crown / antenna
            if rnd.random() < 0.35:
                cw = bw * rnd.uniform(0.3, 0.6)
                ch = bh * rnd.uniform(0.06, 0.16)
                cx0 = x + (bw - cw) * rnd.random()
                d.rectangle([cx0, top - ch, cx0 + cw, top], fill=rgba(col))
            if rnd.random() < 0.25:
                ax = x + bw * rnd.uniform(0.3, 0.7)
                d.line([(ax, top), (ax, top - bh * 0.25)], fill=rgba(col), width=max(1, int(2 * scale)))
                d.ellipse([ax - 3 * scale, top - bh * 0.25 - 3 * scale, ax + 3 * scale, top - bh * 0.25 + 3 * scale],
                          fill=rgba(RED, 230))
            if lit:
                ww, wh = max(1, 3.0 * scale), max(1, 4.0 * scale)
                gx, gy = 9 * scale, 11 * scale
                yy = top + gy
                while yy < h - gy:
                    xx = x + gx * 0.7
                    while xx < x + bw - gx * 0.7:
                        if rnd.random() < lit:
                            c = AMBER if rnd.random() < 0.7 else FRAME
                            d.rectangle([xx, yy, xx + ww, yy + wh], fill=rgba(c, rnd.randint(90, 230)))
                        xx += gx
                    yy += gy
            x += bw + rnd.uniform(0, 6) * scale
        out.alpha_composite(lay)
    return out


def render_scene(w, h, seed=1, n_trails=10, trail_target=(0.72, 0.30), horizon=0.74, cell=48, contrail_width=1.0,
                 small=False, reticle=None):
    rnd = random.Random(seed)
    scale = min(w, h) / 1080
    hy = h * horizon
    sky = vgrad(w, h, [(0, BG_BOT), (0.30, (12, 26, 52)), (horizon * 0.80, (34, 58, 98)),
                       (horizon * 0.95, (150, 102, 70)), (horizon, (255, 160, 70)), (1, (40, 30, 30))]).convert('RGBA')
    sky.alpha_composite(radial(w, h, w * trail_target[0], hy, w * 0.55, AMBER, 2.2, 150))
    # stars
    sd = ImageDraw.Draw(sky)
    for _ in range(int(w * h / (9000 * (1 if not small else 4)))):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h * horizon * 0.5)
        r = rnd.choice((0.6, 0.8, 1.0, 1.4)) * scale
        sd.ellipse([x - r, y - r, x + r, y + r], fill=rgba(WHITE, rnd.randint(60, 200)))
    sky.alpha_composite(grid_layer(w, h, cell, 26, 44, max(1, int(scale))))
    # contrails
    trails = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    td = ImageDraw.Draw(trails)
    heads = []
    tx, ty = w * trail_target[0], h * trail_target[1]
    for i in range(n_trails):
        side = -1 if rnd.random() < 0.7 else 1          # most launch from the left, a few from the right
        sx = (tx + side * rnd.uniform(0.15, 0.65) * w)
        sy = hy + rnd.uniform(-0.01, 0.05) * h
        e = (tx + rnd.uniform(-0.10, 0.10) * w, ty + rnd.uniform(-0.09, 0.09) * h)
        c1 = (sx - side * rnd.uniform(0.02, 0.12) * w, sy - rnd.uniform(0.25, 0.55) * (sy - ty))
        c2 = (e[0] + side * rnd.uniform(0.04, 0.20) * w, e[1] + rnd.uniform(-0.05, 0.22) * h)
        pts = bezier([(sx, sy), c1, c2, e], 140)
        for k in range(len(pts) - 1):
            t = k / len(pts)
            td.line([pts[k], pts[k + 1]], fill=rgba((230, 238, 250), 30 + 170 * t),
                     width=max(1, int((1.5 + 7 * t) * scale * contrail_width)))
        heads.append(pts[-1])
    sky.alpha_composite(glow(trails, 6 * scale, 1.3))
    sky.alpha_composite(trails)
    for hx, hy2 in heads:
        r = 26 * scale * contrail_width
        sky.alpha_composite(radial(w, h, hx, hy2, r * 2.2, AMBER, 1.8, 220))
        sky.alpha_composite(radial(w, h, hx, hy2, r * 0.7, WHITE, 1.2, 255))
    # haze + skyline
    layers = [((70, 80, 112), 70, 200, 30, 90, 0.0),
              ((34, 44, 70), 50, 150, 40, 110, 0.05),
              ((14, 22, 38), 30, 110, 50, 140, 0.16),
              ((5, 9, 18), 15, 60, 70, 190, 0.08)]
    sky.alpha_composite(skyline(w, h, hy, rnd, layers, scale))
    # ground fade
    fade = vgrad(w, h, [(0, (0, 0, 0)), (horizon, (0, 0, 0)), (1, (0, 0, 0))]).convert('RGBA')
    a = np.zeros((h, w), np.uint8)
    ys = np.clip((np.arange(h) - hy) / (h - hy + 1), 0, 1)
    a[:] = (ys[:, None] * 170).astype(np.uint8)
    fade.putalpha(Image.fromarray(a, 'L'))
    sky.alpha_composite(fade)
    return sky


def corner_brackets(img, inset, length, width, col=AMBER, alpha=230):
    w, h = img.size
    d = ImageDraw.Draw(img)
    c = rgba(col, alpha)
    for (x, y, sx, sy) in ((inset, inset, 1, 1), (w - inset, inset, -1, 1), (inset, h - inset, 1, -1),
                           (w - inset, h - inset, -1, -1)):
        d.line([(x, y + sy * length), (x, y), (x + sx * length, y)], fill=c, width=width)


def hud_reticle(w, h, cx, cy, r, scale):
    lay = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=rgba(FRAME, 70), width=int(3 * scale))
    d.ellipse([cx - r * 0.78, cy - r * 0.78, cx + r * 0.78, cy + r * 0.78], outline=rgba(SKY, 45), width=int(2 * scale))
    for a in range(0, 360, 10):
        x, y = math.cos(math.radians(a)), math.sin(math.radians(a))
        l = 0.08 if a % 30 == 0 else 0.04
        d.line([(cx + x * r, cy + y * r), (cx + x * r * (1 - l), cy + y * r * (1 - l))], fill=rgba(FRAME, 90),
               width=int(2 * scale))
    for a in (0, 90, 180, 270):
        x, y = math.cos(math.radians(a)), math.sin(math.radians(a))
        d.line([(cx + x * r * 1.04, cy + y * r * 1.04), (cx + x * r * 1.18, cy + y * r * 1.18)],
               fill=rgba(AMBER, 160), width=int(4 * scale))
    return lay


def mono_label(img, xy, text, size, col=DIM, alpha=200, gap=None):
    f = F('JetBrainsMono-Bold.ttf', size)
    t = spaced_text(text, f, gap if gap is not None else int(size * 0.25), rgba(col, alpha))
    img.alpha_composite(t, (int(xy[0]), int(xy[1])))
    return t.size


# ---------------------------------------------------------------- logo

def make_logo(width=2400, pad=40):
    s = width / 2400
    title_f = F('InterDisplay-Black.otf', 330 * s)
    title = spaced_text('ROBOTECH', title_f, int(10 * s), (255, 255, 255, 255))
    title = shear(title, 0.18)
    tw, th = title.size
    body = fill_gradient(title, [(0, (255, 255, 255)), (0.55, (220, 234, 250)), (1, FRAME)])
    # outline: dilate alpha
    a = title.split()[3]
    outline_a = a.filter(ImageFilter.MaxFilter(int(15 * s) | 1))
    outline = Image.new('RGBA', title.size, rgba(BG_BOT))
    outline.putalpha(outline_a)
    rim_a = outline_a.filter(ImageFilter.MaxFilter(int(9 * s) | 1))
    rim = Image.new('RGBA', title.size, rgba(SKY))
    rim.putalpha(rim_a)

    W = int(tw + 160 * s)
    H = int(th + 300 * s)
    logo = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ox, oy = int(60 * s), int(30 * s)
    glow_l = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    glow_l.alpha_composite(rim, (ox, oy))
    logo.alpha_composite(glow(glow_l, 22 * s, 0.9))
    logo.alpha_composite(rim, (ox, oy))
    logo.alpha_composite(outline, (ox, oy))
    logo.alpha_composite(body, (ox, oy))
    # speed lines through the title's lower third
    d = ImageDraw.Draw(logo)

    # amber bar (parallelogram) with BATTLECRY knocked out
    by0 = oy + th + int(22 * s)
    bh = int(118 * s)
    lean = bh * 0.18
    bx0, bx1 = ox + int(40 * s), ox + tw - int(10 * s)
    bar = [(bx0 + lean, by0), (bx1 + lean, by0), (bx1, by0 + bh), (bx0, by0 + bh)]
    d.polygon([(x + 8 * s, y + 8 * s) for x, y in bar], fill=rgba(BG_BOT, 200))
    d.polygon(bar, fill=rgba(AMBER))
    # chevron stack at the bar's left end (knocked out navy)
    u = bh / 100
    cx = bx0 + 95 * s
    chevrons(d, cx, by0 + 16 * u, u * 0.75, [BG_BOT, BG_BOT, AMBER_DK], span=36, rise=22, thick=14, step=24)
    sub_f = F('InterDisplay-BlackItalic.otf', 100 * s)
    sub = spaced_text('BATTLECRY', sub_f, int(44 * s), rgba(BG_BOT))
    logo.alpha_composite(sub, (int(cx + 90 * s), int(by0 + (bh - sub.height) / 2)))
    # hazard stripes at the right end of the bar
    hz0 = bx1 - int(250 * s)
    hz = Image.new('RGBA', (int(230 * s), bh), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hz)
    for x in range(-bh, hz.width + bh, int(36 * s)):
        hd.polygon([(x, bh), (x + 18 * s, bh), (x + 18 * s + bh * 0.6, 0), (x + bh * 0.6, 0)], fill=rgba(BG_BOT))
    hm = Image.new('L', hz.size, 0)
    ImageDraw.Draw(hm).polygon([(lean, 0), (hz.width, 0), (hz.width - lean, bh), (0, bh)], fill=255)
    hz.putalpha(ImageChops.multiply(hz.split()[3], hm))
    logo.alpha_composite(hz, (hz0, by0))

    # RECOMP tag under the bar, right-aligned
    tag_f = F('JetBrainsMono-ExtraBold.ttf', 58 * s)
    tag = spaced_text('RECOMP', tag_f, int(26 * s), rgba(SKY))
    tx1 = bx1 - int(10 * s)
    ty0 = by0 + bh + int(26 * s)
    tx0 = tx1 - tag.width - int(56 * s)
    d.polygon(chamfer_shape(tx0, ty0, tx1, ty0 + tag.height + int(36 * s), int(18 * s)), fill=rgba(BG_BOT, 235),
              outline=rgba(SKY))
    d.line(chamfer_shape(tx0, ty0, tx1, ty0 + tag.height + int(36 * s), int(18 * s)) +
           [chamfer_shape(tx0, ty0, tx1, ty0 + tag.height + int(36 * s), int(18 * s))[0]], fill=rgba(SKY),
           width=max(2, int(5 * s)))
    logo.alpha_composite(tag, (tx0 + int(28 * s), ty0 + int(18 * s)))
    # thin pale rule to the left of the tag
    ry = ty0 + (tag.height + int(36 * s)) // 2
    d.line([(bx0, ry), (tx0 - int(24 * s), ry)], fill=rgba(FRAME, 200), width=max(2, int(4 * s)))
    d.rectangle([bx0, ry - int(9 * s), bx0 + int(18 * s), ry + int(9 * s)], fill=rgba(AMBER))
    bb = logo.getbbox()
    logo = logo.crop((max(0, bb[0] - pad), max(0, bb[1] - pad), min(W, bb[2] + pad), min(H, bb[3] + pad)))
    return logo


def fit(img, max_w, max_h):
    r = min(max_w / img.width, max_h / img.height)
    return img.resize((int(img.width * r), int(img.height * r)), Image.LANCZOS)


# ---------------------------------------------------------------- products

def make_backdrop(w=3840, h=2160, with_logo=None):
    s = h / 2160
    scene = render_scene(w, h, seed=11, n_trails=11, trail_target=(0.70, 0.30), horizon=0.76, cell=int(64 * s))
    scene.alpha_composite(hud_reticle(w, h, w * 0.70, h * 0.32, h * 0.22, s))
    # left vignette so a logo / launcher UI reads well
    v = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    a = np.zeros((h, w), np.uint8)
    xs = np.clip(1 - np.arange(w) / (w * 0.55), 0, 1) ** 1.6
    a[:] = (xs[None, :] * 150).astype(np.uint8)
    v = Image.new('RGBA', (w, h), rgba(BG_BOT))
    v.putalpha(Image.fromarray(a, 'L'))
    scene.alpha_composite(v)
    corner_brackets(scene, int(70 * s), int(150 * s), int(6 * s), alpha=200)
    mono_label(scene, (w - 760 * s, 66 * s), 'SECTOR 07  /  GRID 64-K', 30 * s, DIM, 180)
    mono_label(scene, (w - 760 * s, h - 110 * s), 'ALT 04200  HDG 078  SPD 0.92', 30 * s, DIM, 180)
    if with_logo is not None:
        lg = fit(with_logo, w * 0.42, h * 0.40)
        scene.alpha_composite(lg, (int(150 * s), int(h * 0.30)))
    return scene.convert('RGB')


def make_boxart(w=1200, h=1800, logo=None):
    s = w / 1200
    scene = render_scene(w, h, seed=23, n_trails=9, trail_target=(0.56, 0.52), horizon=0.80, cell=int(40 * s),
                         contrail_width=1.0)
    # darken top for the logo
    top = vgrad(w, h, [(0, BG_BOT), (0.28, BG_BOT), (0.45, (0, 0, 0)), (1, (0, 0, 0))]).convert('RGBA')
    ta = np.zeros((h, w), np.uint8)
    ys = np.clip(1 - np.arange(h) / (h * 0.42), 0, 1) ** 1.2
    ta[:] = (ys[:, None] * 220).astype(np.uint8)
    top.putalpha(Image.fromarray(ta, 'L'))
    scene.alpha_composite(top)
    scene.alpha_composite(hud_reticle(w, h, w * 0.56, h * 0.52, w * 0.30, s * 0.7))
    # the big mark, glowing, centred on the convergence point
    mark = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    md = ImageDraw.Draw(mark)
    u = w / 520
    chevrons(md, w * 0.56, h * 0.52 - 36 * u, u, [AMBER, AMBER, AMBER_DK])
    scene.alpha_composite(glow(mark, 26 * s, 1.4))
    scene.alpha_composite(mark)
    # logo
    if logo is not None:
        lg = fit(logo, w * 0.86, h * 0.30)
        scene.alpha_composite(lg, (int((w - lg.width) / 2), int(70 * s)))
    # spine stripe and bottom band
    d = ImageDraw.Draw(scene)
    d.rectangle([0, 0, 18 * s, h], fill=rgba(AMBER))
    band_h = int(150 * s)
    band = vgrad(w, band_h, [(0, PANEL_TOP), (1, PANEL_BOT)]).convert('RGBA')
    scene.alpha_composite(band, (0, h - band_h))
    d = ImageDraw.Draw(scene)
    d.line([(0, h - band_h), (w, h - band_h)], fill=rgba(FRAME), width=max(2, int(3 * s)))
    hz = Image.new('RGBA', (int(210 * s), int(44 * s)), rgba(AMBER_DK))
    hd = ImageDraw.Draw(hz)
    for x in range(-50, hz.width + 50, int(22 * s)):
        hd.polygon([(x, hz.height), (x + 11 * s, hz.height), (x + 11 * s + hz.height, 0), (x + hz.height, 0)],
                   fill=rgba(AMBER))
    scene.alpha_composite(hz, (int(w - 60 * s - hz.width), int(h - band_h + 53 * s)))
    mono_label(scene, (60 * s, h - band_h + 34 * s), 'NATIVE PC RECOMPILATION', 34 * s, WHITE, 255, int(7 * s))
    mono_label(scene, (60 * s, h - band_h + 86 * s), 'WIDESCREEN  /  HIGH FRAMERATE  /  GAMEPAD', 22 * s, SKY, 230,
               int(4 * s))
    corner_brackets(scene, int(40 * s), int(70 * s), int(4 * s), alpha=210)
    return scene.convert('RGB')


def make_icon_sheet(icons):
    cols, cell = 5, 300
    W, H = cols * cell + 60, 2 * (cell + 70) + 150
    sheet = vgrad(W, H, [(0, BG_TOP), (1, BG_BOT)]).convert('RGBA')
    sheet.alpha_composite(grid_layer(W, H, 30, 40, 70))
    mono_label(sheet, (34, 34), 'ICON OPTIONS  -  PICK ONE BY NUMBER', 28, AMBER, 255, 5)
    mono_label(sheet, (34, 80), 'each shown at 256 px with 64 / 32 / 16 px previews', 18, DIM, 255, 2)
    for i, (name, img) in enumerate(icons):
        r, c = divmod(i, cols)
        x0, y0 = 30 + c * cell, 140 + r * (cell + 70)
        big = img.resize((220, 220), Image.LANCZOS)
        sheet.alpha_composite(big, (x0 + 10, y0))
        sx = x0 + 240
        for k, sz in enumerate((64, 32, 16)):
            sheet.alpha_composite(img.resize((sz, sz), Image.LANCZOS), (sx, y0 + [0, 80, 128][k]))
        mono_label(sheet, (x0 + 10, y0 + 232), name.replace('_', ' ').upper(), 17, WHITE, 255, 1)
    return sheet.convert('RGB')


def save_ico(img, path):
    img.save(path, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])


def main():
    (OUT / 'icons').mkdir(exist_ok=True)
    (OUT / 'logo').mkdir(exist_ok=True)
    (OUT / 'backdrop').mkdir(exist_ok=True)
    (OUT / 'boxart').mkdir(exist_ok=True)
    (OUT / 'icon').mkdir(exist_ok=True)

    icons = []
    for name, fn, _ in ICONS:
        big = fn(2048).resize((1024, 1024), Image.LANCZOS)
        big.save(OUT / 'icons' / f'icon_{name}_1024.png')
        save_ico(big, OUT / 'icons' / f'icon_{name}.ico')
        icons.append((name, big))
        print('icon', name, flush=True)
    make_icon_sheet(icons).save(OUT / 'icons' / '_icon_options_sheet.png')

    # game icon = option 10, dusk skyline 
    gi = icons[9][1]
    gi.save(OUT / 'icon' / 'icon_1024.png')
    gi.resize((512, 512), Image.LANCZOS).save(OUT / 'icon' / 'icon_512.png')
    gi.resize((256, 256), Image.LANCZOS).save(OUT / 'icon' / 'icon_256.png')
    save_ico(gi, OUT / 'icon' / 'robotech.ico')

    logo = make_logo(4800)
    logo.save(OUT / 'logo' / 'logo_transparent_4k.png')
    fit(logo, 1920, 1080).save(OUT / 'logo' / 'logo_transparent_1920.png')
    fit(logo, 1280, 720).save(OUT / 'logo' / 'logo_transparent_1280.png')
    print('logo', logo.size, flush=True)

    bd = make_backdrop(3840, 2160)
    bd.save(OUT / 'backdrop' / 'backdrop_3840x2160.png')
    bd.save(OUT / 'backdrop' / 'backdrop_3840x2160.jpg', quality=93)
    bd.resize((1920, 1080), Image.LANCZOS).save(OUT / 'backdrop' / 'backdrop_1920x1080.jpg', quality=93)
    # wide hero crop (3840x1240) for launchers that use a banner
    bd.crop((0, 2160 - 1240 - 300, 3840, 2160 - 300)).save(OUT / 'backdrop' / 'hero_3840x1240.jpg', quality=93)
    make_backdrop(3840, 2160, with_logo=logo).save(OUT / 'backdrop' / 'backdrop_with_logo_3840x2160.jpg', quality=93)
    print('backdrop', flush=True)

    box = make_boxart(2400, 3600, logo)
    box.save(OUT / 'boxart' / 'boxart_2400x3600.png')
    box.resize((1200, 1800), Image.LANCZOS).save(OUT / 'boxart' / 'boxart_1200x1800.png')
    box.resize((600, 900), Image.LANCZOS).save(OUT / 'boxart' / 'boxart_600x900.png')
    print('box', flush=True)


if __name__ == '__main__':
    main()
