#!/usr/bin/env python3
"""Post-process gpdasm PIC18 output.

Usage: pic18fix.py in.asm > out.asm

- Decodes relative branches that gpdasm leaves as `dw` (BC, BN, BNC, BNN,
  BNOV, BNZ, BOV, BZ, BRA, RCALL) and prints their absolute targets.
- Restores the full 21-bit target of CALL/GOTO, which gpdasm masks to the
  device's internal flash size.
- Marks every branch/call target with a `L_xxxxxx:` label line.
"""
import re
import sys

LINE = re.compile(r"^([0-9a-f]{6}):\s+([0-9a-f]{4})\s+(\w+)\s*(.*)$")
COND = {0xE0: "bz", 0xE1: "bnz", 0xE2: "bc", 0xE3: "bnc",
        0xE4: "bov", 0xE5: "bnov", 0xE6: "bn", 0xE7: "bnn"}


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    rows = []
    for raw in open(sys.argv[1]):
        m = LINE.match(raw.rstrip("\n"))
        if m:
            rows.append([int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)])
        else:
            rows.append(raw.rstrip("\n"))
    # second word of CALL/GOTO lines has no mnemonic; gpdasm prints it as a bare row
    words = {}
    for raw in open(sys.argv[1]):
        m = re.match(r"^([0-9a-f]{6}):\s+([0-9a-f]{4})", raw)
        if m:
            words[int(m.group(1), 16)] = int(m.group(2), 16)

    targets = set()
    for row in rows:
        if isinstance(row, str):
            continue
        addr, word, op, args = row
        hi = word >> 8
        if op == "dw" and hi in COND:
            t = addr + 2 + 2 * signed(word & 0xFF, 8)
            row[2], row[3] = COND[hi], f"0x{t:06x}"
            targets.add(t)
        elif op == "dw" and word >> 11 in (0x1A, 0x1B):  # BRA / RCALL, 11-bit offset
            t = addr + 2 + 2 * signed(word & 0x7FF, 11)
            row[2], row[3] = "bra" if word >> 11 == 0x1A else "rcall", f"0x{t:06x}"
            targets.add(t)
        elif op in ("call", "goto") and addr + 2 in words:
            k = ((words[addr + 2] & 0xFFF) << 8) | (word & 0xFF)
            t = 2 * k
            rest = args.split(",", 1)
            row[3] = f"0x{t:06x}" + ("," + rest[1] if len(rest) > 1 else "")
            targets.add(t)

    for row in rows:
        if isinstance(row, str):
            print(row)
            continue
        addr, word, op, args = row
        if addr in targets:
            print(f"L_{addr:06x}:")
        print(f"{addr:06x}:  {word:04x}  {op:7s} {args}".rstrip())


if __name__ == "__main__":
    main()
