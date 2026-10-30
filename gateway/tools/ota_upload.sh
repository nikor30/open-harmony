#!/bin/sh
# Install a firmware build on the gateway over Wi-Fi (POST /api/update).
# The password comes from ~/.config/open-harmony/open_harmony_secrets.h (OTA_PASS); the PC only sends, so this
# also works through a firewall that blocks incoming connections.
#   tools/ota_upload.sh [host] [firmware.bin]
cd "$(dirname "$0")/.." || exit 1
HOST=${1:-ir-gateway.local}
BIN=${2:-.pio/build/kc868ag/firmware.bin}
PASS=$(sed -n 's/^#define OTA_PASS "\(.*\)"/\1/p' "$HOME/.config/open-harmony/open_harmony_secrets.h" 2>/dev/null)
curl -fsS -m 120 -H "X-Update-Password: $PASS" -F "firmware=@$BIN" "http://$HOST/api/update" && echo
