# h900-keymap

Guided key mapping for the Harmony 900 and the first native program with
text. `ohkeys` shows `PRESS:` and the name of each key in turn, records
scancode, cap and sym of the press (repeats ignored until release), a tap
on the screen skips a key. After every key it writes the table to
`/usr/local/ohkeys.txt`, which is not writable, so it falls back to
`/usr/local/cache/ohkeys.txt` (region 12 `cache/`, read back in safe mode
with `local/bin/zregion 12`). It ends 5 s after the last key or after
4 minutes; then the Flash UI starts.

The resulting table, cross-checked with the photographed history of
[`h900-input`](../h900-input) v4, is
[`tools/qnx/include/h900keys.h`](../../tools/qnx/include/h900keys.h).

```bash
tools/qnx/qnxcc.py mods/h900-keymap/ohkeys mods/h900-keymap/ohkeys.c \
    -l libc.so.2:usleep,sleep,alarm,creat,write,close \
    -l libgf.so.1:gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach,gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport,gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create,gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end,gfi_group_attach,gfi_layer_attach,gfi_layer_set_attrs,gfi_set_keyboard_focus,gfi_layer_get_event
mods/h900-keymap/apply.py region12.bin mods/h900-keymap/ohkeys region12-keymap.zip
local/bin/zwrite region12-keymap.zip                 # safe mode only
```
