# open-harmony

Working towards open firmware for the Logitech Harmony One universal remote
(USB `046d:c121`).

Status: research. Notes on a second remote, a Harmony 900 (USB networking), are in [`docs/harmony-900.md`](docs/harmony-900.md). The remote can be identified and its flash read over USB;
nothing is written yet. See [`docs/findings.md`](docs/findings.md).

So far:

- concordance can read the identity, the config and the external flash
  (Atmel AT49BV322A, 4 MiB).
- The CPU is almost certainly a Microchip **PIC18F87J50** (USB registers
  match, 128 KiB internal flash, external memory bus).
- A base firmware in the PIC's internal flash runs first, checks the
  application's checksum and only then starts it. Holding **Off** while
  inserting the battery keeps the remote in this base firmware ("Safe
  Mode", firmware 3.2), which still answers concordance and reads the
  flash: a recovery path that needs no hardware.
- The application (firmware 3.4, 60 KiB) runs in place from the external
  flash at 0x20000. Its header and checksum are decoded.

No Logitech firmware or remote configuration is included in this repository.
Dump your own.

## Setup

concordance is not in the Arch repos; build it locally:

```bash
git clone https://github.com/jaymzh/concordance.git src/concordance
git -C src/concordance apply ../../patches/libconcord-dump-size-env.patch
P=$PWD/local
(cd src/concordance/libconcord && autoreconf -i && ./configure --prefix=$P && make && make install)
(cd src/concordance/concordance && autoreconf -i && \
  ./configure --prefix=$P CPPFLAGS=-I$P/include LDFLAGS="-L$P/lib -Wl,-rpath,$P/lib" && \
  make && make install)
```

## Reading the remote

```bash
mkdir -p backup && cd backup
../local/bin/concordance -i -v                              # identity
../local/bin/concordance --dump-config config-backup.EZHex  # keep this!
LC_FW_READ_SIZE=0x40000 ../local/bin/concordance --dump-firmware flash-256k.EZHex
../tools/ezhex2bin.py flash-256k.EZHex flash-256k.bin
../tools/flashmap.py flash-256k.bin

# disassemble the application (needs gputils)
dd if=flash-256k.bin of=app.bin bs=1 skip=$((0x20000)) count=$((0x10000))
../tools/bin2ihex.py app.bin app.hex 0x20000
gpdasm -p p18f87j50 app.hex > app.raw.asm
../tools/pic18fix.py app.raw.asm > app.asm   # decode branches, full 21-bit targets, labels
```

To test the erase/write path, `tools/flashtest.cpp` erases, writes and
verifies the unused flash block 0x030000-0x03FFFF. The patched libconcord
refuses any other address, and the tool only runs in safe mode:

```bash
g++ -O2 -o local/bin/flashtest tools/flashtest.cpp -Ilocal/include \
    -Llocal/lib -Wl,-rpath,$PWD/local/lib -lconcord
local/bin/flashtest
```

`tools/flashapp.cpp` checks or writes an application image at 0x020000.
It validates the header (magic, length, checksum) first, so the base
firmware will accept the image; `write` only runs in safe mode and saves the
current sector to `sector-0x020000-<time>.bin` before erasing:

```bash
g++ -O2 -o local/bin/flashapp tools/flashapp.cpp -Ilocal/include \
    -Llocal/lib -Wl,-rpath,$PWD/local/lib -lconcord
# the stock image, cut from your own dump
head -c $((0x2EA92)) flash-256k.bin | tail -c +$((0x20001)) > app-3.4.bin
local/bin/flashapp check app-3.4.bin   # read-only
local/bin/flashapp write app-3.4.bin   # safe mode only
```

If an image does not start, hold Off while inserting the battery to get
back to safe mode and write a working one.

`tools/patchstr.py` changes text in an image without moving anything and
fixes the checksum, e.g. the "USB Connected" screen message in all four
languages:

```bash
tools/patchstr.py app-3.4.bin app-patched.bin \
  "USB Hat Verbunden=open-harmony" "USB Connecto=open-harmony" \
  "USB a Connecte=open-harmony" "USB Connected=open-harmony"
```

The PIC's internal flash (lower 64 KiB, the base firmware) can be read the
same way:

```bash
LC_FW_READ_ADDR=0xFE0000 LC_FW_READ_SIZE=0x10000 \
  ../local/bin/concordance --dump-firmware internal-64k.EZHex
```

**Warning:** the stock firmware accepts erase and write commands for the
whole external flash *and* the lower 64 KiB of the PIC's internal flash.
Never erase or write 0xFE0000-0xFEFFFF (reset vector, hard brick) or
0x020000-0x02FFFF (running application). See
[Danger zones](docs/findings.md#danger-zones).

## License

MIT, see [LICENSE](LICENSE). The file in [`patches/`](patches/) modifies
concordance and is GPL-3.0 like concordance itself.
