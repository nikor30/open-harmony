# PlatformIO pre-script for env:ota: pass the update password from
# ~/.config/open-harmony/open_harmony_secrets.h (root-only, outside the repository) to espota, so it never sits in platformio.ini.
import os
import re

Import("env")  # noqa: F821

path = os.path.expanduser("~/.config/open-harmony/open_harmony_secrets.h")
m = re.search(r'#define\s+OTA_PASS\s+"([^"]*)"', open(path).read()) if os.path.exists(path) else None
if m:
    env.Append(UPLOAD_FLAGS=["--auth=" + m.group(1)])  # noqa: F821
