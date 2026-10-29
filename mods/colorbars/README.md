# colorbars

First code mod: at boot, show the stock firmware's unused RGB test pattern
(red/green/blue bars, 176x220) for about 2 seconds, then continue normally.

How it works:

- `main` (0x28A0E) calls 0x2112C at 0x28A42 during start-up, after the LCD
  is initialised. `build.py` redirects that call to a trampoline appended
  after the end of the image (0x2EA92).
- The trampoline (`colorbars.asm`) runs the original call, keeps its
  result (`main` stores W in RAM 0xD84), calls the stock test pattern at
  0x229D0, waits with `CLRWDT` and returns the original result.
- `build.py` updates the length at +4 and the checksum at +0. Compared with
  the stock image, 5 bytes change, and 40 bytes are appended.

Build and flash (safe mode for the write):

```bash
mods/colorbars/build.py app-3.4.bin app-colorbars.bin
local/bin/flashapp write app-colorbars.bin
```

Needs gpasm (gputils). Only works on the stock 3.4 image; `build.py`
checks that first.
