#!/usr/bin/env python3
"""Harmony 900: slow down the backlight fade-out before sleep.

Usage: apply.py region12.zip out.zip [microseconds]

region12.zip is the remote's platform region as read with
`tools/zregion.cpp` (region 12). In share/lua/5.1/ethanol/SysService.lua,
fadedown() waits `local fadesleep = 3000` microseconds per brightness step
(about 0.18 s from level 60 to 0). This sets it to 25000 (about 1.5 s) or
the given value. Every other entry is copied unchanged, with its
permissions and timestamps.
"""
import sys
import zipfile

TARGET = "share/lua/5.1/ethanol/SysService.lua"
OLD = b"function fadedown()\r\n\tlocal fadesleep = 3000;"


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__.strip())
    usec = int(sys.argv[3]) if len(sys.argv) == 4 else 25000
    if not 1000 <= usec <= 100000:
        sys.exit("microseconds must be 1000..100000")
    new = OLD.replace(b"3000", str(usec).encode())

    with zipfile.ZipFile(sys.argv[1]) as src, \
            zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as dst:
        if src.testzip() is not None:
            sys.exit("input ZIP has a bad CRC")
        changed = 0
        for info in src.infolist():
            data = src.read(info)
            if info.filename == TARGET:
                if data.count(OLD) != 1:
                    sys.exit("fadedown() does not look as expected; not changing it")
                data = data.replace(OLD, new)
                changed += 1
            dst.writestr(info, data)
        if changed != 1:
            sys.exit(f"{TARGET} not found")
    print(f"{TARGET}: fadedown() fadesleep 3000 -> {usec} us; wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
