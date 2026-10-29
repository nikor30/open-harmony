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
| 0x20000-0x2EA91 | 60050 bytes **PIC18 application image** (firmware 3.4) |
| 0x2EA92-0x3FFFF | empty |

### The image at 0x20000

Opcode statistics over 0x20000-0x2BFFF (24576 words): 3337 `MOVLW` (`0Exx`),
2813 `MOVWF` (`6Exx`/`6Fxx`), 963 `CALL` (`ECxx`/`EDxx`), 105 `GOTO` (`EFxx`),
324 `RETURN` (`0012`/`0013`). ARM, MSP430 and AVR return opcodes do not occur.
`gpdasm` turns it into coherent code.

**It executes in place.** All 1315 `CALL`/`GOTO` targets lie in
0x20014-0x2EA8C, i.e. the code is linked for exactly the addresses it has in
the external flash. A PIC18 can only do that through its external memory bus
in extended microcontroller mode, where everything above the internal flash
is fetched from the bus.

The 7 remaining targets go to 0x1E00C-0x1E014, a jump table near the top of
the internal 128 KiB flash: most likely the API of a bootloader that lives
in the PIC itself.

Header (little endian):

| Offset | Value | Meaning |
|---|---|---|
| +0 | `CD D8` | checksum: XOR of byte pairs +4..end, seeded `21 43` (same algorithm as concordance's `_fix_magic_bytes()`) |
| +2 | `FF FF` | unused |
| +4 | `0xEA8A` | length after +8; 0x20008 + 0xEA8A = 0x2EA92 = end of image |
| +6 | `00 34` | version 3.4 (matches `concordance -i`) |
| +8 | `48 47` | "HG" magic |
| +A | `GOTO 0x2EA38` | entry point |
| +0x20 | `DE AD` x 8 | filler |

## CPU: PIC18F87J50

Special function registers (SFRs) used by the code, resolved with the
gputils headers:

- Heavy use of `TBLPTR`/`TABLAT` (table reads from flash), `FSR0`, `PORTC`.
- USB registers, all at their PIC18F87J50 addresses: `UCON` (0xF65),
  `UIR` (0xF62), `UIE` (0xF5C), `UEIE`, `UADDR` (0xF5E), `UCFG` (0xF5F),
  `UEP0`-`UEP2` (0xF4C-0xF4E). On the J94 family these addresses are clock
  and RTC registers; its USB block (`UCON` at 0xF75) is never touched.
- Banked accesses to 0xF00-0xF3F, which is plain RAM on the J50.

The J50 family with an external memory bus and 128 KiB internal flash
(needed for the 0x20000 boundary and the 0x1E000 jump table) is the
80-pin **PIC18F87J50**. To be confirmed by reading the chip marking.

## Bootloader API (internal flash, 0x1E00C-0x1E014)

Five one-word entries (each a branch into the bootloader). Parameters go in
RAM 0x60-0x62 (24-bit address, low byte first) and 0x63-0x65 (data). The
application wraps every call in 0x2E8F8 / 0x2E908, which save `INTCON.GIE`,
disable interrupts and restore them afterwards.

| Entry | Called from | Arguments | Likely function |
|---|---|---|---|
| 0x1E00C | 0x2DDCE, 0x2DDEC | address 0x022000 / 0x022002, result in 0x63 | read flash ID (results stored to 0xD54/0xD55; `1F`/`C8` = Atmel AT49BV322A) |
| 0x1E00E | 0x2DEDA | address | erase sector |
| 0x1E010 | 0x2DE92 | address, 1 data byte | program |
| 0x1E012 | 0x2DEBC | address, 3 data bytes | program (wider) |
| 0x1E014 | 0x2C8C2, 0x2CA24 | none, called in a loop with `CLRWDT` | probably sleep/idle |

These routines have to run from internal flash: while the external chip is
in ID, erase or program mode the CPU cannot fetch code from it.

## USB command handling

The command byte is in RAM 0xD1F. The dispatcher at 0x26492 switches on its
high nibble: `B0` READ_MISC, `10` GET_VERSION, `70` START_IRCAP, `A0`
WRITE_MISC, `D0` ERASE_FLASH (`D3`), `50` READ_FLASH, `30` WRITE_FLASH,
matching concordance's `protocol.h`.

`ERASE_FLASH` (0x2657A):

1. Reads a 3-byte address, high byte first, into 0x287/0x286/0x285.
2. Classifies it with 0x2637A:
   - high byte 0x00-0x3F: external flash, type 0x00 (all 4 MiB);
   - 0xFE0000-0xFEFFF8: memory type 0xFE, handled by 0x2E6B4;
   - anything else: rejected.
3. For external flash: writes `high - 3` with `TBLWT` to bus address
   **0x020025** (inside the `DE AD` filler of the image header, so most
   likely a memory-mapped bank latch), then erases through the bootloader
   at window address `0x13:mid:low`.

The 4 MiB chip is therefore not mapped linearly into the 2 MiB PIC18 address
space; part of it is banked through the latch at 0x020025.

**There is no guard for the application region.** An `ERASE_FLASH` for
0x020000-0x02FFFF is accepted and would erase the code the CPU is running
from. Never send erase or write commands in that range with concordance or
your own tools until the bootloader's recovery behaviour is known.

The relative branches in this section were decoded with
[`tools/pic18fix.py`](../tools/pic18fix.py).

## What this means for custom firmware

- The application is not hidden in the PIC; it sits in the external flash,
  which concordance can already read over USB, and the header format is known.
- A replacement application could be linked for 0x20000, given the same
  header and checksum, and written to the external flash, leaving the
  bootloader in the internal flash untouched.
- Unknown: how the bootloader decides whether to start the application, and
  whether it still offers USB recovery if the image is bad. Until that is
  known, writing 0x20000 can brick the remote.

## Next steps

1. Work out the bank latch at 0x020025: which latch value maps which part
   of the 4 MiB into the window, and whether the window at 0x13xxxx can
   reach the application region at all.
2. Open the remote, confirm the PIC18F87J50 marking, and find the ICSP pads
   (MCLR, PGC, PGD, VDD, GND). A PICkit on ICSP gives a brick-proof way to
   back up and restore the internal flash, including the bootloader.
3. Only then: build a minimal test image (e.g. toggle a backlight pin) and
   flash it to 0x20000.
