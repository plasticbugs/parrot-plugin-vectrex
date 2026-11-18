#!/usr/bin/env python3
"""png256.py -- the kit's PNG chores, with no library to install.

    png256.py crop IN.png OUT.png   the middle 256x192 of a bigger picture (a
                                    MAME snapshot's borders cut off)
    png256.py check FILE.png ...    is each a registry screenshot: a PNG,
                                    exactly 256x192, 100 KB at most
                                    (docs/screenshots.md)
    png256.py same A.png B.png      do two 256x192 pictures show the same
                                    thing, each pixel taken as the nearest of
                                    the console's 15 colours (emulators'
                                    palettes differ a little)

Reads 8-bit RGB, RGBA and palette PNGs that aren't interlaced, which is what
MAME, openMSX and this kit write.
"""
import struct
import sys
import zlib

W, H, MAX_BYTES = 256, 192, 100 * 1024
# the TMS9918A's colours as the SDK draws them (vdp.c's vdp_rgb); 0 and 1 are both black
TMS = [(0, 0, 0), (0, 0, 0), (33, 200, 66), (94, 220, 120), (84, 85, 237), (125, 118, 252), (212, 82, 77),
       (66, 235, 245), (252, 85, 84), (255, 121, 120), (212, 193, 84), (230, 206, 128), (33, 176, 59),
       (201, 91, 186), (204, 204, 204), (255, 255, 255)]


def read(path):
    """-> width, height, rows of (r, g, b)"""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG")
    p, idat, plte = 8, b"", None
    while p < len(d):
        n, kind = struct.unpack_from(">I4s", d, p)
        body = d[p + 8 : p + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, lace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = [tuple(body[i : i + 3]) for i in range(0, n, 3)]
        elif kind == b"IDAT":
            idat += body
        p += 12 + n
    if depth != 8 or ctype not in (2, 3, 6) or lace:
        sys.exit(f"{path}: an 8-bit RGB, RGBA or palette PNG without interlace is wanted")
    bpp = {2: 3, 3: 1, 6: 4}[ctype]
    raw, stride = zlib.decompress(idat), w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ctype == 3:
            rows.append([plte[v] for v in line])
        else:
            rows.append([tuple(line[i : i + 3]) for i in range(0, stride, bpp)])
    return w, h, rows


def write(path, rows):
    """a palette PNG when the picture has 256 colours or fewer (the console's always has)"""
    h, w = len(rows), len(rows[0])
    colours = sorted({px for r in rows for px in r})

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    if len(colours) <= 256:
        index = {c: i for i, c in enumerate(colours)}
        raw = b"".join(b"\0" + bytes(index[px] for px in r) for r in rows)
        body = chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)) + chunk(b"PLTE", bytes(v for c in colours for v in c))
    else:
        raw = b"".join(b"\0" + bytes(v for px in r for v in px) for r in rows)
        body = chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + body + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def nearest(px):
    return min(range(1, 16), key=lambda i: sum((a - b) ** 2 for a, b in zip(px, TMS[i])))


def main():
    cmd, args = (sys.argv[1] if len(sys.argv) > 1 else ""), sys.argv[2:]
    if cmd == "crop" and len(args) == 2:
        w, h, rows = read(args[0])
        if w < W or h < H:
            sys.exit(f"{args[0]}: {w}x{h} is smaller than {W}x{H}")
        x, y = (w - W) // 2, (h - H) // 2
        write(args[1], [r[x : x + W] for r in rows[y : y + H]])
        print(f"{args[1]}: {W}x{H} from the middle of {w}x{h}")
    elif cmd == "check" and args:
        bad = 0
        for path in args:
            w, h, _ = read(path)
            size = len(open(path, "rb").read())
            why = []
            if (w, h) != (W, H):
                why.append(f"it is {w}x{h}, not {W}x{H}")
            if size > MAX_BYTES:
                why.append(f"it is {size} bytes, more than {MAX_BYTES}")
            print(f"{path}: {'ok' if not why else '; '.join(why)} ({size} bytes)")
            bad += bool(why)
        sys.exit(1 if bad else 0)
    elif cmd == "same" and len(args) == 2:
        (wa, ha, a), (wb, hb, b) = read(args[0]), read(args[1])
        if (wa, ha) != (wb, hb):
            sys.exit(f"{wa}x{ha} against {wb}x{hb}")
        memo = {}
        diff = [(x, y) for y in range(ha) for x in range(wa)
                if memo.setdefault(a[y][x], nearest(a[y][x])) != memo.setdefault(b[y][x], nearest(b[y][x]))]
        if diff:
            x, y = diff[0]
            sys.exit(f"{len(diff)} pixels differ, the first at {x},{y}: {a[y][x]} against {b[y][x]}")
        print(f"the same picture ({wa}x{ha})")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
