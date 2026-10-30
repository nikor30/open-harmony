# h900-wakeblink

A clearly visible test mod for the Harmony 900: the backlight blinks three
times (0.15 s off, 0.15 s on) every time the screen turns on, at boot and on
every wake-up.

Every wake goes through `lightsUp()` in
`share/lua/5.1/ethanol/SysService.lua` (region 12). `apply.py` appends the
blink after its last `env_set_backlight_master_level()` call, only when
the LCD is switched on. It uses `socket.sleep` (LuaSocket, already loaded
in that file; `usleep` is only used in dead code) inside `pcall`, and sets
the normal level again afterwards, so an error cannot leave the screen dark.

```bash
mods/h900-wakeblink/apply.py region12.bin region12-wakeblink.zip
unzip -tqq region12-wakeblink.zip
luac5.1 -p <(unzip -p region12-wakeblink.zip share/lua/5.1/ethanol/SysService.lua)
local/bin/zwrite region12-wakeblink.zip              # safe mode only
```

Undo: write back the region 12 that `zwrite` saved before writing.
