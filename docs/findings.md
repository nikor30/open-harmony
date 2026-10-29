# Findings

Notes from poking at one Harmony One over USB with concordance. Only
read operations were used; nothing has been written to the remote.

Addresses below are PIC18 program addresses unless they say "flash", which
means a byte offset in the external 4 MiB chip as concordance sees it.

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

`--dump-firmware` nevertheless works: it reads 64 KiB starting at flash
address 0. [`patches/libconcord-dump-size-env.patch`](../patches/libconcord-dump-size-env.patch)
adds two environment variables for read-only dumps of other ranges:
`LC_FW_READ_SIZE` and `LC_FW_READ_ADDR`.

## CPU: PIC18F87J50

Special function registers (SFRs) used by the code, resolved with the
gputils headers:

- Heavy use of `TBLPTR`/`TABLAT` (table reads from flash), `FSR0`, `PORTC`.
- USB registers, all at their PIC18F87J50 addresses: `UCON` (0xF65),
  `UIR` (0xF62), `UIE` (0xF5C), `UEIE`, `UADDR` (0xF5E), `UCFG` (0xF5F),
  `UEP0`-`UEP2` (0xF4C-0xF4E). On the J94 family these addresses are clock
  and RTC registers; its USB block (`UCON` at 0xF75) is never touched.
- Banked accesses to 0xF00-0xF3F, which is plain RAM on the J50.
- Internal flash is programmed in 64-byte blocks (the J50 write block size).

The J50 family with an external memory bus and 128 KiB internal flash is
the 80-pin **PIC18F87J50**. To be confirmed by reading the chip marking.

## Memory map

| PIC address | What | Readable over USB |
|---|---|---|
| 0x000000-0x00AFFF | **internal firmware** ("base"): reset vector, own USB stack, boot decision | yes, as 0xFE0000+ |
| 0x00B000-0x00FFFF | empty | yes |
| 0x010000-0x01FFFF | internal upper half, checksummed by the base firmware; flash driver table at 0x1E00C | **no** |
| 0x020000-0x02002F | header of the application image; 0x020020-0x020029 is overlaid by I/O registers | |
| 0x020000-0x02EA91 | **application** (firmware 3.4), runs in place from external flash | yes, flash 0x020000 |
| 0x130000-0x13FFFF | 64 KiB window into the external flash, bank selected by the latch at 0x020025 | |

### External flash, 0x000000-0x03FFFF

`tools/flashmap.py` on the 256 KiB dump:

| Flash range | Content |
|---|---|
| 0x00000-0x01FFF | almost empty, a few `67`/`F7`/`6F` bytes at the start |
| 0x02000-0x04FFF | data structures; `GSPM` at 0x2000 equals the arch 12 config cookie `0x4D505347` |
| 0x05000-0x1FFFF | empty (0xFF) |
| 0x20000-0x2EA91 | 60050 bytes, the application image |
| 0x2EA92-0x3FFFF | empty |

## The application at 0x20000

Opcode statistics over 0x20000-0x2BFFF (24576 words): 3337 `MOVLW` (`0Exx`),
2813 `MOVWF` (`6Exx`/`6Fxx`), 963 `CALL` (`ECxx`/`EDxx`), 105 `GOTO` (`EFxx`),
324 `RETURN` (`0012`/`0013`). ARM, MSP430 and AVR return opcodes do not occur.

**It executes in place.** All 1315 `CALL`/`GOTO` targets lie in
0x20014-0x2EA8C, i.e. the code is linked for exactly the addresses it has in
the external flash. A PIC18 can only do that through its external memory bus
in extended microcontroller mode. The remaining 7 targets go to the flash
driver table at 0x1E00C-0x1E014.

Header (little endian):

| Offset | Value | Meaning |
|---|---|---|
| +0 | `CD D8` | checksum: XOR of byte pairs +4..end, seeded `21 43` (same algorithm as concordance's `_fix_magic_bytes()`) |
| +2 | `FF FF` | unused |
| +4 | `0xEA8A` | length after +8; 0x20008 + 0xEA8A = 0x2EA92 = end of image |
| +6 | `00 34` | version 3.4 (matches `concordance -i`) |
| +8 | `48 47` | "HG" magic |
| +A | `GOTO 0x2EA38` | entry point |
| +0x20 | `DE AD` x 8 | filler; the bus decodes I/O registers here (see below) |

## Boot flow

Internal flash vectors:

| Vector | Instruction |
|---|---|
| reset 0x0000 | `GOTO 0x000FA4` (C startup, main loop at 0x002C) |
| high-priority interrupt 0x0008 | `GOTO 0x020400` (application) |
| low-priority interrupt 0x0018 | `GOTO 0x020800` (application) |

So the base firmware in internal flash always runs first. At 0x54C0 it
decides whether to start the application:

0. 0x498C probes the I/O chip (0x020024 must read neither 0x00 nor 0xFF) and
   initialises it; if that fails the application is not started.
1. 0x934C scans the key matrix: it drives lines through 0x020020 (active
   low, binary search over 8 lines) and reads the return lines at 0x020021
   (first clear bit). Code = 8 x drive line + read line + 1 (lines 0-based),
   0 = no key. If the code is 0x25 (drive 4 / read 4), 0x1C (3/3) or 0x1E
   (3/5) the application is **not** started. This matches Logitech's
   "Safe Mode" on other Harmony models (hold Off while inserting the
   battery); which physical buttons the three positions are is not yet known.
2. 0x5814 recomputes the checksum over the image at 0x020000 and compares it
   with the stored one; only if they match does the code write `0x02:00:0A`
   to `PCLATU`/`PCLATH`/`PCL`, i.e. jump to the application entry.

0x584E does the same check for memory type 0xFE at 0x010000 (the internal
upper half). Another computed jump at 0x53B6 goes to 0x0210F0; its purpose
is still open. A block at internal 0x6092 has its own startup code and calls
0x0210F0-0x0211B6, so it was linked for another address (probably a staged
module).

### Safe mode (tested)

Holding **Off** while inserting the battery shows "Safe Mode" on the
screen; the remote then runs the base firmware only. Over USB:

| | Normal | Safe mode |
|---|---|---|
| USB strings | `Harmony Remote 0-3.4.0` | `Harmony Remote 4-3.2.0` |
| Firmware version | 3.4 | 3.2 |
| Firmware type | 0 | 4 |
| Everything else in `concordance -i -v` | | identical |

A 256 KiB `--dump-firmware` of flash 0x000000-0x03FFFF in safe mode is
byte-for-byte identical to the normal-mode dump, so the base firmware
serves `READ_FLASH` with the same addressing. Leave safe mode by removing
and reinserting the battery without holding a key.

This makes safe mode the recovery path: the base firmware (3.2) lives in
internal flash, answers USB and does not run the application. It is also
the natural place to rewrite 0x020000, because the code being replaced is
not running.

### Safe mode USB commands (from the disassembly)

The dispatcher at 0x6BB2 handles six commands; there is no IR capture:

| Command | Handler |
|---|---|
| `B0` READ_MISC | 0x6DD6 |
| `A0` WRITE_MISC | 0x6D0A |
| `D0` ERASE_FLASH | 0x6C72 |
| `50` READ_FLASH | 0x6C28 |
| `30` WRITE_FLASH | 0x6BDE |
| `10` GET_VERSION | 0x6BD0 |

- The address check at 0x6AA0 is the same as the application's:
  0x000000-0x3FFFFF external flash, 0xFE0000-0xFEFFF8 internal flash.
- External erase uses the same bank latch (0x020025, `H - 3`) and window
  (0x13xxxx), but the base firmware's own routine at 0xA34C, not the
  0x1E00C table.
- Internal erase (0xA83C) only checks a guard flag (RAM 0x110 bit 4, set by
  the USB handler; otherwise `RESET`) against stray jumps. There is **no
  address protection**: over USB, safe mode erases its own code at
  0xFE0000+ just as willingly as the application does.
- Safe mode never enables interrupts. Its enable routine (0xAD68) has no
  callers; flash operations only save and restore the (off) state. So the
  application's interrupt vectors at 0x020400/0x020800 are never used in
  safe mode, and a broken application cannot crash it. (No indirect calls
  were checked.)

Holding Off is checked before the application's checksum, so even a
corrupt application with a valid checksum cannot lock out safe mode.

### Write test (done)

`tools/flashtest.cpp` in safe mode on the unused block 0x030000-0x03FFFF:
pre-check empty, erase, write a 64 KiB pattern, read back and compare,
erase, check empty. All steps passed. Afterwards, flash 0x000000-0x03FFFF
was byte-for-byte identical to the backup taken before the test, and the
config dump differed only in the `<SOFTWARETYPE>` header field (0 in
normal mode, 4 in safe mode), not in the data.

So the erase and write commands of the base firmware work on the external
flash, and concordance's `EraseFlash`/`WriteFlash` drive them correctly.

### Application round trip (done)

In safe mode, `tools/flashapp.cpp` saved sector 0x020000, erased it, wrote
the original 3.4 image (60050 bytes, cut from the dump) and verified the
whole sector. After reinserting the battery without a key the remote
started normally (`Harmony Remote 0-3.4.0`, firmware 3.4 type 0), all
buttons worked, and `flashapp check` matched the image.

This is the complete update path, with known-good code: safe mode, erase,
write, verify, normal boot.

### First modified image (done)

`tools/patchstr.py` replaced the four built-in "USB connected" strings
with "open-harmony" (58 bytes changed: 56 text bytes and the checksum).
Written in safe mode with `flashapp`, the image booted normally and
`flashapp check` matched it. So a modified image with a recomputed
checksum is accepted and runs.

The screen did **not** change: when USB is plugged in it still shows
"USB-Verbindung hergestellt". That text is in none of the dumps as ASCII,
UTF-16, single-byte XOR or shifted text, so it comes from the
configuration (generated in the user's language by Logitech's software),
stored in some other form (compressed or pre-rendered).

The built-in strings are a fallback. They sit behind a table of 3-byte
pointers at 0x2E982 (order: English, French, Spanish, German):

| Table | Messages |
|---|---|
| 0x2E982 | "Hello" / "Bonjour" / "Buenos Dias" / "Guten Tag" |
| 0x2E98E | "USB Connected" / "USB a Connecte" / "USB Connecto" / "USB Hat Verbunden" |
| 0x2E9AA | "System Error" / "Message D Erreur" / "Error Inesperado" / "Unerwarteter Fehler" |

They are read by 0x2088A, which picks one of four messages from flags in
RAM 0x2B3 (set from 0x2B4 by 0x20858/0x20862) and the language from
0x2B5. 0x2088A has no direct callers (it is probably reached through a
function pointer), so when the fallback is used is still open. (The first `write` attempt failed at
`get_identity` right after the remote enumerated; a retry a few seconds
later worked.)

## Flash driver (internal flash, 0x1E00C-0x1E014)

Five one-word entries (each a branch). Parameters go in RAM 0x60-0x62
(24-bit address, low byte first) and 0x63-0x65 (data). The application wraps
every call in 0x2E8F8 / 0x2E908, which save `INTCON.GIE`, disable interrupts
and restore them afterwards.

| Entry | Called from | Arguments | Likely function |
|---|---|---|---|
| 0x1E00C | 0x2DDCE, 0x2DDEC | address 0x022000 / 0x022002, result in 0x63 | read flash ID (results stored to 0xD54/0xD55; `1F`/`C8` = Atmel AT49BV322A) |
| 0x1E00E | 0x2DEDA | address | erase sector |
| 0x1E010 | 0x2DE92 | address, 1 data byte | program |
| 0x1E012 | 0x2DEBC | address, 3 data bytes | program (wider) |
| 0x1E014 | 0x2C8C2, 0x2CA24 | none, called in a loop with `CLRWDT` | sleep |

These have to run from internal flash: while the external chip is in ID,
erase or program mode the CPU cannot fetch code from it.

The application carries a matching driver at 0x2CD3A: a `RESET` word, then
five `BRA` entries (0x2CD3C-0x2CD44) and JEDEC helpers (unlock `AA`/`55`
at xAAA/x555, command, `F0` reset, DQ6 toggle polling). Its fifth entry
(0x2CF6A) executes `SLEEP`. Nothing in the application calls it; it uses
only relative branches, so it is position independent and is most likely
the payload that gets installed at 0x1E00A.

## I/O registers at 0x020020-0x020029

Both firmwares access these with table instructions at constant addresses
instead of reading flash. Accesses with a constant `TBLPTR` found so far
(base firmware: reads only scanned):

| Address | Application | Base firmware | Notes |
|---|---|---|---|
| 0x020020 | read/write | read | |
| 0x020021 | read | read | |
| 0x020022 | | read | |
| 0x020023 | write | | |
| 0x020024 | read | read | |
| 0x020025 | write | read | **flash bank latch** |
| 0x020027 | write | read | |
| 0x020029 | | read | |

### Flash banking

To access flash address `H:M:L`, the application writes `H - 3` to the latch
at 0x020025 and then read, erase or program at PIC address `0x13:M:L`
(RAM 0x19C-0x19E holds the address, 0x1A2 the latch value). So
0x130000-0x13FFFF is a 64 KiB window, and

    flash block = (latch + 3) mod 64

The USB read and erase handlers use the same window. Reading flash 0x02xxxx
(latch 0xFF) returns exactly the code that is running, so the window does
reach the application block.

## USB command handling

The command byte is in RAM 0xD1F. The dispatcher at 0x26492 switches on its
high nibble, matching concordance's `protocol.h`:

| Command | Handler |
|---|---|
| `B0` READ_MISC | 0x2671E |
| `A0` WRITE_MISC | 0x26626 |
| `D0` ERASE_FLASH (`D3`) | 0x2657A |
| `70` START_IRCAP | 0x26556 |
| `50` READ_FLASH | 0x2650C |
| `30` WRITE_FLASH | 0x264C2 |
| `10` GET_VERSION | 0x264B4 |

Every flash command reads a 3-byte address, high byte first, into
0x287/0x286/0x285 and classifies it with 0x2637A:

| Address | Memory type (RAM 0x28B) |
|---|---|
| 0x000000-0x3FFFFF | 0x00, external flash, through the bank window |
| 0xFE0000-0xFEFFF8 | 0xFE, **internal PIC flash** 0x0000-0xFFF8 (`TBLRD`; erase/write via `EECON1`/`EECON2` at 0x2E6A8-0x2E6EA) |
| anything else | rejected |

concordance reads in 1022-byte chunks, each with its own address check, so
the internal upper half (0x10000-0x1FFFF, including the flash driver) can
neither be read nor erased with these commands.

## Danger zones

The stock firmware does no further checks. Over USB, without opening the
remote:

- **0xFE0000-0xFEFFFF: never erase or write.** It is the base firmware with
  the reset vector. Losing it is a hard brick that only an ICSP programmer
  can fix.
- **0x020000-0x02FFFF: never erase or write** with the current tools. It
  is the running application; the base firmware probably recovers from a
  bad image, but that is not tested.

## What this means for custom firmware

- A replacement application can be linked for 0x20000 with the known header
  and checksum and written over USB, leaving the base firmware and the flash
  driver in the PIC untouched. If the base firmware's checksum check works as
  read, a broken image keeps the remote in the base firmware, still on USB.
- Interrupt vectors are fixed at 0x020400 (high) and 0x020800 (low).
- The application can use the flash driver at 0x1E00C-0x1E014 and the I/O
  registers at 0x020020-0x020029.

## Next steps

1. Round trip of the real application: in safe mode, erase 0x020000,
   write back the original 3.4 image byte for byte, verify, and check that
   the remote boots normally. Proves the update path with known-good code.
2. Open the remote, confirm the PIC18F87J50 marking, and find the ICSP pads
   (MCLR, PGC, PGD, VDD, GND). With a PICkit the whole internal flash,
   including 0x10000-0x1FFFF, can be backed up and restored.
3. Only then: build a minimal test application (e.g. toggle a backlight pin)
   and flash it to 0x20000.
