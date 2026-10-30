# h900-ui

First native user interface for the Harmony 900, in place of the Flash UI.
`flash.sh` runs `ohui` right before the Flash player; Flash only starts
when `ohui` ends ("FLASH UI" on its home screen) or fails, so the stock
UI stays as fallback.

- **Home**: one button per activity from `GET /userconfig` (data_srv);
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
    -l libc.so.2:usleep,creat,write,close,read,select,memset,memcpy \
    -l libsocket.so.2:getaddrinfo,freeaddrinfo,socket,connect \
    -l libgf.so.1:gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach,gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport,gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create,gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end,gfi_group_attach,gfi_layer_attach,gfi_layer_set_attrs,gfi_set_keyboard_focus,gfi_layer_get_event
mods/h900-ui/apply.py region12.bin mods/h900-ui/ohui region12-ui.zip
local/bin/zwrite region12-ui.zip                     # safe mode only
```

hao's replies to `StartActivity` (from the `h900-hao` log):
`ActivityDeviceList` (devices with `UsedOnStartup`, `ManualPower`), then
a stream of `Queue` events (`Send` False/IR/RF/Done, `QueueStatus`,
per device `DeviceStatus` Doing/Done) and `ReceiverLinkDown` when the RF
IR blaster is not reachable.
