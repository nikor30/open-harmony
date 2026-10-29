# Findings

Notes from poking at one Harmony One over USB with concordance. Only
read operations were used; nothing has been written to the remote.

## Identity

```
Model:             Logitech Harmony One
USB:               046d:c121, "Harmony Remote 0-3.4.0"
Firmware Version:  3.4
Hardware Version:  0.5.0
Architecture:      12
Protocol:          12
External Flash:    4 MiB, 1F:C8 Atmel AT49BV322A
Config Flash Used: 42% (1621 of 3840 KiB)
```

The kernel's usbhid deliberately ignores the Harmony range (046d:c110-c14f),
so there is no `/dev/hidraw` node; concordance talks to it via libusb.

## concordance support for arch 12

From `SupportedModels.md`: config update and learn IR work, firmware update
does **not**. In `remote_info.h` arch 12 has `flash_base = 0`,
`firmware_base = 0`, `config_base = 0x040000`, `firmware_update_base = 0`
and an empty `micro` field, so nobody has documented the CPU.

`--dump-firmware` nevertheless works: it reads the first 64 KiB of the
external flash at address 0. With
[`patches/libconcord-dump-size-env.patch`](../patches/libconcord-dump-size-env.patch)
(`LC_FW_READ_SIZE=0x40000`) the whole 256 KiB below the config area can be read.

## External flash, 0x000000-0x03FFFF

`tools/flashmap.py` on the 256 KiB dump:

| Range | Content |
|---|---|
| 0x00000-0x01FFF | almost empty, a few `67`/`F7`/`6F` bytes at the start |
| 0x02000-0x04FFF | data structures; `GSPM` at 0x2000 equals the arch 12 config cookie `0x4D505347` |
| 0x05000-0x1FFFF | empty (0xFF) |
| 0x20000-0x2BFFF | ~48 KiB **PIC18 program image** |
| 0x2C000-0x3FFFF | rest of image, then empty |

### The image at 0x20000

- Offset 8 holds `48 47` ("HG"), the same magic concordance's
  `_fix_magic_bytes()` writes into firmware images of older models.
- Filler `DE AD` runs at 0x20020.
- Of the 24576 16-bit words in 0x20000-0x2BFFF: 3337 `MOVLW` (`0Exx`),
  2813 `MOVWF` (`6Exx`/`6Fxx`), 963 `CALL` (`ECxx`/`EDxx`), 105 `GOTO` (`EFxx`),
  324 `RETURN` (`0012`/`0013`). ARM, MSP430 and AVR return opcodes do not occur.

So the One most likely runs on a Microchip PIC18, and the external flash
carries a copy of its program. Whether that copy is what the PIC executes
from (unlikely for PIC18), a staging area for updates, or a backup is still
open.

## Next steps

1. Disassemble the image (`gpdasm -p 18f...`) and check it is coherent code;
   find the USB command handler and the flash-write path.
2. Open the remote and read the part number of the main chip; look for the
   ICSP header (MCLR, PGC, PGD, VDD, GND) used for PIC programming.
3. With the exact part known, an ICSP programmer (PICkit) gives a
   brick-proof way to read and write the internal flash.
