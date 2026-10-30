#!/usr/bin/env python3
"""Harmony 900: add the hao control test (ohhao) to region 12.

Usage: apply.py region12.zip ohhao out.zip

Adds `bin/ohhao` (built with tools/qnx/qnxcc.py, see README.md) with the
same mode and owner bits as the other programs in bin/, and runs it from
bin/flash.sh just before the Flash player. ohhao ends with the EXIT key
or after 5 minutes (alarm() 10 s later at the latest), then the UI starts
as usual. Every other entry is copied
unchanged, with its permissions and timestamps.
"""
import sys
import zipfile

SCRIPT = "bin/flash.sh"
OLD = b"slay -f splash\n\n$FLASH_PATH/bin/flash-gf "
NEW = b"slay -f splash\n\n/usr/local/bin/ohhao\n\n$FLASH_PATH/bin/flash-gf "


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    prog = open(sys.argv[2], "rb").read()
    if prog[:4] != b"\x7fELF" or prog[0x12] != 40:
        sys.exit("ohhao is not an ARM ELF")
    with zipfile.ZipFile(sys.argv[1]) as src, \
            zipfile.ZipFile(sys.argv[3], "w", zipfile.ZIP_DEFLATED) as dst:
        if src.testzip() is not None:
            sys.exit("input ZIP has a bad CRC")
        if "bin/ohhao" in src.namelist():
            sys.exit("bin/ohhao already present")
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
                new = zipfile.ZipInfo("bin/ohhao", date_time=info.date_time)
                new.create_system = info.create_system
                new.external_attr = info.external_attr
                new.compress_type = zipfile.ZIP_DEFLATED
                dst.writestr(new, prog)
                changed += 1
        if changed != 2:
            sys.exit("flash.sh or bin/hao not found")
    print(f"added bin/ohhao, started from {SCRIPT}; wrote {sys.argv[3]}")


if __name__ == "__main__":
    main()
