#!/bin/sh
# Run the hub on this machine: http://localhost:8099/
# HUB_HA_TOKEN_FILE holds a Home Assistant long-lived access token.
cd "$(dirname "$0")"
[ -d .venv ] || { python3 -m venv .venv && .venv/bin/pip install -q aiohttp; }
export HUB_DATA="${HUB_DATA:-$PWD/data}"
export HUB_HA_URL="${HUB_HA_URL:-http://homeassistant.local:8123}"
exec .venv/bin/python app/hub.py
