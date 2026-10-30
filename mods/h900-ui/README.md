# h900-ui

Native user interface for the Harmony 900, in place of the Flash UI.

Version 2 draws with [`ohdraw.h`](../../tools/qnx/include/ohdraw.h)
(double-buffered, anti-aliased Noto Sans, rounded cards, spring-driven
slide between the activity list and an activity, touch feedback on
touch-down with commit on release). The drawing code also builds on the PC
(`OHUI_HOST`) to render screenshots for design checks. Version 1 (plain
rectangles drawn straight onto the display) worked on the remote but
flickered on every tap because it cleared the screen in place.
`flash.sh` runs `ohui` right before the Flash player; Flash only starts
when `ohui` ends ("Classic UI" on its home screen) or fails, so the stock
UI stays as fallback.

- **Home**: one card per activity from `GET /userconfig` (data_srv);
  power off (Id -1) in red. A tap sends `StartActivity` to `hao`
  (127.0.0.1:1100) and opens the activity.
- **Activity**: the activity's `ControlGroup name="Misc"` buttons as a
  touch grid (label, or icon name if empty, sorted by `Position`). Hard
  keys look up `ControlGroup name="HardButtons"` (`Button name` ->
  `ActionId`, Logitech names such as `VolumeUp`, `DirectionLeft`,
  `Number5`, `Select`) and send `SendIR <ActionId>`, as the Flash UI does.
  Volume, channel, direction and rocker keys repeat while held.
  ACTIVITIES or UNDER LEFT: home; OFF: power off.
- **Status**: from hao's replies (`Queue` with `Send` IR/RF/Done,
  `ReceiverLinkDown`).
- Log: `/usr/local/cache/ohui.log` (read with `zregion 12` in safe mode).

```bash
tools/qnx/qnxcc.py mods/h900-ui/ohui mods/h900-ui/ohui.c \
    -l libc.so.2:usleep,creat,write,close,read,select,memset,memcpy,time,localtime,clock_gettime,__divsi3,__modsi3 \
    -l libsocket.so.2:getaddrinfo,freeaddrinfo,socket,connect -l libcdf_client.so:env_service_connect,env_get_battery_level \
    -l libgf.so.1:gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach,gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport,gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create,gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end,gf_surface_create,gf_surface_get_info,gf_draw_blit2,gf_draw_finish,gfi_group_attach,gfi_layer_attach,gfi_layer_set_attrs,gfi_set_keyboard_focus,gfi_layer_get_event
mods/h900-ui/apply.py region12.bin mods/h900-ui/ohui region12-ui5.zip
local/bin/zwrite region12-ui5.zip                     # safe mode only
```

hao's replies to `StartActivity` (from the `h900-hao` log):
`ActivityDeviceList` (devices with `UsedOnStartup`, `ManualPower`), then
a stream of `Queue` events (`Send` False/IR/RF/Done, `QueueStatus`,
per device `DeviceStatus` Doing/Done) and `ReceiverLinkDown` when the RF
IR blaster is not reachable.

Battery: `env_get_battery_level(&result, &level)` (libcdf_client) writes a
16-bit level 0..8; `cache/batt_lvls.dat` holds the millivolt thresholds
for levels 1..8 (3216 ... 4043 mV). data_srv does not answer
`GET /system/batterylevel`, so it is not asked (each attempt blocked the
UI for the read timeout).
