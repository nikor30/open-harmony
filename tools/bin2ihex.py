#!/usr/bin/env python3
"""Write a raw binary as Intel HEX at a given base address (for gpdasm).

Usage: bin2ihex.py in.bin out.hex base_address

Trailing 0xFF (erased flash) is dropped.
"""
import sys


def record(rtype, addr, data):
    body = bytes([len(data), addr >> 8 & 0xFF, addr & 0xFF, rtype]) + data
    return ":%s%02X\n" % (body.hex().upper(), -sum(body) & 0xFF)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    data = open(sys.argv[1], "rb").read().rstrip(b"\xff")
    base = int(sys.argv[3], 0)
    upper = None
    with open(sys.argv[2], "w") as out:
        for off in range(0, len(data), 16):
            addr = base + off
            if addr >> 16 != upper:
                upper = addr >> 16
                out.write(record(4, 0, upper.to_bytes(2, "big")))
            out.write(record(0, addr & 0xFFFF, data[off:off + 16]))
        out.write(record(1, 0, b""))
    print(f"{len(data)} bytes at 0x{base:x} -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
