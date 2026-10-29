#!/usr/bin/env python3
"""Build the colorbars mod onto a stock 3.4 application image.

Usage: build.py app-3.4.bin out.bin

Assembles colorbars.asm with gpasm, appends it after the image (2-byte
aligned), redirects the CALL 0x2112C at 0x28A42 to it, updates the length
at +4 and the checksum at +0. Refuses anything that is not the expected
stock image.
"""
import os
import subprocess
import sys
import tempfile

BASE = 0x20000
HOOK = 0x28A42                      # CALL 0x2112C in main
ORIG_CALL = bytes.fromhex("96ec08f1")
STOCK_LEN = 0xEA92                  # 3.4 image size


def checksum(img):
    a, b = 0x21, 0x43
    for i in range(4, len(img), 2):
        a ^= img[i]
        b ^= img[i + 1]
    return a, b


def call(addr):
    k = addr >> 1
    w1 = 0xEC00 | (k & 0xFF)
    w2 = 0xF000 | ((k >> 8) & 0xFFF)
    return bytes([w1 & 0xFF, w1 >> 8, w2 & 0xFF, w2 >> 8])


def assemble():
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "colorbars.hex")
        subprocess.run(["gpasm", "-p", "18f87j50", "-o", out,
                        os.path.join(here, "colorbars.asm")], check=True)
        code = bytearray()
        upper = 0
        for line in open(out):
            n, addr, rtype = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
            data = bytes.fromhex(line[9:9 + 2 * n])
            if rtype == 4:
                upper = int.from_bytes(data, "big") << 16
            elif rtype == 0 and upper + addr < 0x300000:  # skip config words
                a = upper + addr
                code[len(code):a + n] = b"\xff" * max(0, a + n - len(code))
                code[a:a + n] = data
        return bytes(code)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    img = bytearray(open(sys.argv[1], "rb").read())
    if (len(img) != STOCK_LEN or img[8:10] != b"HG" or img[7] != 0x34
            or checksum(img) != (img[0], img[1])):
        sys.exit("input is not the stock 3.4 image")
    if img[HOOK - BASE:HOOK - BASE + 4] != ORIG_CALL:
        sys.exit("unexpected bytes at the hook 0x28A42")

    code = assemble()
    tramp = BASE + len(img)
    img += code + (b"\xff" if len(code) % 2 else b"")
    img[HOOK - BASE:HOOK - BASE + 4] = call(tramp)
    length = len(img) - 8
    img[4], img[5] = length & 0xFF, length >> 8
    img[0], img[1] = checksum(img)
    open(sys.argv[2], "wb").write(img)
    print(f"trampoline {len(code)} bytes at 0x{tramp:06x}, hook 0x{HOOK:06x} -> "
          f"call 0x{tramp:06x}, image {len(img)} bytes -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
