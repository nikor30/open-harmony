#!/usr/bin/env python3
"""Harmony 900: add the native user interface (ohui) to region 12.

Usage: apply.py region12.zip ohui out.zip

Adds `bin/ohui` (built with tools/qnx/qnxcc.py, see README.md) with the
same mode and owner bits as the other programs in bin/, and runs it from
bin/flash.sh just before the Flash player. ohui runs until "Classic UI" is
tapped on its home screen (or it fails); then the Flash UI starts as
before.

Also teaches hao one more event, `SendCode` with a `Code` parameter: the
protocol code goes to the IR queue as it is, like the codes of the user
configuration do in commandAction(). ohui uses it for codes that are in no
Logitech configuration. Every other entry is copied unchanged, with its
permissions and timestamps.
"""
import sys
import zipfile

SCRIPT = "bin/flash.sh"
OLD = b"slay -f splash\n\n$FLASH_PATH/bin/flash-gf "
NEW = b"slay -f splash\n\n/usr/local/bin/ohui\n\n$FLASH_PATH/bin/flash-gf "

HAO = "share/lua/5.1/ethanol/HAO.lua"
HAO_OLD = b'\tif parsed.Payload[1].Name[1].value == "SendChannel" then\r\n'
# the device id only labels hao's queue events; any configured device will do
HAO_NEW = b"""\t-- open-harmony: send a protocol code that is not in the user configuration
\tif parsed.Payload[1].Name[1].value == "SendCode" then
\t\tif parsed.Payload[1].Params == nil then return -1 end
\t\tif parsed.Payload[1].Params[1] == nil then return -1 end
\t\tif parsed.Payload[1].Params[1].Code == nil then return -1 end
\t\tlocal id = "0"
\t\tfor k in pairs(devices) do id = k break end
\t\tsendCommand(id, hash, parsed.Payload[1].Params[1].Code[1].value, 1, 0, "", 0, 0, 0)
\tend
\t
""".replace(b"\n", b"\r\n") + HAO_OLD


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    prog = open(sys.argv[2], "rb").read()
    if prog[:4] != b"\x7fELF" or prog[0x12] != 40:
        sys.exit("ohui is not an ARM ELF")
    with zipfile.ZipFile(sys.argv[1]) as src, \
            zipfile.ZipFile(sys.argv[3], "w", zipfile.ZIP_DEFLATED) as dst:
        if src.testzip() is not None:
            sys.exit("input ZIP has a bad CRC")
        if "bin/ohui" in src.namelist():
            sys.exit("bin/ohui already present")
        changed = 0
        for info in src.infolist():
            data = src.read(info)
            if info.filename == SCRIPT:
                if data.count(OLD) != 1:
                    sys.exit("flash.sh does not look as expected; not changing it")
                data = data.replace(OLD, NEW)
                changed += 1
            if info.filename == HAO:
                if data.count(HAO_OLD) != 1:
                    sys.exit("HAO.lua does not look as expected; not changing it")
                data = data.replace(HAO_OLD, HAO_NEW)
                changed += 1
            dst.writestr(info, data)
            if info.filename == "bin/hao":
                new = zipfile.ZipInfo("bin/ohui", date_time=info.date_time)
                new.create_system = info.create_system
                new.external_attr = info.external_attr
                new.compress_type = zipfile.ZIP_DEFLATED
                dst.writestr(new, prog)
                changed += 1
        if changed != 3:
            sys.exit("flash.sh, HAO.lua or bin/hao not found")
    print(f"added bin/ohui, started from {SCRIPT}; SendCode in {HAO}; wrote {sys.argv[3]}")


if __name__ == "__main__":
    main()
