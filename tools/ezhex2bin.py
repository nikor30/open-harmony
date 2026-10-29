#!/usr/bin/env python3
"""Convert a concordance .EZHex firmware dump to a raw binary.

Usage: ezhex2bin.py dump.EZHex out.bin
"""
import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    text = open(sys.argv[1]).read()
    data = bytes.fromhex("".join(re.findall(r"<DATA>([0-9A-Fa-f]*)</DATA>", text)))
    open(sys.argv[2], "wb").write(data)
    print(f"{len(data)} bytes -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
