#!/usr/bin/env python3
"""Harmony 900: blink the backlight 3 times whenever the screen turns on.

Usage: apply.py region12.zip out.zip

region12.zip is the remote's platform region as read with
`tools/zregion.cpp` (region 12). In share/lua/5.1/ethanol/SysService.lua,
lightsUp() ends by setting the backlight master level. This appends, only
when the LCD is switched on, three off/on blinks of 0.15 s each using
socket.sleep (LuaSocket, already loaded there), wrapped in pcall so an
error cannot keep the screen dark. Every other entry is copied unchanged,
with its permissions and timestamps.
"""
import sys
import zipfile

TARGET = "share/lua/5.1/ethanol/SysService.lua"
OLD = (b"\tret, result = env_set_backlight_master_level( backlightlevel , result );\r\n"
       b"\r\n"
       b"end\r\n"
       b"\r\n"
       b"function lightsDown(")
BLINK = (b"\t-- open-harmony wakeblink\r\n"
         b"\tif lcd ~= nil and lcd ~= 0 then\r\n"
         b"\t\tpcall(function()\r\n"
         b"\t\t\tfor i = 1,3 do\r\n"
         b"\t\t\t\tsocket.sleep(0.15)\r\n"
         b"\t\t\t\tenv_set_backlight_master_level( 0 , 0 )\r\n"
         b"\t\t\t\tsocket.sleep(0.15)\r\n"
         b"\t\t\t\tenv_set_backlight_master_level( backlightlevel , 0 )\r\n"
         b"\t\t\tend\r\n"
         b"\t\tend)\r\n"
         b"\t\tenv_set_backlight_master_level( backlightlevel , 0 )\r\n"
         b"\tend\r\n")
NEW = OLD.replace(b"\r\n\r\nend\r\n", b"\r\n" + BLINK + b"\r\nend\r\n", 1)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    with zipfile.ZipFile(sys.argv[1]) as src, \
            zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as dst:
        if src.testzip() is not None:
            sys.exit("input ZIP has a bad CRC")
        changed = 0
        for info in src.infolist():
            data = src.read(info)
            if info.filename == TARGET:
                if data.count(OLD) != 1:
                    sys.exit("lightsUp() does not look as expected; not changing it")
                data = data.replace(OLD, NEW)
                changed += 1
            dst.writestr(info, data)
        if changed != 1:
            sys.exit(f"{TARGET} not found")
    print(f"{TARGET}: lightsUp() blinks 3x on LCD on; wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
