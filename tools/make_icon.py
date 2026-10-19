"""Draw Explorer Plus' icons at every size Windows asks for: the app's (the familiar yellow
file folder with a badge in its corner), its tools' (the same folder, another badge) and
the badges the file lists put on items in sync folders. One renderer and one set of
glyphs for all of them, so they look alike.

Badges (the app's in its folder's corner; the file lists' on their own):
  plus    the app             green, "+"
  play    the media player    orange, a play triangle
  note    the Notepad         blue, lines of text
  sync    sync (and its tray icon)            teal blue, two circling arrows
  alert   sync's tray icon: needs attention   red, "!"
  paused  sync's tray icon: paused            grey, two bars
  camera  the screenshot editor               purple, a camera
  synced / syncing / error / excluded: an item in a sync folder (the file lists),
          green tick / teal arrows / red "!" / grey bar; drawn on its own with a ring in
          the list's background colour (--theme light or dark) round it

Design sets (--set): how a badge is drawn; one set for all of them
  disc     a round badge, shaded top to bottom, a gap cut round it
  tile     a rounded square, flat colour, a gap cut round it
  glyph    no badge shape: the symbol itself, coloured, larger, a gap cut round it
  sticker  a round badge, flat colour, a white rim                   (the default: Explorer Plus' icons)

Usage: python -I tools/make_icon.py [--set S] res/app.ico [preview-dir]
       python -I tools/make_icon.py [--set S] --player|--notepad|--sync|--sync-alert|
              --sync-paused|--screenshot <out.ico> [preview-dir]
       python -I tools/make_icon.py [--set S] --badge synced|syncing|error|excluded
              --theme light|dark <out.ico>
       python -I tools/make_icon.py --all [--set S]       (every .ico in res/)
       python -I tools/make_icon.py --sheet <out.html>    (every set side by side, to choose)

No dependencies: shapes are signed distances, sampled 4x4 per pixel; every size is a PNG
entry in the .ico (supported since Windows Vista).
"""
import base64
import math
import os
import struct
import sys
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
BADGE_SIZES = list(range(8, 41)) + [44, 48, 56, 64]
SUB = 4

BACK_A, BACK_B = (236, 170, 44), (214, 140, 20)       # tab and back panel, top -> bottom
FRONT_A, FRONT_B = (255, 216, 102), (252, 186, 46)    # front panel, top -> bottom
FRONT_LIP = (255, 234, 166)                           # light top edge of the front panel
WHITE = (255, 255, 255)
GREEN = ((84, 200, 84), (16, 124, 16))                # top -> bottom (Windows' success green)
ORANGE = ((255, 146, 48), (214, 70, 0))
BLUE = ((76, 160, 255), (0, 92, 196))
TEAL = ((40, 190, 230), (0, 112, 180))
RED = ((240, 90, 80), (190, 30, 30))
GREY = ((160, 160, 160), (100, 100, 100))
PURPLE = ((190, 120, 255), (120, 50, 210))
CONTENT = {'light': (255, 255, 255), 'dark': (25, 25, 25)}   # the lists' background (theme.cpp)

# badge: (colours, glyph)
BADGES = {
    'plus': (GREEN, 'plus'), 'play': (ORANGE, 'play'), 'note': (BLUE, 'lines'), 'sync': (TEAL, 'arrows'),
    'alert': (RED, 'bang'), 'paused': (GREY, 'pause'), 'camera': (PURPLE, 'camera'),
    'synced': (GREEN, 'tick'), 'syncing': (TEAL, 'arrows'), 'error': (RED, 'bang'), 'excluded': (GREY, 'minus'),
}
MODES = {'--player': ('play', 'res/player.ico'), '--notepad': ('note', 'res/notepad.ico'),
         '--sync': ('sync', 'res/sync.ico'), '--sync-alert': ('alert', 'res/sync_alert.ico'),
         '--sync-paused': ('paused', 'res/sync_paused.ico'), '--screenshot': ('camera', 'res/screenshot.ico')}
FILE_BADGES = ['synced', 'syncing', 'error', 'excluded']

# per set: the badge's radius in the folder's 32-unit grid (small sizes, larger ones), its
# symbol's weight, and the file lists' badge as a part of the icon it sits on (sync_ui.cpp)
SETS = {
    'disc':    dict(r=(11.0, 10.5), weight=1.0, list_scale=0.75),
    'tile':    dict(r=(10.5, 10.0), weight=1.0, list_scale=0.72),
    'glyph':   dict(r=(12.0, 11.5), weight=1.25, list_scale=0.80),
    'sticker': dict(r=(10.0, 9.5), weight=1.0, list_scale=0.75),
}
SET = 'sticker'


def mix(a, b, t):
    t = min(max(t, 0.0), 1.0)
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def clamp(v, a, b):
    return min(max(v, a), b)


# ---------------------------------------------------------------- signed distances
def sd_circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) - r


def sd_box(x, y, cx, cy, hw, hh, r=0.0):
    qx, qy = abs(x - cx) - hw + r, abs(y - cy) - hh + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def sd_segment(x, y, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0)
    return math.hypot(x - ax - t * dx, y - ay - t * dy)


def sd_tri(x, y, a, b, c):
    def dot(u, v):
        return u[0] * v[0] + u[1] * v[1]
    e0, e1, e2 = (b[0] - a[0], b[1] - a[1]), (c[0] - b[0], c[1] - b[1]), (a[0] - c[0], a[1] - c[1])
    v0, v1, v2 = (x - a[0], y - a[1]), (x - b[0], y - b[1]), (x - c[0], y - c[1])

    def proj(v, e):
        t = clamp(dot(v, e) / dot(e, e), 0.0, 1.0)
        return (v[0] - e[0] * t, v[1] - e[1] * t)
    pq0, pq1, pq2 = proj(v0, e0), proj(v1, e1), proj(v2, e2)
    s = 1.0 if e0[0] * e2[1] - e0[1] * e2[0] > 0 else -1.0
    dist = min(dot(pq0, pq0), dot(pq1, pq1), dot(pq2, pq2))
    side = min(s * (v0[0] * e0[1] - v0[1] * e0[0]), s * (v1[0] * e1[1] - v1[1] * e1[0]), s * (v2[0] * e2[1] - v2[1] * e2[0]))
    return -math.sqrt(dist) if side > 0 else math.sqrt(dist)


def sd_arc(x, y, rm, a0, a1):
    """distance to an arc of radius rm from angle a0 to a1 (degrees, clockwise on screen)"""
    a = math.degrees(math.atan2(y, x))
    while a < a0:
        a += 360
    if a <= a1:
        return abs(math.hypot(x, y) - rm)
    ends = [(rm * math.cos(math.radians(t)), rm * math.sin(math.radians(t))) for t in (a0, a1)]
    return min(math.hypot(x - ex, y - ey) for ex, ey in ends)


# ---------------------------------------------------------------- the symbols
class Grid:
    """the pixels a badge is drawn on: its centre `c` in pixels (whole: on a boundary, .5: on
    a pixel's middle), its radius `rpx` in pixels; bars put on it are crisp at small sizes"""
    def __init__(self, c, rpx):
        self.c, self.rpx, self.px = c, rpx, 1.0 / rpx

    def snap(self, t):                                  # a badge coordinate, to a pixel boundary
        return (round(self.c + t * self.rpx) - self.c) / self.rpx

    def centred(self, half):
        """a half-width about the centre, in whole pixels, at least one (the centre's parity)"""
        odd = abs((self.c % 1) - 0.5) < 0.25            # (a pixel's middle: an odd count is centred)
        n = max(1, round(2 * half * self.rpx))
        if (n % 2 == 1) != odd:
            n = n + 1 if n == 1 or 2 * half * self.rpx > n else n - 1
        return n / self.rpx / 2

    def whole(self, size, least=1):                     # a length, in whole pixels
        return max(least, round(size * self.rpx)) / self.rpx


def glyph(name, u, v, w, g):
    """signed distance (badge units: radius 1) to a symbol; w its weight, g its pixels"""
    if name == 'plus':
        hw, arm = g.centred(0.17 * w), g.centred(0.62)
        return min(sd_box(u, v, 0, 0, arm, hw), sd_box(u, v, 0, 0, hw, arm))
    if name == 'minus':
        return sd_box(u, v, 0, 0, g.centred(0.56), g.centred(0.16 * w))
    if name == 'pause':
        bw, gap = g.whole(0.28 * w), g.centred(0.12 * w)
        hh = g.centred(0.5)
        x0 = gap + bw / 2
        return min(sd_box(u, v, -x0, 0, bw / 2, hh), sd_box(u, v, x0, 0, bw / 2, hh))
    if name == 'bang':
        hw = g.centred(0.14 * w)
        top = g.snap(-0.62)
        dot = 2 * hw
        gap = g.whole(0.14)
        bottom = g.snap(0.62)
        mid = bottom - dot - gap
        return min(sd_box(u, v, 0, (top + mid) / 2, hw, (mid - top) / 2, hw * 0.3),
                   sd_box(u, v, 0, bottom - dot / 2, hw, dot / 2, hw * 0.45))
    if name == 'tick':
        hw = 0.15 * w
        d = min(sd_segment(u, v, -0.5, 0.02, -0.15, 0.37), sd_segment(u, v, -0.15, 0.37, 0.5, -0.32))
        return d - hw
    if name == 'play':
        return sd_tri(u, v, (-0.36, -0.56), (0.6, 0.0), (-0.36, 0.56)) - 0.04 * w
    if name == 'lines':
        th = g.whole(0.18 * w)                          # three lines, the last one short
        gap = g.whole(0.16 * w)
        top = g.snap(-(3 * th + 2 * gap) / 2)
        left, right, short = g.snap(-0.56), g.snap(0.56), g.snap(0.12)
        d = 9.0
        for i in range(3):
            y0 = top + i * (th + gap)
            r = short if i == 2 else right
            d = min(d, sd_box(u, v, (left + r) / 2, y0 + th / 2, (r - left) / 2, th / 2))
        return d
    if name == 'camera':
        body = sd_box(u, v, 0, 0.08, 0.66, 0.44, 0.14)
        bump = sd_box(u, v, -0.08, -0.38, 0.24, 0.16, 0.06)
        lens = sd_circle(u, v, 0, 0.1, 0.27)
        return max(min(body, bump), -lens)
    if name == 'arrows':
        if g.rpx < 6.5:                                 # small: one bold arrow round, a clear gap
            rm, hw = 0.46, 0.17 * w
            d = sd_arc(u, v, rm, 0, 290) - hw
            ends = [(290, 0.4, 0.5)]
        else:                                           # two arrows chasing each other
            rm, hw = 0.5, 0.13 * w
            d = min(sd_arc(u, v, rm, -160, -40), sd_arc(u, v, rm, 20, 140)) - hw
            ends = [(-40, 0.3 * w ** 0.5, 0.42), (140, 0.3 * w ** 0.5, 0.42)]
        for end, hb, ln in ends:                        # the arrowheads, on clockwise
            t = math.radians(end)
            ux, uy, tx, ty = math.cos(t), math.sin(t), -math.sin(t), math.cos(t)
            cx, cy = rm * ux, rm * uy
            d = min(d, sd_tri(u, v, (cx + ux * hb, cy + uy * hb), (cx + tx * ln, cy + ty * ln), (cx - ux * hb, cy - uy * hb)))
        return d
    raise ValueError(name)


GLYPH_FILL = 0.68        # the glyph set: its symbols this much larger (they are the badge)


def container(u, v):
    """signed distance to the badge's own shape (badge units), by the set"""
    if SET == 'tile':
        return sd_box(u, v, 0, 0, 1, 1, 0.36)
    return sd_circle(u, v, 0, 0, 1)


def badge_colour(name, u, v, d_glyph, d_shape, rim):
    """the colour at (u, v) of a badge (None: outside it); rim: the sticker's white rim's width"""
    cols, _ = BADGES[name]
    shaded = mix(cols[0], cols[1], (v + 1) / 2)
    flat = mix(cols[0], cols[1], 0.62)
    if SET == 'glyph':
        return shaded if d_glyph <= 0 else None
    if SET == 'sticker':
        if d_shape > rim:
            return None
        if d_shape > 0:
            return WHITE
        return WHITE if d_glyph <= 0 else flat
    if d_shape > 0:
        return None
    if d_glyph <= 0:
        return WHITE
    return flat if SET == 'tile' else shaded


def badge_extent(d_glyph, d_shape, rim):
    """signed distance to the badge's outline, all of it (its gap or ring goes round this)"""
    if SET == 'glyph':
        return d_glyph
    if SET == 'sticker':
        return d_shape - rim
    return d_shape


# ---------------------------------------------------------------- the folder and its badge
def badge_geometry(size):
    """centre and radius (32-unit grid) of the app icon's badge, snapped to whole pixels"""
    k = 32.0 / size
    small = size <= 24
    r = SETS[SET]['r'][0 if small else 1]
    rim = 0.0
    if SET == 'sticker':
        rim = max(1.4, k)
    diam = max(3, round(2 * (r + rim) / k))               # outer diameter, whole pixels
    outer = diam * k / 2
    c = 32.0 - outer - (0.0 if small else 0.4)
    c = round(c / k) * k if diam % 2 == 0 else (math.floor(c / k) + 0.5) * k
    return c, outer - rim, rim


def folder_shade(x, y, size):
    k = 32.0 / size
    small = size <= 24

    def snap(v):                                        # edges on pixel boundaries keep small sizes crisp
        return round(v / k) * k
    left, right, top, bottom = snap(1), snap(31), snap(11), snap(28)
    if in_rrect(x, y, left, top, right, bottom, 2.2):
        if not small and y < top + 0.9:
            return FRONT_LIP
        return mix(FRONT_A, FRONT_B, (y - top) / (bottom - top))
    back = mix(BACK_A, BACK_B, (y - 4) / (bottom - 4))
    if in_rrect(x, y, left, snap(7), right, bottom, 2.2):
        return back
    tab, tab_end = snap(4), snap(12)
    if in_rrect(x, y, left, tab, tab_end, snap(9), 1.6) or in_tri(x, y, (tab_end - 1, tab), (tab_end + 3, snap(7.5)), (tab_end - 1, snap(7.5))):
        return back
    return None


def in_rrect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def in_tri(x, y, a, b, c):
    def side(p, q):
        return (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0])
    s1, s2, s3 = side(a, b), side(b, c), side(c, a)
    return (s1 >= 0 and s2 >= 0 and s3 >= 0) or (s1 <= 0 and s2 <= 0 and s3 <= 0)


def app_shader(name, size):
    """the folder with the badge `name` in its corner (name None: the folder alone)"""
    k = 32.0 / size
    if name is None:
        return lambda x, y: folder_shade(x, y, size)
    c, r, rim = badge_geometry(size)
    if SET == 'glyph':
        r /= GLYPH_FILL
    w = SETS[SET]['weight']
    gname = BADGES[name][1]
    gap = max(1.3, k) / r                                # the gap cut round it, badge units
    grid = Grid(c / k, r / k)

    def shade(x, y):
        u, v = (x - c) / r, (y - c) / r
        if u < -2 or v < -2:
            return folder_shade(x, y, size)
        dg = glyph(gname, u, v, w, grid)
        ds = container(u, v)
        col = badge_colour(name, u, v, dg, ds, rim / r)
        if col:
            return col
        if badge_extent(dg, ds, rim / r) <= gap:
            return None
        return folder_shade(x, y, size)
    return shade


def badge_shader(name, size, theme):
    """a file list's badge on its own: the badge and a ring of the list's background round it"""
    k = 1.0 / size                                      # (a unit square)
    w = SETS[SET]['weight']
    gname = BADGES[name][1]
    ring = max(1.0, size / 11.0) * k                    # its ring (or the sticker's rim): whole-ish pixels
    rim = ring if SET == 'sticker' else 0.0
    outer = 0.5 - (0.0 if SET == 'sticker' else ring)   # the badge, inside its ring
    r = outer - rim
    if SET == 'glyph':
        r /= GLYPH_FILL
    c = 0.5
    back = CONTENT[theme]
    grid = Grid(size / 2, r * size)

    def shade(x, y):
        u, v = (x - c) / r, (y - c) / r
        dg = glyph(gname, u, v, w, grid)
        ds = container(u, v)
        col = badge_colour(name, u, v, dg, ds, rim / r)
        if col:
            return col
        if SET != 'sticker' and badge_extent(dg, ds, 0) <= ring / r:
            return back
        return None
    return shade, 1.0


# ---------------------------------------------------------------- pixels, PNG, ICO
def render(shade, size, unit=32.0):
    rows = []
    k = unit / size
    for py in range(size):
        row = bytearray([0])                            # PNG filter: none
        for px in range(size):
            acc, n = [0.0, 0.0, 0.0], 0
            for sy in range(SUB):
                for sx in range(SUB):
                    col = shade((px + (sx + 0.5) / SUB) * k, (py + (sy + 0.5) / SUB) * k)
                    if col:
                        n += 1
                        for i in range(3):
                            acc[i] += col[i]
            if n:
                row += bytes(int(acc[i] / n + 0.5) for i in range(3)) + bytes([255 * n // (SUB * SUB)])
            else:
                row += b'\0\0\0\0'
        rows.append(bytes(row))
    return png(size, b''.join(rows))


def png(size, raw):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)   # 8-bit RGBA
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


def write_ico(out, sizes, images):
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b'', b''
    for s, img in zip(sizes, images):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(img), offset + len(data))
        data += img
    with open(out, 'wb') as f:
        f.write(header + entries + data)
    print('wrote', out, len(header + entries + data), 'bytes')


def app_icon(name, sizes=SIZES):
    return [render(app_shader(name, s), s) for s in sizes]


def file_badge(name, theme, sizes=BADGE_SIZES):
    out = []
    for s in sizes:
        shade, unit = badge_shader(name, s, theme)
        out.append(render(shade, s, unit))
    return out


# ---------------------------------------------------------------- the sheet to choose from
def doc_shader(size):
    """a plain document (the sheet's file rows)"""
    k = 32.0 / size

    def snap(v):
        return round(v / k) * k
    l, r, t, b, fold = snap(6), snap(26), snap(2), snap(30), snap(7)

    def shade(x, y):
        if not (l <= x <= r and t <= y <= b) or (x > r - fold and y < t + fold and (x - (r - fold)) > (y - t)):
            return None
        edge = max(k, 1.0)
        if x < l + edge or x > r - edge or y > b - edge or y < t + edge:
            return (140, 140, 140)
        if x > r - fold - edge and y < t + fold + edge and abs((x - (r - fold)) - (y - t)) < edge:
            return (140, 140, 140)
        return (250, 250, 250)
    return shade


def sheet(out):
    global SET
    uri = lambda p: 'data:image/png;base64,' + base64.b64encode(p).decode()
    apps = ['plus', 'play', 'note', 'camera', 'sync', 'alert', 'paused']
    names = {'plus': 'Explorer Plus', 'play': 'Player', 'note': 'Notepad', 'camera': 'Screenshot',
             'sync': 'Sync', 'alert': 'Sync: attention', 'paused': 'Sync: paused'}
    states = {'synced': 'Up to date', 'syncing': 'Syncing', 'error': 'Error / conflict', 'excluded': 'Not synced'}
    # the current icons (res/*.ico as committed), for comparison
    cur = {}
    for n, f in (('plus', 'app'), ('play', 'player'), ('note', 'notepad'), ('camera', 'screenshot'), ('sync', 'sync'),
                 ('alert', 'sync_alert'), ('paused', 'sync_paused')):
        cur[n] = read_ico(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'res', f + '.ico'))
    folder = {s: uri(render(app_shader(None, s), s)) for s in (16, 20, 24, 32)}
    doc = {s: uri(render(doc_shader(s), s)) for s in (16, 20, 24, 32)}
    html = [SHEET_HEAD]
    blocks = []
    for st in ['current'] + list(SETS):
        if st != 'current':
            SET = st
        print('sheet:', st, flush=True)
        title = {'current': 'Now (for comparison)', 'disc': 'A - Disc', 'tile': 'B - Tile', 'glyph': 'C - Glyph',
                 'sticker': 'D - Sticker'}[st]
        blurb = {'current': 'The icons as they are; the file lists\' badges are drawn small, without smoothing.',
                 'disc': 'Round badges, shaded, larger; a gap cut round them. The closest to now.',
                 'tile': 'Rounded squares, flat colours, larger; a gap cut round them.',
                 'glyph': 'No badge shape: the symbol itself, coloured and larger, a gap cut round it.',
                 'sticker': 'Round badges, flat colours, larger, with a white rim: stand out on anything.'}[st]
        b = ['<section><h2>%s</h2><p>%s</p>' % (title, blurb)]
        b.append('<div class="apps">')
        for n in apps:
            imgs = {}
            for s in (16, 20, 24, 32, 48, 64):
                imgs[s] = cur[n].get(s) if st == 'current' else render(app_shader(n, s), s)
            b.append('<figure><div class="row">%s</div><div class="zoom">%s</div><figcaption>%s</figcaption></figure>' % (
                ''.join('<img class="px" src="%s" data-s="%d">' % (uri(imgs[s]), s) for s in (16, 20, 24, 32, 48, 64) if imgs[s]),
                ''.join('<img class="z" src="%s" data-s="%d" data-z="4">' % (uri(imgs[s]), s) for s in (16, 24) if imgs[s]),
                names[n]))
        b.append('</div>')
        if st != 'current':
            b.append('<div class="lists">')
            for theme in ('light', 'dark'):
                for icon in (16, 24):
                    bs = round(icon * SETS[SET]['list_scale'])
                    rows = []
                    for i, (state, label) in enumerate(states.items()):
                        shade, unit = badge_shader(state, bs, theme)
                        bimg = uri(render(shade, bs, unit))
                        base = folder[icon] if i % 2 == 0 else doc[icon]
                        rows.append('<div class="item" data-icon="%d" data-badge="%d"><span class="ic"><img class="px" src="%s" data-s="%d">'
                                    '<img class="px b" src="%s" data-s="%d"></span><span>%s %s</span></div>' % (
                                        icon, bs, base, icon, bimg, bs, 'Folder' if i % 2 == 0 else 'Report.txt', '- ' + label))
                    b.append('<div class="list %s"><div class="cap">%s, %d px icons (%d%% scale)</div>%s</div>' % (
                        theme, theme.capitalize(), icon, icon * 100 // 16, ''.join(rows)))
            b.append('</div>')
        b.append('</section>')
        blocks.append(''.join(b))
    SET = 'sticker'
    html += blocks
    html.append(SHEET_TAIL)
    with open(out, 'w', encoding='utf-8') as f:
        f.write(''.join(html))
    print('wrote', out)


def read_ico(path):
    """{size: png bytes} of an .ico made here (PNG entries)"""
    out = {}
    try:
        with open(path, 'rb') as f:
            d = f.read()
    except OSError:
        return out
    n = struct.unpack('<H', d[4:6])[0]
    for i in range(n):
        w, h, _, _, _, _, ln, off = struct.unpack('<BBBBHHII', d[6 + 16 * i:22 + 16 * i])
        out[w or 256] = d[off:off + ln]
    return out


SHEET_HEAD = '''<!doctype html><meta charset="utf-8"><title>Explorer Plus - icon sets</title>
<style>
body{font:14px "Segoe UI",sans-serif;margin:24px;background:#f3f3f3;color:#1b1b1b}
h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:0 0 2px}p{margin:0 0 12px;color:#555}
section{background:#fff;border-radius:8px;padding:16px 20px;margin:0 0 18px;box-shadow:0 1px 3px #0002}
.apps{display:flex;flex-wrap:wrap;gap:18px 26px}
figure{margin:0}figcaption{font-size:12px;color:#555;margin-top:4px}
.row{display:flex;align-items:flex-end;gap:8px;padding:8px;border-radius:6px;background:linear-gradient(90deg,#e9e9e9 50%,#202020 50%)}
.zoom{display:flex;gap:8px;margin-top:6px}.z{image-rendering:pixelated;background:#e9e9e9;border-radius:4px}
.lists{display:flex;flex-wrap:wrap;gap:14px;margin-top:16px}
.list{padding:8px 0;border-radius:6px;min-width:250px}.list.light{background:#fff;border:1px solid #ddd}
.list.dark{background:#191919;color:#fff}.cap{font-size:11px;opacity:.6;padding:0 12px 6px}
.item{display:flex;align-items:center;gap:10px;padding:3px 12px 3px 18px}.item:nth-child(3){background:#cce8ff}
.list.dark .item:nth-child(3){background:#3a5a78}.ic{position:relative;display:inline-block}.b{position:absolute}
</style>
<h1>Explorer Plus - icon sets</h1>
<p>Each set: the app's icons (light and dark backgrounds, at 16 to 64 pixels, and 16 and 24 four times enlarged),
then a file list's sync badges (on folders and files; the second row selected). Pixels are shown as they are on your screen.</p>
'''
SHEET_TAIL = '''<script>
// pixels as the screen has them: a 16-pixel icon is 16 device pixels, whatever the scaling
const r = window.devicePixelRatio || 1;
for (const i of document.querySelectorAll('img.px')) { i.style.width = i.dataset.s / r + 'px'; i.style.height = i.dataset.s / r + 'px'; }
for (const i of document.querySelectorAll('img.z')) { const s = i.dataset.s * i.dataset.z / r; i.style.width = s + 'px'; i.style.height = s + 'px'; }
for (const it of document.querySelectorAll('.item')) {
  const ic = +it.dataset.icon, bs = +it.dataset.badge, b = it.querySelector('.b');
  // as sync_ui.cpp puts it: at the icon's bottom left, a sixth of it past the icon's edges
  const o = Math.round(bs / 6);
  b.style.left = (-o) / r + 'px'; b.style.top = (ic - bs + o) / r + 'px';
  it.style.minHeight = (ic + 8) / r + 'px';
}
</script>
'''


def main():
    global SET
    args = sys.argv[1:]
    if '--set' in args:
        i = args.index('--set')
        SET = args[i + 1]
        assert SET in SETS, 'sets: ' + ', '.join(SETS)
        del args[i:i + 2]
    if args[:1] == ['--sheet']:
        sheet(args[1] if len(args) > 1 else 'icon-sets.html')
        return
    if args[:1] == ['--all']:
        res = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'res')
        write_ico(os.path.join(res, 'app.ico'), SIZES, app_icon('plus'))
        for mode, (name, path) in MODES.items():
            write_ico(os.path.join(res, os.path.basename(path)), SIZES, app_icon(name))
        for b in FILE_BADGES:
            for theme in ('light', 'dark'):
                write_ico(os.path.join(res, 'badge_%s_%s.ico' % (b, theme)), BADGE_SIZES, file_badge(b, theme))
        return
    if args[:1] == ['--badge']:
        name, args = args[1], args[2:]
        theme = 'light'
        if args[:1] == ['--theme']:
            theme, args = args[1], args[2:]
        write_ico(args[0], BADGE_SIZES, file_badge(name, theme))
        return
    name, default = 'plus', 'res/app.ico'
    if args[:1] and args[0] in MODES:
        name, default = MODES[args[0]]
        args = args[1:]
    out = args[0] if args else default
    preview = args[1] if len(args) > 1 else None
    images = app_icon(name)
    write_ico(out, SIZES, images)
    if preview:
        for s, img in zip(SIZES, images):
            with open(os.path.join(preview, 'icon%d.png' % s), 'wb') as f:
                f.write(img)


if __name__ == '__main__':
    main()
