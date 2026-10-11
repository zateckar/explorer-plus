"""Draw the app icon (the familiar yellow file folder with a green plus badge) at every
size Windows asks for. The folder fills the canvas, so the icon reads at taskbar sizes.

Usage: python -I tools/make_icon.py res/app.ico [preview-dir]

No dependencies: shapes are sampled 4x4 per pixel and every size is stored as a PNG
entry in the .ico (supported since Windows Vista). With a preview dir, each size is
also written there as a PNG.
"""
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
    out = sys.argv[1] if len(sys.argv) > 1 else 'res/app.ico'
    preview = sys.argv[2] if len(sys.argv) > 2 else None
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
