#!/usr/bin/env python3
"""cvpinfo.py FILE.CVP [--version] -- what a plugin file says about itself
(the header's layout is in docs/plugin-rules.md and the SDK's mkcvp.py).
--version prints the version alone, for the release workflow."""
import hashlib
import struct
import sys


def text(b):
    return b.split(b"\0")[0].decode("ascii")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        sys.exit(__doc__)
    d = open(args[0], "rb").read()
    if d[:8] != b"CVPLUG\0\0" or len(d) < 512:
        sys.exit(f"{args[0]}: not a plugin file")
    hdr, api, size = struct.unpack_from("<III", d, 8)
    if hashlib.sha256(d[hdr : hdr + size]).digest() != d[20:52]:
        sys.exit(f"{args[0]}: the image's SHA-256 isn't the header's")
    version = text(d[116:132])
    if "--version" in sys.argv:
        print(version)
        return
    readme_at, readme_len = struct.unpack_from("<II", d, 280)
    exts = [text(d[196 + 8 * i : 204 + 8 * i]) for i in range(8)]
    (flags,) = struct.unpack_from("<I", d, 260)
    uses = [n for bit, n in ((1, "usb-host"), (2, "usb-device"), (4, "psram")) if flags & bit]
    print(f"{args[0]}: {text(d[84:116])} {version} ({text(d[52:84])}) by {text(d[132:164]) or '?'}")
    print(f"  API {api}, image {size} bytes, home {text(d[164:196]) or '-'}, opens {' '.join(e for e in exts if e) or '-'}, uses {' '.join(uses) or '-'}")
    print(f"  README {readme_len} bytes" if readme_at else "  no README")
    for name, at in (("icon", 264), ("file icon", 272)):
        if any(d[at : at + 8]):
            print(f"  {name}:")
            for row in d[at : at + 8]:
                print("    " + "".join("#" if row >> (7 - b) & 1 else "." for b in range(8)))


if __name__ == "__main__":
    main()
