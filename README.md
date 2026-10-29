# open-harmony

Working towards open firmware for the Logitech Harmony One universal remote
(USB `046d:c121`).

Status: research. The remote can be identified and its flash read over USB;
nothing is written yet. See [`docs/findings.md`](docs/findings.md).

So far:

- concordance can read the identity, the config and the external flash
  (Atmel AT49BV322A, 4 MiB).
- The external flash at 0x20000 holds a ~48 KiB PIC18 program image, so the
  remote most likely runs on a Microchip PIC18.

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
```

## License

MIT, see [LICENSE](LICENSE). The file in [`patches/`](patches/) modifies
concordance and is GPL-3.0 like concordance itself.
