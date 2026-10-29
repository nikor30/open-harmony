#!/usr/bin/env python3
"""Replace text strings in a Harmony One application image.

Usage: patchstr.py in.bin out.bin "Old text=New text" [...]

Each old text must occur exactly once as a NUL-terminated string. The new
text may not be longer; it is padded with spaces so no code or data moves.
The header checksum (+0, XOR of byte pairs +4..end seeded 21 43) is
recomputed. Check the result with `flashapp check out.bin`.
"""
import sys


def checksum(img):
    a, b = 0x21, 0x43
    for i in range(4, len(img), 2):
        a ^= img[i]
        b ^= img[i + 1]
    return a, b


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip())
    img = bytearray(open(sys.argv[1], "rb").read())
    if img[8:10] != b"HG" or checksum(img) != (img[0], img[1]):
        sys.exit("input is not a valid application image")

    for pair in sys.argv[3:]:
        old, sep, new = pair.partition("=")
        if not sep:
            sys.exit(f"expected Old=New, got {pair!r}")
        needle = b"\0" + old.encode("latin-1") + b"\0"
        hits = [i for i in range(len(img)) if img.startswith(needle, i)]
        if len(hits) != 1:
            sys.exit(f"{old!r} found {len(hits)} times, need exactly 1")
        if len(new) > len(old):
            sys.exit(f"{new!r} is longer than {old!r}")
        start = hits[0] + 1
        img[start:start + len(old)] = new.ljust(len(old)).encode("latin-1")
        print(f"0x{0x20000 + start:06x}: {old!r} -> {new.ljust(len(old))!r}")

    img[0], img[1] = checksum(img)
    open(sys.argv[2], "wb").write(img)
    print(f"checksum {img[0]:02X} {img[1]:02X}, {len(img)} bytes -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
