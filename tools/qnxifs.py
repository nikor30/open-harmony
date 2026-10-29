#!/usr/bin/env python3
"""List or extract a QNX Neutrino boot image (startup + imagefs).

Usage: qnxifs.py image.bin [outdir]

Finds the startup header (signature EB 7E FF 00), decompresses the image
file system if needed (UCL/NRV2B blocks, each with a 2-byte big-endian
length, 0 ends; zlib also handled), and lists the imagefs directory. With
outdir, regular files, symlink targets and directories are written there;
paths are kept inside outdir. Nothing from the image is executed.

Written for the Harmony 900 (a 64-byte Logitech "ETHANOL" header comes
first), but only relies on the QNX formats.
"""
import os
import stat
import struct
import sys
import zlib

STARTUP_SIG = b"\xeb\x7e\xff\x00"


def nrv2b(src):
    """UCL NRV2B decompression (ucl_nrv2b_decompress_8)."""
    dst = bytearray()
    pos = 0
    bb = 0
    bits = 0

    def bit():
        nonlocal pos, bb, bits
        if bits == 0:
            bb = src[pos]
            pos += 1
            bits = 8
        bits -= 1
        return (bb >> bits) & 1

    last_off = 1
    while True:
        while bit():
            dst.append(src[pos])
            pos += 1
        off = 1
        while True:
            off = off * 2 + bit()
            if bit():
                break
        if off == 2:
            off = last_off
        else:
            off = ((off - 3) * 256 + src[pos]) & 0xFFFFFFFF
            pos += 1
            if off == 0xFFFFFFFF:
                break
            off += 1
            last_off = off
        length = bit()
        length = length * 2 + bit()
        if length == 0:
            length = 1
            while True:
                length = length * 2 + bit()
                if bit():
                    break
            length += 2
        length += off > 0xD00
        start = len(dst) - off
        for i in range(length + 1):
            dst.append(dst[start + i])
    return bytes(dst)


def imagefs(data):
    sig = data.find(STARTUP_SIG)
    if sig < 0:
        sys.exit("no QNX startup header")
    (flags1, startup_size, stored_size, imagefs_size) = (
        data[sig + 6], *struct.unpack_from("<I", data, sig + 0x20),
        *struct.unpack_from("<I", data, sig + 0x24),
        *struct.unpack_from("<I", data, sig + 0x2C))
    comp = flags1 & 0x1C
    pos = sig + startup_size
    print(f"startup header at 0x{sig:x}, startup {startup_size} bytes, "
          f"imagefs {imagefs_size} bytes, compression 0x{comp:x}")
    if comp == 0:
        return data[pos:pos + imagefs_size]
    out = bytearray()
    while True:
        n = struct.unpack_from(">H", data, pos)[0]
        pos += 2
        if n == 0:
            break
        block = data[pos:pos + n]
        pos += n
        out += zlib.decompress(block) if comp == 0x04 else nrv2b(block)
    if comp not in (0x04, 0x0C):
        sys.exit(f"unsupported compression 0x{comp:x}")
    return bytes(out)


def entries(fs):
    if not fs.startswith(b"imagefs"):
        sys.exit("no imagefs signature after decompression")
    dir_offset = struct.unpack_from("<I", fs, 16)[0]
    pos = dir_offset
    while True:
        size, _ext, ino, mode, gid, uid, mtime = struct.unpack_from("<HHIIIII", fs, pos)
        if size == 0:
            return
        body = pos + 24
        if stat.S_ISREG(mode):
            off, length = struct.unpack_from("<II", fs, body)
            path = fs[body + 8:pos + size].split(b"\0")[0]
            yield path, mode, fs[off:off + length], None
        elif stat.S_ISLNK(mode):
            sym_off, sym_size = struct.unpack_from("<HH", fs, body)
            path = fs[body + 4:pos + size].split(b"\0")[0]
            target = fs[body + 4 + sym_off:body + 4 + sym_off + sym_size]
            yield path, mode, None, target
        elif stat.S_ISDIR(mode):
            yield fs[body:pos + size].split(b"\0")[0], mode, None, None
        else:
            yield fs[body + 8:pos + size].split(b"\0")[0], mode, None, None
        pos += size


def safe_join(root, path):
    parts = [p for p in path.decode("latin-1").split("/") if p not in ("", ".", "..")]
    return os.path.join(root, *parts) if parts else root


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip())
    fs = imagefs(open(sys.argv[1], "rb").read())
    out = sys.argv[2] if len(sys.argv) == 3 else None
    for path, mode, content, target in entries(fs):
        kind = "d" if stat.S_ISDIR(mode) else "l" if stat.S_ISLNK(mode) else "-" if stat.S_ISREG(mode) else "?"
        size = len(content) if content is not None else 0
        extra = f" -> {target.decode('latin-1')}" if target else ""
        print(f"{kind} {mode & 0o7777:04o} {size:9d} /{path.decode('latin-1')}{extra}")
        if not out:
            continue
        dest = safe_join(out, path)
        if stat.S_ISDIR(mode):
            os.makedirs(dest, exist_ok=True)
        elif content is not None:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(content)
        elif target is not None:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest + ".symlink", "wb") as f:
                f.write(target)


if __name__ == "__main__":
    main()
