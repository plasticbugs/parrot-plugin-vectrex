#!/usr/bin/env python3
"""bin2h_revc.py rom.bin out.h -- ROM image for the rev C cart, pre-permuted so
the bus loop is just:

    in  = sio_hw->gpio_in;                 // GP0-14 = address lines (any order)
    out = rom[in & 0x7fff];                // already in GP16-23 data-bit order
    sio_hw->gpio_out = (out << 16) | LED;  // GP16-23 -> 74HCT245 -> console D0-7

The address/data pin order comes from hw3/pins.py (ADDR_GPIO, DATA_GPIO).
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "hw3"))
import pins as N

def build(img):
    img = (img * (0x8000 // len(img) + 1))[:0x8000]
    out = bytearray(0x8000)
    for idx in range(0x8000):
        a = 0
        for g, line in enumerate(N.ADDR_GPIO):          # GPIO g carries address line ADDR_GPIO[g]
            if idx >> g & 1: a |= 1 << line
        d = img[a]
        v = 0
        for k, bit in enumerate(N.DATA_GPIO):           # GPIO 16+k drives console data bit DATA_GPIO[k]
            if d >> bit & 1: v |= 1 << k
        out[idx] = v
    return out

if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    assert len(data) <= 0x8000
    out = build(data)
    with open(sys.argv[2], "w") as f:
        f.write(f"// generated from {sys.argv[1]} for the rev C pin order (hw3/pins.py)\n")
        f.write("// addr_line[g] = cartridge address line on GPIO g (for decoding the bus log)\n")
        f.write("static const uint8_t addr_line[15] = {" + ",".join(map(str, N.ADDR_GPIO)) + "};\n")
        f.write("// data_bit[k] = console data bit carried by GPIO 16+k (for un/permuting bytes)\n")
        f.write("static const uint8_t data_bit[8] = {" + ",".join(map(str, N.DATA_GPIO)) + "};\n")
        f.write("static const uint8_t rom_image[0x8000] __attribute__((aligned(4))) = {\n")
        for i in range(0, 0x8000, 16):
            f.write("  " + ",".join(f"0x{b:02x}" for b in out[i:i+16]) + ",\n")
        f.write("};\n")
