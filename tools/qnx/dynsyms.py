#!/usr/bin/env python3
"""List the defined dynamic symbols of an ELF32 ARM library, using only the
program headers (PT_DYNAMIC -> DT_HASH/DT_SYMTAB/DT_STRTAB), so it also
works for the 900's boot-image libraries, which have no section headers.

Usage: dynsyms.py lib.so [sym ...]   with syms: report which are missing
"""
import struct
import sys


def dynsyms(path):
    d = open(path, "rb").read()
    phoff, = struct.unpack_from("<I", d, 0x1c)
    phnum, = struct.unpack_from("<H", d, 0x2c)
    loads, dyn = [], None
    for i in range(phnum):
        t, off, va, _, fsz, _, _, _ = struct.unpack_from("<8I", d, phoff + 32 * i)
        if t == 1:
            loads.append((off, va, fsz))
        elif t == 2:
            dyn = off

    def fo(va):
        for off, v, sz in loads:
            if v <= va < v + sz:
                return off + va - v
        raise ValueError(hex(va))

    tags = {}
    i = dyn
    while True:
        tag, val = struct.unpack_from("<2I", d, i)
        if tag == 0:
            break
        tags.setdefault(tag, val)
        i += 8
    nsym, = struct.unpack_from("<I", d, fo(tags[4]) + 4)  # DT_HASH nchain
    symtab, strtab = fo(tags[6]), fo(tags[5])
    out = {}
    for n in range(1, nsym):
        name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, symtab + 16 * n)
        if shndx == 0:
            continue
        s = d[strtab + name:d.index(b"\0", strtab + name)].decode()
        out[s] = ("FUNC" if info & 0xf == 2 else "OBJECT" if info & 0xf == 1 else str(info & 0xf), value)
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip())
    syms = dynsyms(sys.argv[1])
    if len(sys.argv) == 2:
        for s in sorted(syms):
            print(s, *syms[s])
        return
    missing = [s for s in sys.argv[2:] if s not in syms]
    print(f"{len(sys.argv) - 2 - len(missing)} found, missing: {' '.join(missing) or '-'}")
    sys.exit(1 if missing else 0)


if __name__ == "__main__":
    main()
