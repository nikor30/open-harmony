# h900-input

Input test for the Harmony 900: `ohinput` runs for 30 s before the Flash
UI starts. Touching the upper part of the screen draws white dots; the last
touch event (cyan, 8 raw 32-bit words) and the last key event (yellow) are
shown in hex below it, so the event layout can be read off the screen. The
small square on the right toggles green/red with every key event; the
green bar at the bottom shows the remaining time.

Input goes through GF's input API in `libgf.so.1`, set up as `flash-gf`
does it (disassembled): `gfi_group_attach(&grp, 1, 0)`,
`gfi_layer_attach(&il, grp, 0, main_layer)`,
`gfi_layer_set_attrs(il, 0, 0, w-1, h-1, 7)`,
`gfi_set_keyboard_focus(grp, main_layer)`, then
`gfi_layer_get_event(il, 1, ev, 1)` (non-blocking, 35 = nothing pending).
Known so far from `flash-gf`: event type at +0 (1 touch, 2 key); touch x
+8, y +12, buttons +16 (4 = pressed); key flags +24.

```bash
tools/qnx/qnxcc.py mods/h900-input/ohinput mods/h900-input/ohinput.c -l libc.so.2:usleep,sleep,alarm,signal,_exit \
    -l libgf.so.1:gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach,gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport,gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create,gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end,gfi_group_attach,gfi_layer_attach,gfi_layer_set_attrs,gfi_set_keyboard_focus,gfi_layer_get_event
mods/h900-input/apply.py region12.bin mods/h900-input/ohinput region12-input.zip
local/bin/zwrite region12-input.zip                  # safe mode only
```
