# h900-nativetest

The first native program for the Harmony 900, built without the QNX SDK.
`ohtest` connects to `env_mgr` like `SysService.lua` does and switches the
backlight off and on 5 times slowly (0.4 s off, 0.4 s on), then exits.
`flash.sh` starts it in the background right before the Flash player, so
at boot you see the 3 fast blinks of [`h900-wakeblink`](../h900-wakeblink)
followed by 5 slow ones, and the UI starts as usual even if `ohtest` fails.

```bash
tools/qnx/qnxcc.py mods/h900-nativetest/ohtest mods/h900-nativetest/ohtest.c \
    -l libc.so.2:usleep -l libcdf_client.so:env_service_connect,env_set_backlight_master
mods/h900-nativetest/apply.py region12-wakeblink.zip mods/h900-nativetest/ohtest region12-nativetest.zip
local/bin/zwrite region12-nativetest.zip             # safe mode only
```

See [`tools/qnx`](../../tools/qnx/README.md) for how the binary is built.
