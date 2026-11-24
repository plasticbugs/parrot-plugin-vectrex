#!/usr/bin/env python3
"""mkcvp -- make a plugin file (.cvp) for the SD cart's menu.

    mkcvp.py IN.uf2 OUT.cvp --id developer.name --name NAME --version 1.0.0
             [--by DEVELOPER] [--home /FOLDER] [--opens .ext ...]
             [--uses usb-host,usb-device,psram,standalone] [--icon FILE] [--file-icon FILE]
             [--xip] [--readme README.md]

IN is an RP2350 no_flash binary packaged with a relative load map: the .uf2
that pico_package_uf2_output() makes (or `picotool uf2 convert x.elf x.uf2
-o 0x10E00000`). Its payload, from its lowest address to its highest (any
gap 0xFF, as erased flash), is the image; a 512-byte header goes before it.
The header's layout is docs/plugins.md's; plugins.c reads it.

The cart copies the image into a slot of its flash and starts it from there
(plugslot.c), so the image must load itself into SRAM from wherever it is:
its block loop (picobin) must have a load map, relative, or absolute only
from inside the image. mkcvp checks that, as the cart will.

--xip: IN is an ordinary flash binary (linked at 0x10000000) too big for
SRAM, which runs in place from its slot: the bootrom maps the slot at
0x10000000 when the menu starts it (QMI address translation). It must fit
two of the cache's 1 MB slots (2 MB less the second's 4K record); one over
1 MB less 4K takes two slots side by side.

--readme: the plugin's README, which its details page on the cart shows.
A Markdown file is turned into the cart's plain text (30 columns, ASCII:
../sdk/cvreadme.py has the rules, and prints the text to look at); a .txt
file is taken as that text already, and checked. It goes after the image,
with its offset and length in the header at 280 and 284 (docs/plugins.md),
so the image's SHA-256, and with it the cart's copy in flash, is the same
with or without one.

--uses standalone: the plugin opens files (--opens) but can also start
with none, as the Text Editor does with an empty page. The menu then lists
it with the plugins that open no files, which always run on their own, and
A starts it. Without it, a plugin that opens files is started only with
one.

An icon FILE is 8 lines of 8 characters, '#' a pixel and '.' none, as in
icons.h.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
for _d in (HERE, os.path.join(HERE, "..", "sdk")):     # cvreadme.py: beside this file, or in the SDK
    if os.path.exists(os.path.join(_d, "cvreadme.py")):
        sys.path.insert(0, _d)
        break

HDR = 512
API = 1                                    # plugins.h's CVP_API
FLAGS = {"usb-host": 1, "usb-device": 2, "psram": 4, "standalone": 8}
UF2_MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)
FAMILY_ABSOLUTE = 0xE48BFF57               # RP2350-E10's workaround block: not part of the image


def uf2_image(path):
    data = open(path, "rb").read()
    if len(data) % 512:
        sys.exit(f"{path}: not a UF2 (its size isn't a multiple of 512)")
    blocks = []
    for off in range(0, len(data), 512):
        m0, m1, flags, addr, size, _, _, family = struct.unpack_from("<8I", data, off)
        (end,) = struct.unpack_from("<I", data, off + 508)
        if (m0, m1, end) != UF2_MAGIC:
            sys.exit(f"{path}: block {off // 512} isn't a UF2 block")
        if flags & 1 or (flags & 0x2000 and family == FAMILY_ABSOLUTE):
            continue                       # "not main flash", or the E10 block
        blocks.append((addr, data[off + 32 : off + 32 + size]))
    if not blocks:
        sys.exit(f"{path}: no blocks")
    lo = min(a for a, _ in blocks)
    hi = max(a + len(b) for a, b in blocks)
    if not 0x10000000 <= lo < 0x11000000:
        sys.exit(f"{path}: starts at {lo:#x}, not in flash: package it first (pico_package_uf2_output)")
    img = bytearray(b"\xff" * (hi - lo))
    for a, b in blocks:
        img[a - lo : a - lo + len(b)] = b
    return lo, bytes(img)


SLOT_ROOM = 2 * 0x100000 - 0x1000        # plugslot.h's PS_ROOM(PS_SPAN_MAX): two slots less the record


def check_movable(path, base, img, xip=False):
    """the image's block loop, as picobin.h's picobin_walk reads it: why it
    can't start from a slot, or None. xip: it runs in place, mapped at
    0x10000000, so it needs no load map"""
    w = struct.unpack_from(f"<{len(img) // 4}I", img)
    first = next((i for i in range(min(len(w), 1024) - 1) if w[i] == 0xFFFFDED3 and w[i + 1] & 0xFF == 0x42), None)
    if first is None:
        return "no IMAGE_TYPE block in its first 4K: is it an RP2350 binary?"
    b, maps, ram = first, 0, 0
    for _ in range(8):
        i = b + 1
        while True:
            if i + 2 >= len(w):
                return "its block runs off the end"
            h = w[i]
            t = h & 0xFF
            if t == 0xFF:
                break
            sz = h >> 8 & (0xFFFF if t & 0x80 else 0xFF)
            if not sz:
                return "a block item of size 0"
            if t == 0x06:
                maps += 1
                for e in range(h >> 24 & 0x7F):
                    st, rt, z = w[i + 1 + 3 * e : i + 4 + 3 * e]
                    if h >> 31:
                        z -= rt
                        if 0x10000000 <= st < 0x14000000 and not (base <= st and st + z <= base + len(img)):
                            return f"its load map copies from {st:#x}, outside the image: make the load map relative"
                    if 0x20000000 <= rt < 0x20080000 and z:
                        ram += 1
            i += sz
        link = w[i + 1] - (1 << 32 if w[i + 1] >> 31 else 0)
        if w[i + 2] != 0xAB123579 or link % 4:
            return "its block loop is broken"
        b += link // 4
        if b == first:
            break
        if not 0 <= b < len(w) or w[b] != 0xFFFFDED3:
            return "its block loop is broken"
    else:
        return "its block loop doesn't close"
    if xip:
        if base != 0x10000000:
            return f"it starts at {base:#x}: an image that runs from its slot is linked at 0x10000000"
        if len(img) > SLOT_ROOM:
            return f"it is {len(img)} bytes: a plugin can be up to {SLOT_ROOM} (2 MB less 4K)"
        if not 0x10000000 <= w[1] < 0x10000000 + len(img):
            return f"its reset vector {w[1]:#x} isn't in the image"
        return None
    if not maps or not ram:
        return "it has no load map into SRAM: build it no_flash (pico_set_binary_type(... no_flash)) and package it (or --xip)"
    return None


def field(s, n, what):
    b = s.encode("ascii")
    if len(b) > n:
        sys.exit(f"{what} {s!r} is longer than {n} characters")
    return b + b"\0" * (n - len(b))


def icon(path):
    if not path:
        return bytes(8)
    rows = [r.strip() for r in open(path) if r.strip()]
    if len(rows) != 8 or any(len(r) != 8 or set(r) - set("#.") for r in rows):
        sys.exit(f"{path}: an icon is 8 lines of 8 '#' or '.'")
    return bytes(int(r.replace("#", "1").replace(".", "0"), 2) for r in rows)


def readme(path):
    """the README as the cart shows it: ASCII lines of 30 at most, 16384 bytes at most"""
    if not path:
        return b""
    try:
        import cvreadme
    except ImportError:
        sys.exit("--readme needs cvreadme.py (firmware/revc/sdk), beside mkcvp.py or in ../sdk")
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if path.lower().endswith(".txt"):
        text = text.replace("\r\n", "\n")
        why = cvreadme.check(text)
        if why:
            sys.exit(f"{path}: {why} (a .md file is converted)")
    else:
        text = cvreadme.convert(text)
    return text.encode("ascii")


def main():
    ap = argparse.ArgumentParser(description="Make a plugin file (.cvp) for the SD cart's menu.")
    ap.add_argument("uf2")
    ap.add_argument("out")
    ap.add_argument("--id", required=True, help="developer.pluginname: lower case, digits, . - _")
    ap.add_argument("--name", required=True, help="as the menu shows it")
    ap.add_argument("--version", required=True, help="major.minor.patch")
    ap.add_argument("--by", default="", help="the developer, as the menu shows it")
    ap.add_argument("--home", default="", help="its folder on the card (/NES)")
    ap.add_argument("--opens", nargs="*", default=[], help="extensions it opens (.nes)")
    ap.add_argument("--uses", default="", help="comma-separated: " + ", ".join(FLAGS))
    ap.add_argument("--icon", help="its icon (8x8 text)")
    ap.add_argument("--file-icon", help="its files' icon (8x8 text)")
    ap.add_argument("--xip", action="store_true", help="a flash binary that runs in place from its slot")
    ap.add_argument("--readme", help="its README (Markdown, or the cart's text as .txt)")
    a = ap.parse_args()

    if not re.fullmatch(r"[a-z0-9_-]+(\.[a-z0-9_-]+)+", a.id) or len(a.id) > 32:
        sys.exit(f"--id {a.id!r}: developer.pluginname, lower case, digits, . - _, 32 at most")
    if not re.fullmatch(r"\d+\.\d+\.\d+", a.version):
        sys.exit(f"--version {a.version!r}: major.minor.patch")
    if len(a.opens) > 8 or any(not re.fullmatch(r"\.[A-Za-z0-9]{1,7}", e) for e in a.opens):
        sys.exit("--opens: up to 8 extensions, each a dot and 1-7 letters or digits")
    flags = 0
    for u in filter(None, a.uses.split(",")):
        if u not in FLAGS:
            sys.exit(f"--uses {u!r}: one of {', '.join(FLAGS)}")
        flags |= FLAGS[u]

    base, img = uf2_image(a.uf2)
    why = check_movable(a.uf2, base, img, a.xip)
    if why:
        sys.exit(f"{a.uf2}: the cart couldn't start it from its flash: {why}")
    h = bytearray(HDR)
    h[0:8] = b"CVPLUG\0\0"
    struct.pack_into("<III", h, 8, HDR, API, len(img))
    h[20:52] = hashlib.sha256(img).digest()
    h[52:84] = field(a.id, 32, "--id")
    h[84:116] = field(a.name, 32, "--name")
    h[116:132] = field(a.version, 16, "--version")
    h[132:164] = field(a.by, 32, "--by")
    h[164:196] = field(a.home, 32, "--home")
    h[196:260] = b"".join(field(e.lower(), 8, "--opens") for e in a.opens) + bytes(8 * (8 - len(a.opens)))
    struct.pack_into("<I", h, 260, flags)
    h[264:272] = icon(a.icon)
    h[272:280] = icon(a.file_icon)
    text = readme(a.readme)
    if text:                               # after the image: offset from the file's start, length
        struct.pack_into("<II", h, 280, HDR + len(img), len(text))
    with open(a.out, "wb") as f:
        f.write(h + img + text)
    print(f"{a.out}: {a.name} {a.version} ({a.id}), image {len(img)} bytes from {base:#x}" +
          (f", README {len(text)} bytes" if text else ""))


if __name__ == "__main__":
    main()
