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

## Firmware: QNX Neutrino 6.3.2 on a Freescale i.MX21

Region 2 (application) is a QNX Neutrino boot image for a Logitech board
called "Ethanol". [`tools/qnxifs.py`](../tools/qnxifs.py) decompresses it
(UCL/NRV2B blocks) and unpacks the image file system (198 entries):

- Boot banner: `Welcome to Logitech Ethanol -- QNX Neutrino 6.3.2`.
- CPU: Freescale **i.MX21** (ARM926, ARMv5 little-endian). Named by the
  drivers `i2c-imx21`, `devg-imx21_logitech`, `devi-imx21s` and the graphics
  config `drivername=imx21_logitech`. (The `pxa270` string in the raw dump
  is just one entry in the startup code's SoC name table, not this board.)
- Also NAND flash (`Error in Accessing NAND - Page`, `ECC Error
  Unrecoverable`) and an IPL that selects an image
  (`IPL: 0x%X, Safe: 0x%X, Normal: 0x%X`).

Region 1 (safe mode) is the **same image**: the two dumps differ only in
header byte 22 (the region number, 1 vs 2) and byte 62 (a header checksum,
off by the same 1). The remote keeps a full second copy as a fallback.

### What runs

`/proc/boot/.script` starts procnto, the serial driver, mounts the ETFS
(NAND) regions, then runs `/etc/rc.d/rc.sysinit`, which chains
`rc.network`, `rc.platform`, `rc.application`:

- **Networking**: `io-net -dusbdnet -ptcpip -ppppmgr` (the USB-network
  link to the PC), `inetd`, and `ethanol-usbd` (the config protocol on
  TCP 3074). `inetd.conf` also offers ftp/telnet, and `qconn` (the QNX
  remote-debug agent) starts on port 8000 if `/fs/etfs/scratch/qconnboot`
  exists.
- **Platform**: `psrv`, power (`tps65800`), `tilt`, `irgen` (IR blaster),
  `ircap` (IR learning), per-variant Z-Wave (`rfgen-zw`) and audio.
- **UI**: an **Adobe Flash Lite** player (`flash.sh -f flashlite-565.so
  /usr/local/app/main.swf`). The actual interface, `main.swf`, lives on
  the ETFS/NAND (`/usr/local/app`), not in this boot image.

### Who talks to the PC

- `ethanol-usbd` is only the USB network gadget (Belcarra USBLAN). On
  connect it runs `/etc/net/usb/usbd_connect`: take the address from the
  host or run `dhcp.client`, then start `mDNSResponder`.
- `/etc/net/mdns_services` advertises **"Remote3"**, service
  `_remotesrv._tcp` on **port 3074** (the port concordance uses), with
  `UsbProductId=0xC11F`.
- `/usr/local/sbin/update` is a QNX resource manager that serves the
  regions listed in `/etc/regiontab`, with a cache under
  `/var/run/update/cache` and `.version` files per region.
- The program that actually listens on 3074 is not in the boot image; it
  is on the NAND (see below).

### Region table (`/etc/regiontab`)

| # | Name | Kind | Where |
|---|---|---|---|
| 0 | ipl | flash | 0x000000, 16 KiB |
| 1 | safe_mode | flash | 0x200000, 4 MiB |
| 2 | normal_mode | flash | 0x600000, 8 MiB |
| 3 | etfs_base | fs | the whole NAND file system |
| 4 | config_base | fs | user configuration |
| 6 | man_data | flash | 0x004000 |
| 7 | zw_fw_dev | dev | Z-Wave radio firmware |
| 11 | media_home | fs | `/usr/media` |
| 12 | appl_home | fs | `/fs/etfs/platform` (Logitech programs, UI, Lua) |
| 14 | ui_appl_home | fs | `/usr/flashlite` (Flash Lite player, fonts) |
| 16 | ecnet_home | fs | `/usr/device/ecnet` |
| 23 | preload | flash | 0xE00000, 32 MiB |

"fs" regions are delivered as **ZIP archives** when read.

## NAND software (regions 12 and 14)

Read with `zregion` (limit raised to 32 MiB), both are ZIP archives dated
2010-04-05:

- **Region 12, platform** (7.0 MB ZIP, 607 files, 8.5 MB unpacked):
  - `app/main.swf`, `app/app-main.swf` (1.5 MB, the Flash UI),
    `app/oob-main.swf` (out-of-box setup), `vodkatest.swf`,
    `message-fwsuccess.swf`, `message-badguid.swf`, and hundreds of PNG
    assets per variant (even a leftover `.psd`).
  - `app/RemoteStates.xml` and translation tables.
  - Programs: `hao`, `sys_srv`, `data_srv`, `env_mgr`, `ir_mgr`, `irgen`,
    `ircap`, `link_proxy`, `ltcp_repeater`, `ecnet_mgr`, `rfgen-zw`,
    `rfgen-ecnet`, `notify_mgr`, `psrv`, `whyboot`, and a `lua`
    interpreter.
  - **Lua 5.1 with Logitech's logic as plain source**: 29 `.lua` files,
    about 6,900 lines in `share/lua/5.1/ethanol/` (`HAO.lua`, `Activity.lua`,
    `Device.lua`, `Infrared.lua`, `IRQueue.lua`, `Controller.lua`,
    `BindingList.lua`, `RemoteInfo.lua`, `RFService.lua`, `HelpEngine.lua`,
    ...), plus LuaSocket and LuaExpat.
- **Region 14, flashlite** (3.5 MB ZIP, 53 files): Flash Lite players for
  RGB565/555/8888, video and sound decoders, fonts (incl. CJK),
  translations.
- Region 14 failed once at the identity step right after the previous read
  and worked on a retry a few seconds later.

This makes the 900 a much easier modding target than the One: the logic is
Lua source and the UI is a Flash file, both inside a ZIP that the remote's
own update mechanism handles. Writing a region back has **not** been tried.

### Lua modules (`share/lua/5.1/ethanol/`)

| Module | Lines | Role |
|---|---|---|
| `HAO.lua` | 1228 | central dispatcher: UI events, action lists, inter-key delays |
| `RFService.lua` | 1247 | RF receivers (extenders) and their database |
| `SysService.lua` | 796 | backlight fade, sleep/wake, battery, HTTP to local services |
| `IRQueue.lua` | 665 | IR send queue, device delays, look-ahead |
| `Activity.lua`, `Device.lua`, `State.lua` | ~1400 | activities, devices, power/input state |
| `HelpEngine.lua` | 503 | the Help button (resync devices) |
| `Numeric.lua` | 274 | number entry |

Backlight level and timeout in `SysService.lua` are overwritten from the
user settings (`/system/backlightlevel`, timeout) at start and on change,
so their defaults have no visible effect. `fadedown()` and `fadeup()` are
dead code (their calls are commented out); the backlight is switched by
`lightsUp()` and `lightsDown()`, so
[`mods/h900-slowfade`](../mods/h900-slowfade) has no visible effect.

### Boot modes and recovery

`/etc/rc.d/rc.normal` runs `rc.services` (`data_srv` from the NAND, web
root `/usr/data`), `rc.platform` (drivers from the NAND), `rc.application`
(the Flash UI), then `ltcp_repeater` and `link_proxy` (NAND).
`/etc/rc.d/rc.safe` starts **nothing from the NAND**.

Port 3074 is not an `inetd` service (`inetd.conf` only runs ftp, telnet
and discard).

### Safe mode (tested)

Holding Off while inserting the battery shows "Safe Mode". In safe mode:

- Port **3074 is open**, port 80 is closed (`data_srv`, the web server, is
  only started by normal mode). So the protocol service comes from the boot
  image, not from region 12.
- Stock concordance fails to identify the remote here: `GetIdentity` ends
  with an HTTP request to port 80 for "XMLUserRFSetting" and treats a
  failure as fatal. That value is only used by `web.cpp` (Logitech's
  website). The libconcord patch makes the step non-fatal (empty string).
- With that, `concordance -i` reports the same remote with **firmware type
  4** (normal mode: type 0), like the Harmony One's safe mode.
- Region 12 read in safe mode has exactly the same contents as in normal
  mode (556 files, none missing or different).

So safe mode starts only from the boot image (of which the remote keeps two
copies), answers concordance and serves region 12. That is the recovery
path for a failed region-12 write.

### Region 12 round trip (done)

[`tools/zwrite.cpp`](../tools/zwrite.cpp) writes region 12 with the same
command sequence concordance uses for config updates (`START_UPDATE`,
`WRITE_UPDATE_HEADER`, `WRITE_UPDATE_DATA` in 1 KiB chunks,
`WRITE_UPDATE_DATA_DONE`, `GET_UPDATE_CHECKSUM`, `FINISH_UPDATE`), with the
region number changed. The libconcord patch only allows region 12; the tool
requires safe mode and a ZIP that passes `unzip -t`, saves the current
region first and reads it back afterwards.

In safe mode the unchanged original region 12 (7 MB) was written in about
2 minutes. Read back: all 556 files present with identical contents and all
28 programs in `bin/` still executable; only 20 settings cache files in
`cache/` came back as `0775` instead of `0600`/`0644` (normal mode wipes
and rebuilds that directory at start). After reinserting the battery the
remote started normally (firmware type 0, port 80 open, so `data_srv` from
region 12 runs) and worked as before.

To leave safe mode, reinsert the battery without holding a key.

### First visible mod (done)

[`mods/h900-wakeblink`](../mods/h900-wakeblink) makes `lightsUp()` blink
the backlight three times whenever the LCD is switched on. Written to
region 12 in safe mode with `zwrite` (read-back identical); after a normal
boot the remote blinks at start and on every wake-up and otherwise works
as before. So modified Lua in region 12 runs on the remote.

### Hardware variants

`display.conf` and `rc.platform` select between three product variants by a
display/board id: **cognac** and **hennessy** are 320x240 landscape;
**vodka** is 240x320 portrait and is the Harmony 900. All RGB565. The
image carries Logitech logo JPEGs per variant.

### Credentials

The image contains `/etc/passwd`, `/etc/shadow` and `/etc/ftpusers`. Their
contents are deliberately **not** reproduced here or extracted; only their
presence is noted. Anyone studying the login/telnet/ftp surface should do
so on their own unit.
