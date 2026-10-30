# open-harmony Hub (Home Assistant add-on)

The management side of open-harmony: one place for the IR/433 MHz gateways,
their devices and commands, and **activities** that mix gateway commands
with any Home Assistant service ("Watch a movie" = projector on via HA,
wait, Blu-ray power via IR, receiver input via 433 MHz ...).

- **Activities**: start steps and stop steps; switching runs the current
  activity's stop steps first, then the new one's start steps. Step kinds:
  `send` (gateway device/command), `ha` (service + entity), `wait` (ms).
  Progress is shown live; a failing step is reported and skipped.
- **Devices**: every gateway's library mirrored live (its WebSocket), tap a
  tile to send, learn new commands with the original remote (the gateway
  hears it; the hub saves it into the gateway's library).
- **Settings**: add/remove gateways by URL, export/import activities.

Runs in the Home Assistant sidebar ("Remote") through ingress and uses the
Supervisor token for the HA API. Data in `/data/hub.json`.

## Install

Add `https://github.com/nikor30/open-harmony` as an add-on repository
(Settings → Add-ons → Add-on store → ⋮ → Repositories), install
*open-harmony Hub*, start, open "Remote" in the sidebar. Or copy this folder
to `/addons/open_harmony_hub` (Samba share `addons`) and install it from
*Local add-ons*.

## Develop

```bash
HUB_HA_URL=http://192.168.10.10:8123 HUB_HA_TOKEN_FILE=~/ha-token ./run_dev.sh   # http://localhost:8099/
```

API: `GET /api/state`, WebSocket `/api/events` (state, gateway, step,
step_error, current, gw_event), `POST /api/gateways {url}`,
`DELETE /api/gateways/<id>`, `POST /api/gateways/<id>/send`,
`POST /api/gateways/<id>/commands {device,name,code}`,
`PUT /api/activities`, `POST /api/switch {activity|null}`,
`POST /api/step` (run one step), `GET /api/ha/entities`, `GET /api/ha/services`.

Next: activities as a Home Assistant `select` entity (MQTT), Harmony 900
configuration (activities and buttons pushed to the remote's ohui).
