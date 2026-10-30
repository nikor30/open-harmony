# h900-colorbars

First native drawing on the Harmony 900: `ohbars` fills the screen with
eight colour bars (white, yellow, cyan, green, magenta, red, blue, black)
through the QNX Graphics Framework, holds them 4 s and exits. `flash.sh`
runs it right before the Flash player, so the UI starts afterwards as
usual; `alarm(8)` ends it after 8 s at the latest.

The GF calls follow `flash-gf`'s own start-up code (disassembled):
`gf_dev_attach(&dev, NULL, NULL)`, `gf_display_attach(.., 0, info)`,
`gf_layer_attach` on the main layer (display info +4; width/height
16-bit at +8/+10, pixel format at +12), `gf_surface_create_layer`,
`gf_context_create`/`set_surface`, `gf_layer_set_surfaces`, source and
destination viewport, `gf_layer_enable`, `gf_layer_update`; then
`gf_draw_begin`, `gf_context_set_fgcolor(0xRRGGBB)`, `gf_draw_rect(x1, y1,
x2, y2)`, `gf_draw_end`.

Built on the original region 12 (no wakeblink, no blink test):

```bash
tools/qnx/qnxcc.py mods/h900-colorbars/ohbars mods/h900-colorbars/ohbars.c \
    -l libc.so.2:sleep,alarm -l libgf.so.1:gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach,gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport,gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create,gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end
mods/h900-colorbars/apply.py region12.bin mods/h900-colorbars/ohbars region12-colorbars.zip
local/bin/zwrite region12-colorbars.zip              # safe mode only
```
