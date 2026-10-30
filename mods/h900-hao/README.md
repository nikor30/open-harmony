# h900-hao

First native control program for the Harmony 900. `ohhao` loads the
user's activities from `data_srv` (`GET /userconfig` on 127.0.0.1:80; each
`<Activity>` with its first `<Id>` and `<Label>`), shows one touch button
per activity (power off, Id -1, in red at the bottom) and on a tap sends
the same message as the Flash UI to `hao` on 127.0.0.1:1100:

```
<Event><Payload><Name>StartActivity</Name><Params><Id>ID</Id></Params></Payload></Event>\0
```

`hao` splits what it receives at zero bytes (`strlen`) and queues each
message for `handleEventXML()` in `HAO.lua`. Replies are shown (their
`<Name>`) and logged. Hard keys are shown by name from
[`h900keys.h`](../../tools/qnx/include/h900keys.h); EXIT ends the program
and the Flash UI starts (also after 5 minutes). Everything goes to
`/usr/local/cache/ohhao.log`, read back in safe mode with `zregion 12`.

Sockets follow the BSD layout that `sys_srv` uses (`getaddrinfo` with
`ai_socktype` 1, `socket(+4, +8, +12)`, `connect(fd, +24, +16)`).

```bash
tools/qnx/qnxcc.py mods/h900-hao/ohhao mods/h900-hao/ohhao.c \
    -l libc.so.2:usleep,sleep,alarm,creat,write,close,read,select,memset,memcpy \
    -l libsocket.so.2:getaddrinfo,freeaddrinfo,socket,connect \
    -l libgf.so.1:gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach,gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport,gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create,gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end,gfi_group_attach,gfi_layer_attach,gfi_layer_set_attrs,gfi_set_keyboard_focus,gfi_layer_get_event
mods/h900-hao/apply.py region12.bin mods/h900-hao/ohhao region12-hao.zip
local/bin/zwrite region12-hao.zip                    # safe mode only
```
