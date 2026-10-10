"""Draw the app icon (a white folder on a blue-violet tile with an amber plus badge) at
every size Windows asks for.

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

TILE_A, TILE_B = (64, 120, 255), (132, 60, 240)       # top-left -> bottom-right
FOLDER, FOLDER_TAB = (255, 255, 255), (222, 230, 255)
BADGE_A, BADGE_B = (255, 196, 64), (255, 128, 16)
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
    # plus badge, with a gap in the tile colour around it
    bx, by = 23.0, 23.0
    br = 7.0 if small else 6.4
    d2 = (x - bx) ** 2 + (y - by) ** 2
    if d2 <= br * br:
        arm, half = (4.6, 1.5) if small else (3.8, 1.15)
        if (abs(x - bx) <= half and abs(y - by) <= arm) or (abs(y - by) <= half and abs(x - bx) <= arm):
            return PLUS
        return mix(BADGE_A, BADGE_B, (y - by + br) / (2 * br))
    gap = br + (1.0 if small else 1.4)
    in_gap = d2 <= gap * gap
    if not in_gap:
        # folder: tab behind, body in front
        if in_rrect(x, y, 6.5, 11.5, 25.5, 24.5, 1.8):
            return FOLDER
        if in_rrect(x, y, 6.5, 8.5, 14.5, 13, 1.3) or in_tri(x, y, (14, 8.5), (17, 11.6), (14, 11.6)):
            return FOLDER_TAB
    if in_rrect(x, y, 1, 1, 31, 31, 7):
        return mix(TILE_A, TILE_B, (x + y - 2) / 60)
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
