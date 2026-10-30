#!/usr/bin/env python3
"""Build a dynamically linked QNX Neutrino 6.3 ARM executable (Harmony 900).

Usage: qnxcc.py out file.c [file.c ...] [-l lib.so:sym,sym ...]

No QNX SDK needed: compiles with arm-none-eabi-gcc in the old ARM ABI
(-mabi=apcs-gnu, soft float, ARM926), starts with tools/qnx/crt0.S and
links with ld.lld against stub libraries that only carry the symbol names
(-l libcdf_client.so:env_service_connect,...). At run time ldqnx.so.2 on the
remote resolves them against the real libraries. libc.so.2 is always
linked; its stub covers _init_libc and exit plus the -l libc.so.2 symbols.
The result matches the remote's own programs: interpreter
/usr/lib/ldqnx.so.2, load address 0x100000, SysV hash, e_flags 0x2.
Avoid float/double for now (word order of soft doubles not verified).
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CFLAGS = ["-mabi=apcs-gnu", "-marm", "-mcpu=arm926ej-s", "-msoft-float",
          "-mno-unaligned-access", "-O2", "-Wall", "-ffreestanding",
          "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-nostdinc",
          "-I" + os.path.join(HERE, "include")]


def run(cmd):
    subprocess.run(cmd, check=True)


def stub(tmp, soname, syms):
    src = os.path.join(tmp, soname + ".S")
    with open(src, "w") as f:
        f.write("\t.text\n\t.arm\n")
        for s in syms:
            f.write(f"\t.global {s}\n\t.type {s}, %function\n{s}:\tbx lr\n")
    obj = src + ".o"
    run(["arm-none-eabi-gcc", *CFLAGS, "-c", src, "-o", obj])
    so = os.path.join(tmp, soname)
    run(["ld.lld", "-shared", "-soname", soname, "--hash-style=sysv", obj, "-o", so])
    return so


def fixup(path):
    """Make the header match the 900's own programs (GNU ld conventions)."""
    with open(path, "r+b") as f:
        d = f.read()
        phoff, = struct.unpack_from("<I", d, 0x1c)
        phnum, = struct.unpack_from("<H", d, 0x2c)
        loads, dyn = [], None
        for i in range(phnum):
            t, off, va, _, fsz, _, _, _ = struct.unpack_from("<8I", d, phoff + 32 * i)
            if t == 1:
                loads.append((off, va, fsz))
            elif t == 2:
                dyn = (off, va, fsz)
        pltgot = None
        for i in range(0, dyn[2], 8):
            tag, val = struct.unpack_from("<2I", d, dyn[0] + i)
            if tag == 3:
                pltgot = val
        for off, va, fsz in loads:  # GOT[0] = address of _DYNAMIC
            if pltgot is not None and va <= pltgot < va + fsz:
                f.seek(off + pltgot - va)
                f.write(struct.pack("<I", dyn[1]))
        f.seek(0x24)  # e_flags 0x2 as in the 900's programs
        f.write(struct.pack("<I", 0x2))


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__.strip())
    out, rest = args[0], args[1:]
    srcs, libs = [], {"libc.so.2": ["_init_libc", "exit"]}
    i = 0
    while i < len(rest):
        if rest[i] == "-l":
            name, syms = rest[i + 1].split(":")
            libs.setdefault(name, []).extend(s for s in syms.split(",") if s)
            i += 2
        else:
            srcs.append(rest[i])
            i += 1
    with tempfile.TemporaryDirectory() as tmp:
        objs = []
        for n, src in enumerate([os.path.join(HERE, "crt0.S"), *srcs]):
            obj = os.path.join(tmp, f"{n}.o")
            run(["arm-none-eabi-gcc", *CFLAGS, "-c", src, "-o", obj])
            objs.append(obj)
        sos = [stub(tmp, name, sorted(set(syms))) for name, syms in libs.items()]
        run(["ld.lld", "--dynamic-linker=/usr/lib/ldqnx.so.2", "--hash-style=sysv",
             "--image-base=0x100000", "-z", "norelro", "-z", "max-page-size=0x1000",
             "--no-rosegment", "-z", "nognustack", "-e", "_start", "--as-needed", *objs, *sos,
             "-L", tmp, "-o", out])
    fixup(out)
    os.chmod(out, 0o755)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
