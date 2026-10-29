# h900-slowfade

A first, harmless change for the Harmony 900: fade the backlight out over
about 1.5 s instead of about 0.18 s when the remote goes to sleep.

`share/lua/5.1/ethanol/SysService.lua` (region 12, platform) has
`fadedown()`, which steps the brightness from the user's level to 0 and
waits `local fadesleep = 3000` microseconds per step. `apply.py` changes
only that number (default 25000) and copies every other entry of the ZIP
unchanged, with permissions and timestamps:

```bash
local/bin/zregion 12 region12.zip                       # read-only dump
mods/h900-slowfade/apply.py region12.zip region12-slowfade.zip
unzip -tqq region12-slowfade.zip                        # the remote's own first check
```

The Lua thread sleeps during the fade, so input during those 1.5 s is
handled a little later; this only happens while going to sleep.

**Not written to a remote yet.** Region 12 has no second copy, and the
program serving port 3074 seems to come from region 12 in normal mode, so
recovery over USB in safe mode has to be confirmed first (see
[`docs/harmony-900.md`](../../docs/harmony-900.md)).
