#!/usr/bin/env python3
"""Map a raw flash dump: entropy and PIC18 opcode score per block.

Usage: flashmap.py dump.bin [block_size]

Empty flash reads 0xFF. Code shows medium entropy (~5-6.5 bits/byte);
the PIC18 score is the share of 16-bit little-endian words whose high
byte is a common PIC18 opcode (MOVLW, MOVWF, CALL, GOTO).
"""
import collections
import math
import sys

PIC18_HIGH = {0x0E, 0x6E, 0x6F, 0xEC, 0xED, 0xEF}


def entropy(block):
    counts = collections.Counter(block)
    return -sum(n / len(block) * math.log2(n / len(block)) for n in counts.values())


def pic18_score(block):
    words = len(block) // 2
    hits = sum(1 for i in range(1, words * 2, 2) if block[i] in PIC18_HIGH)
    return hits / words if words else 0


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip())
    data = open(sys.argv[1], "rb").read()
    size = int(sys.argv[2], 0) if len(sys.argv) == 3 else 0x4000
    print("offset   entropy  0xFF%  pic18%")
    for off in range(0, len(data), size):
        block = data[off:off + size]
        ff = block.count(0xFF) * 100 // len(block)
        print(f"{off:06x}   {entropy(block):5.2f}   {ff:4d}   {pic18_score(block) * 100:5.1f}")


if __name__ == "__main__":
    main()
