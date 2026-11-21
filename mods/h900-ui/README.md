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

Version 9 turns it into a stateless remote for a home cinema that Home
Assistant runs. The remote holds no device logic at all:

- **Home**: the foyer of the Home Assistant "Kino" panel: in front of the
  curtain one large ticket per activity, one to a page, and "Vorstellung
  beenden" below. Swipe left or right (or press a side arrow key) to turn
  the page; a tap on the ticket sends its infrared code. The ticket tapped
  last gets a white frame and a gold page dot (infrared is one-way, the
  remote cannot see what really runs). Settings and Classic UI are on the
  last page.
- **Hard keys**: every key sends its own code, the same in every activity.
  ACTIVITIES: home; OFF: same as "Vorstellung beenden"; volume, channel,
  direction and rocker keys repeat while held.
- **Codes**: NEC, four bytes `4F 48 <code> <~code>` (the gateway reports
  `NEC 0x4F48ccnn`). `0x10` off, `0x11`..`0x14` the four tickets,
  `0x80 | scancode` for hard keys (scancodes in
  [`h900keys.h`](../../tools/qnx/include/h900keys.h), e.g. VOL + = `0x8A`).
- **Home Assistant**: an IR gateway publishes what it hears; one automation
  selects the activity for ticket codes and maps key codes by the activity
  that runs (volume to the receiver, the rest to the activity's player,
  over an API or back out of the gateway as that device's own infrared code).
- **SendCode**: the codes are in no Logitech configuration, so `apply.py`
  adds an event to `hao` (`HAO.lua`): `SendCode` with a `Code` parameter
  puts a protocol code into the IR queue as it is. The code is the frame a
  NEC command has in this remote's `UserConfiguration.xml`
  (`04 00 F4 01 01 01 00 <4 bytes> 01 01 00`, protocol 4 of its
  `IrProto.bin`); a remote without a NEC device in its configuration needs
  another frame.
- The device and activity screens of version 8 (`/userconfig`, `SendIR`,
  `SendChannel`) are still in the source but not reachable.
- Log: `/usr/local/cache/ohui.log` (read with `zregion 12`).

```bash
tools/qnx/qnxcc.py mods/h900-ui/ohui mods/h900-ui/ohui.c \
    -l libc.so.2:usleep,creat,open,write,close,read,select,memset,memcpy,time,localtime,clock_gettime,__divsi3,__modsi3 \
    -l libsocket.so.2:getaddrinfo,freeaddrinfo,socket,connect \
    -l libcdf_client.so:env_service_connect,env_get_battery_level,env_get_charger_state,env_set_backlight_master_level,audio_service_connect,audio_buzzer_generate_sound,env_get_cradle_state,env_get_light_level,env_get_light_reading,env_get_tune_light_read_freq \
    -l libgf.so.1:gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach,gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport,gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create,gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end,gf_surface_create,gf_surface_get_info,gf_draw_blit2,gf_draw_finish,gfi_group_attach,gfi_layer_attach,gfi_layer_set_attrs,gfi_set_keyboard_focus,gfi_layer_get_event
mods/h900-ui/apply.py region12.bin mods/h900-ui/ohui region12-ui10.zip
local/bin/zwrite region12-ui10.zip                     # safe mode only
```

Screens can be rendered on the PC first ([`host.c`](host.c), 240x320 RGB565):

```bash
gcc -O1 -w -ffreestanding -fno-builtin -Itools/qnx/include -o /tmp/ohui-host mods/h900-ui/host.c
/tmp/ohui-host out.raw 0 -1 208 0     # running ticket, pressed ticket, px (208 = second page), screen
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

Hardware used by version 6 (all through the remote's own services):

- **Soft-button LEDs**: each of the four soft buttons has a white and an
  orange LED, set like the Flash UI does with `POST /system/leftarrow`,
  `/system/rightarrow`, `/system/leftbottombutton`, `/system/rightbottombutton`
  (0 off, 1 white, 2 orange; SysService.lua keeps them across sleep). ohui
  lights under-left white when back is possible, under-right orange when an
  activity runs (opens it), and the side arrows white when the list can be
  paged up or down.
- **Settings** go to data_srv as `POST /system/<name>` with the value as a
  plain-text body (as sys_srv's `httppost`); SysService applies
  `backlightlevel` and `backlighttimeout` at once. Current values come from
  data_srv's cache files `/usr/local/cache/system_<name>.dat`.
- **Buzzer**: `audio_buzzer_generate_sound(unused, Hz, ms, &result)`
  (libcdf_client, audio_mgr calls `buzzer_tone_generate(Hz, ms)`); only
  used when `/system/sound` is on. The 900 starts no `io-audio` driver; the
  WM8971 codec path is for the other variants.
- **Charger**: `env_get_charger_state(&result, &state)`, 16-bit like the
  battery level.
