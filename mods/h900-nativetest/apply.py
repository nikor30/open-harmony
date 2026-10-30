#!/usr/bin/env python3
"""Harmony 900: add the first native program (ohtest) to region 12.

Usage: apply.py region12.zip ohtest out.zip

Adds `bin/ohtest` (built with tools/qnx/qnxcc.py, see README.md) with the
same mode and owner bits as the other programs in bin/, and starts it from
bin/flash.sh in the background just before the Flash player, so the UI
starts even if ohtest fails or hangs. Every other entry is copied
unchanged, with its permissions and timestamps.
"""
import sys
import zipfile

SCRIPT = "bin/flash.sh"
OLD = b"slay -f splash\n\n$FLASH_PATH/bin/flash-gf "
NEW = b"slay -f splash\n\n/usr/local/bin/ohtest &\n\n$FLASH_PATH/bin/flash-gf "


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    prog = open(sys.argv[2], "rb").read()
    if prog[:4] != b"\x7fELF" or prog[0x12] != 40:
        sys.exit("ohtest is not an ARM ELF")
    with zipfile.ZipFile(sys.argv[1]) as src, \
            zipfile.ZipFile(sys.argv[3], "w", zipfile.ZIP_DEFLATED) as dst:
        if src.testzip() is not None:
            sys.exit("input ZIP has a bad CRC")
        if "bin/ohtest" in src.namelist():
            sys.exit("bin/ohtest already present")
        changed = 0
        for info in src.infolist():
            data = src.read(info)
            if info.filename == SCRIPT:
                if data.count(OLD) != 1:
                    sys.exit("flash.sh does not look as expected; not changing it")
                data = data.replace(OLD, NEW)
                changed += 1
            dst.writestr(info, data)
            if info.filename == "bin/hao":
                new = zipfile.ZipInfo("bin/ohtest", date_time=info.date_time)
                new.create_system = info.create_system
                new.external_attr = info.external_attr
                new.compress_type = zipfile.ZIP_DEFLATED
                dst.writestr(new, prog)
                changed += 1
        if changed != 2:
            sys.exit("flash.sh or bin/hao not found")
    print(f"added bin/ohtest, started from {SCRIPT}; wrote {sys.argv[3]}")


if __name__ == "__main__":
    main()
