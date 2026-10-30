# ESP32 IR/RF gateway (Kincony KC868-AG)

An ESP32 board with Wi-Fi, an IR transmitter and receiver and a 433 MHz
transmitter and receiver, intended as the network side of the Harmony 900
project: the 900 has no Wi-Fi, but it can send any IR code with its own IR
LED, and the gateway can pass that on to the home network (and send IR and
433 MHz on its own).

## Hardware

- ESP32-D0WD-V3 rev 3.1, 4 MB flash, CH340 USB-serial.
- 433 MHz receiver on GPIO13, transmitter on GPIO22 (from the stock
  firmware's start-up log).
- Opening the serial port resets the board (DTR/RTS wiring).

## Stock firmware (as found)

"ESP32 IR/RF GATEWAY + WEBSOCKET v1.2.1" (Arduino-ESP32 2.0.17), sold as a
Haptique IR extender. Partition table:

| Name | Offset | Size |
|---|---|---|
| nvs | 0x009000 | 20 KiB |
| otadata | 0x00e000 | 8 KiB |
| app0 | 0x010000 | 1920 KiB |
| app1 | 0x1f0000 | 1920 KiB |
| spiffs | 0x3d0000 | 128 KiB |
| coredump | 0x3f0000 | 64 KiB |

It joins the configured Wi-Fi (or opens an access point at 192.168.4.1),
announces itself over mDNS as `haptique-extender.local`, serves an HTTP API
(`/api/ir/send`, `/api/ir/last`, `/api/ir/save`, `/api/rf/send`,
`/api/rf/last`, `/api/wifi/...`, `/api/ota/...`, all behind a token and
challenge-response login under `/api/auth/challenge/...`) and a WebSocket
on port 81 (`/ws`) that broadcasts received codes, e.g.
`{"type":"rf_rx","code":12345,"bits":24,"protocol":1,"pulselen":348}`.
Unknown paths answer `200 Not found`. It prints its auth token on the
serial console at every boot, and counts quick restarts (factory reset
after several in a row; the counter clears after 30 s of normal running).

## Backup

Before any change, the whole flash was read with
`esptool read-flash 0 ALL` (4 MB) and the parts that do not change at run
time (bootloader, partition table, both app slots, SPIFFS) were verified
against the chip with `esptool verify-flash`; NVS changes at every boot
(restart counter). The image holds Wi-Fi credentials and the device token
and stays out of the repository. Restore:

```bash
esptool --port /dev/ttyUSB1 write-flash 0 esp32-flash-full-<time>.bin
```
