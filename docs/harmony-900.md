# Harmony 900 (first contact)

A second remote, a Harmony 900, read with concordance. Nothing written.

```
Model:             Logitech Harmony 900
USB:               046d:c11f, "Belcarra Generic MDLM-BLAN"
Firmware Version:  7.5 (type 0)
Hardware Version:  0.1.15
External Flash:    2 MiB, 01:49 AMD Am29LV160BB
Architecture:      15
Protocol:          15
```

concordance's `SupportedModels.md` lists arch 15 / 900 as: config update
working, learn IR working, firmware update not working. The "Config Flash
Used" line of `concordance -i` shows a nonsense percentage for this remote.

## Connecting on Linux

Unlike the Harmony One (HID), the 900 is a USB network device: the kernel's
`zaurus` driver creates an Ethernet interface, the remote asks for an
address by DHCP, and concordance then talks to it at 169.254.1.2 (TCP port
3074, plus HTTP on port 80).

concordance ships a udev rule and `start_concordance_dhcpd.sh` (dnsmasq)
for this. Without them, on a desktop with NetworkManager and a firewall:

```bash
IF=enp9s0u1u4u4                                 # see `ip -br link`
nmcli device set $IF managed no                 # NM otherwise runs DHCP and drops the address
ip addr add 169.254.1.1/16 dev $IF
tools/usbnet-dhcp.py $IF 120                    # answers the remote's DHCP with 169.254.1.2
ping -c 2 169.254.1.2
local/bin/concordance -i -v
```

`tools/usbnet-dhcp.py` uses a raw packet socket, so it works even when the
host firewall drops incoming DHCP (UFW does by default). All of this is
undone when the remote is unplugged (the interface disappears).

The remote also runs a small web server on port 80 (`Server: Slinger
1.0`); `/` returns 404.

## Regions

The 900 is not addressed by flash address but by region number
(`TRegion` in libconcord's `remote.h`). concordance's `ReadRegion` copies
as many bytes as the remote announces, without a buffer limit, so
[`tools/zregion.cpp`](../tools/zregion.cpp) reads one region per run through
`lc_z_read_region()` from the libconcord patch, which refuses regions
larger than its 4 MiB buffer before any data is transferred.

| Region | Announced size | Result |
|---|---|---|
| 0 bootloader | 16384 | read refused by the remote |
| 1 safe mode | 3359840 | read |
| 2 application | 3359840 | read |
| 3 embedding config | 13057999 | not read (larger than the buffer; probably not a real size) |
| 4 user config | 51025 | read, identical to `concordance --dump-config` |

The connection stayed up after every attempt.

## Firmware: QNX Neutrino on a PXA270

Region 2 (application) is a QNX Neutrino boot image:

- Header magic `ETHANOL`; strings `LogitechEthanol` and `startup-ethanol`
  (the QNX startup program for a board Logitech called "Ethanol").
- `imagefs` (QNX image file system) and QNX startup output such as
  `syspage ptr`, `cpupage ptr`, `callout`.
- `pxa270`: an Intel/Marvell PXA270 (XScale, ARM) CPU.
- NAND flash besides the 2 MiB NOR (`Error in Accessing NAND - Page`,
  `ECC Error Unrecoverable`), and an IPL that picks between images
  (`IPL: 0x%X, Safe: 0x%X, Normal: 0x%X`).
- Mostly compressed (about 7.8 bits/byte throughout).

Region 1 (safe mode) is the **same image**: the two dumps differ only in
header byte 22 (the region number, 1 or 2) and byte 62 (a header checksum
that differs by the same 1). The remote keeps a complete second copy of
its firmware as a fallback.
