"""Draw the app icon (the familiar yellow file folder with a green plus badge) at every
size Windows asks for. The folder fills the canvas, so the icon reads at taskbar sizes.
With --player: the media player's icon, the same folder with an orange play badge; with
--notepad: the Notepad's, a blue badge with lines of text; with --sync: sync's, a blue
badge with two circling arrows (--sync-alert: a red badge with "!", --sync-paused: a grey
one with two bars, for the tray); with --screenshot: the screenshot editor's, a purple
badge with a camera.

Usage: python -I tools/make_icon.py res/app.ico [preview-dir]
       python -I tools/make_icon.py --player res/player.ico [preview-dir]
       python -I tools/make_icon.py --notepad res/notepad.ico [preview-dir]
       python -I tools/make_icon.py --sync res/sync.ico [preview-dir]
       python -I tools/make_icon.py --sync-alert res/sync_alert.ico [preview-dir]
       python -I tools/make_icon.py --sync-paused res/sync_paused.ico [preview-dir]
       python -I tools/make_icon.py --screenshot res/screenshot.ico [preview-dir]

No dependencies: shapes are sampled 4x4 per pixel and every size is stored as a PNG
entry in the .ico (supported since Windows Vista). With a preview dir, each size is
also written there as a PNG.
"""
import math
import os
import struct
import sys
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SUB = 4

BACK_A, BACK_B = (236, 170, 44), (214, 140, 20)       # tab and back panel, top -> bottom
FRONT_A, FRONT_B = (255, 216, 102), (252, 186, 46)    # front panel, top -> bottom
FRONT_LIP = (255, 234, 166)                           # light top edge of the front panel
BADGE_A, BADGE_B = (84, 200, 84), (16, 124, 16)        # top -> bottom (Windows' success green)
PLUS = (255, 255, 255)
PLAY_A, PLAY_B = (255, 146, 48), (214, 70, 0)         # the player's badge, top -> bottom
NOTE_A, NOTE_B = (76, 160, 255), (0, 92, 196)         # the Notepad's badge, top -> bottom
SYNC_A, SYNC_B = (40, 190, 230), (0, 112, 180)        # sync's badge (teal blue), top -> bottom
ALERT_A, ALERT_B = (240, 90, 80), (190, 30, 30)       # sync needs attention
PAUSED_A, PAUSED_B = (160, 160, 160), (100, 100, 100) # sync paused
SHOT_A, SHOT_B = (190, 120, 255), (120, 50, 210)      # the screenshot editor's badge
BADGE = 'plus'                                        # plus, play, note, sync, alert, paused, camera
MODES = {'--player': ('play', 'res/player.ico'), '--notepad': ('note', 'res/notepad.ico'),
         '--sync': ('sync', 'res/sync.ico'), '--sync-alert': ('alert', 'res/sync_alert.ico'),
         '--sync-paused': ('paused', 'res/sync_paused.ico'), '--screenshot': ('camera', 'res/screenshot.ico')}


def mix(a, b, t):
    t = min(max(t, 0.0), 1.0)
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


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


def shade(x, y, size):
    """colour at a point of the 32-unit grid, or None (transparent); front to back"""
    small = size <= 24                      # bolder shapes where pixels are scarce
    k = 32.0 / size                         # grid units per pixel

    def snap(v):                            # edges on pixel boundaries keep small sizes crisp
        return round(v / k) * k

    # plus badge, cut out of the folder by a transparent ring; the plus bars are a whole
    # number of pixels wide, centred on a pixel boundary (even width) or centre (odd)
    br = 9.0 if small else 8.5
    bar = max(2, round((3.8 if small else 3.4) / k))
    bx = by = 32.0 - br - (0.5 if small else 0.8) + (k / 2 if bar % 2 else 0.0)
    d2 = (x - bx) ** 2 + (y - by) ** 2
    if d2 <= br * br:
        def fill(a, b):
            return mix(a, b, (y - by + br) / (2 * br))
        if BADGE == 'play':                 # a play triangle, a little right of centre (optically centred)
            if in_tri(x, y, (bx - br * 0.36, by - br * 0.52), (bx - br * 0.36, by + br * 0.52), (bx + br * 0.56, by)):
                return PLUS
            return mix(PLAY_A, PLAY_B, (y - by + br) / (2 * br))
        if BADGE == 'sync':                 # two arcs chasing each other clockwise, arrowheads at their ends
            r0, r1 = br * (0.34 if small else 0.36), br * (0.66 if small else 0.62)
            rm, d = (r0 + r1) / 2, math.sqrt(d2)
            a = math.degrees(math.atan2(y - by, x - bx))      # screen coordinates: clockwise grows
            if r0 <= d <= r1 and (-165 <= a <= -35 or 15 <= a <= 145):
                return PLUS
            for end in (-35, 145):                            # the arrowheads, pointing on clockwise
                t = math.radians(end)
                ux, uy, tx, ty = math.cos(t), math.sin(t), -math.sin(t), math.cos(t)
                cx, cy = bx + rm * ux, by + rm * uy
                hw, ln = br * (0.36 if small else 0.32), br * 0.42
                if in_tri(x, y, (cx + ux * hw, cy + uy * hw), (cx - ux * hw, cy - uy * hw), (cx + tx * ln, cy + ty * ln)):
                    return PLUS
            return fill(SYNC_A, SYNC_B)
        if BADGE == 'alert':                # "!": a bar and a dot
            half = max(1, round(br * 0.26 / k)) * k / 2
            if abs(x - bx) <= half and (by - br * 0.6 <= y <= by + br * 0.14 or by + br * 0.34 <= y <= by + br * 0.34 + 2 * half):
                return PLUS
            return fill(ALERT_A, ALERT_B)
        if BADGE == 'paused':               # two bars
            w = max(1, round(br * 0.22 / k)) * k
            gap = max(1, round(br * 0.2 / k)) * k
            l0 = snap(bx - gap / 2 - w)                      # bar edges on pixel boundaries
            r0 = l0 + w + gap
            if abs(y - by) <= br * 0.46 and (l0 <= x < l0 + w or r0 <= x < r0 + w):
                return PLUS
            return fill(PAUSED_A, PAUSED_B)
        if BADGE == 'camera':               # a body with a bump on top and the lens cut out of it
            lens = br * (0.24 if small else 0.26)
            ld2 = (x - bx) ** 2 + (y - by - br * 0.08) ** 2
            body = in_rrect(x, y, bx - br * 0.62, by - br * 0.36, bx + br * 0.62, by + br * 0.48, br * 0.14)
            bump = in_rrect(x, y, bx - br * 0.28, by - br * 0.56, bx + br * 0.16, by - br * 0.2, br * 0.08)
            if (body or bump) and not ld2 <= lens * lens:
                return PLUS
            return fill(SHOT_A, SHOT_B)
        if BADGE == 'note':                 # three lines, the last one short; whole pixels thick
            lh = max(1, round(br * 0.2 / k)) * k
            y0 = snap(by - 2.5 * lh)
            w = br * 0.56
            for i in range(3):
                top = y0 + 2 * i * lh
                if top <= y < top + lh and bx - w <= x <= bx + (w if i < 2 else w * 0.15):
                    return PLUS
            return mix(NOTE_A, NOTE_B, (y - by + br) / (2 * br))
        half, arm = bar * k / 2, snap(br * 0.74)
        if (abs(x - bx) <= half and abs(y - by) <= arm) or (abs(y - by) <= half and abs(x - bx) <= arm):
            return PLUS
        return mix(BADGE_A, BADGE_B, (y - by + br) / (2 * br))
    gap = br + max(1.3, k)
    if d2 <= gap * gap:
        return None
    # folder: front panel, then the back panel and its tab
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


def render(size):
    rows = []
    k = 32.0 / size
    for py in range(size):
        row = bytearray([0])                         # PNG filter: none
        for px in range(size):
            acc, n = [0.0, 0.0, 0.0], 0
            for sy in range(SUB):
                for sx in range(SUB):
                    c = shade((px + (sx + 0.5) / SUB) * k, (py + (sy + 0.5) / SUB) * k, size)
                    if c:
                        n += 1
                        for i in range(3):
                            acc[i] += c[i]
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


def main():
    global BADGE
    args = sys.argv[1:]
    default = 'res/app.ico'
    if args[:1] and args[0] in MODES:
        BADGE, default = MODES[args[0]]
        args = args[1:]
    out = args[0] if args else default
    preview = args[1] if len(args) > 1 else None
    images = [render(s) for s in SIZES]
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b'', b''
    for s, img in zip(SIZES, images):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(img), offset + len(data))
        data += img
        if preview:
            with open(os.path.join(preview, 'icon%d.png' % s), 'wb') as f:
                f.write(img)
    with open(out, 'wb') as f:
        f.write(header + entries + data)
    print('wrote', out, len(header + entries + data), 'bytes')


if __name__ == '__main__':
    main()
