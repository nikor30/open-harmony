# IR/RF gateway firmware (Kincony KC868-AG)

Own firmware for the ESP32 IR/RF gateway (see
[`docs/esp32-gateway.md`](../docs/esp32-gateway.md) for the board and the
backup of its stock firmware). It is the networked side of open-harmony:
it learns and sends infrared (IRremoteESP8266, 100+ protocols, raw
otherwise) and 433 MHz (rc-switch), keeps a library of devices, commands
and activities, serves a web UI and links itself to Home Assistant.

- **Web UI** at `http://ir-gateway.local/` (embedded in the firmware):
  devices with their commands as tiles (tap = send), learning with any
  remote ("+ Learn": press the original button once), activities as
  macros with pauses, a live feed of received codes, settings (Wi-Fi,
  MQTT), library export/import and firmware upload.
- **Home Assistant** via MQTT discovery (device "IR Gateway"): a button per
  command and per activity, the event entity *Received code* (fires for
  every IR or 433 MHz code heard, e.g. from the Harmony 900; attributes
  carry protocol/value) and the sensor *Last code*. Topics:
  `irgw/<id>/send/<device>/<command>`, `irgw/<id>/activity/<id>`,
  `irgw/<id>/code` (raw code JSON), `irgw/<id>/received`, `irgw/<id>/status`.
- **REST API**: `GET /api/status`, `GET|PUT /api/db`, `GET /api/last`,
  `POST /api/send` (`{"device","command"}` or `{"code":{...}}`),
  `POST /api/run` (`{"activity"}`), `POST /api/config`,
  `POST /api/update` (multipart `firmware`). WebSocket `/ws` streams
  `rx`, `sent`, `activity_start|done`, `db` events.

Pins: IR receive GPIO23, IR send GPIO2 (7 IR LEDs), 433 MHz receive GPIO13,
send GPIO22.

Codes are stored as JSON: `{"kind":"ir","protocol":"NEC","value":"20DF10EF","bits":32}`,
`{"kind":"ir","protocol":"UNKNOWN","khz":38,"raw":[...µs...]}`, state
protocols (air conditioners) with `"state":"<hex>"`, or
`{"kind":"rf","code":5393,"bits":24,"protocol":1,"pulse":350}`.

## Build and flash

```bash
pio run                          # build (web/index.html is embedded by tools/embed_web.py)
pio run -t upload                # first time over USB (CH340, resets the board)
tools/ota_upload.sh              # later, over Wi-Fi: POST /api/update with the password
```

Updates need the password `OTA_PASS` from `~/.config/open-harmony/open_harmony_secrets.h` (header
`X-Update-Password`; the web UI asks for it). `pio run -e ota -t upload`
(ArduinoOTA/espota) also works where the PC accepts the board's connection
back; with an inbound-blocking firewall use `tools/ota_upload.sh`.

`~/.config/open-harmony/open_harmony_secrets.h` (root-only, outside the repository, mode 600)
may define `WIFI_SSID`/`WIFI_PASS` for the first
start; without Wi-Fi the gateway opens the access point `IR-Gateway-Setup`
(web UI at 192.168.4.1). Settings are kept in NVS, the library in
LittleFS (`/db.json`, partition `littlefs`, 896 KB).
