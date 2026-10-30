# PlatformIO pre-build step: gzip web/index.html into src/web_index.h so the
# whole web UI ships inside the firmware image (one flash, OTA-able).
import gzip
import os

Import("env")  # noqa: F821  (provided by PlatformIO)

root = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(root, "web", "index.html")
dst = os.path.join(root, "src", "web_index.h")
data = gzip.compress(open(src, "rb").read(), 9)
with open(dst, "w") as f:
    f.write("// generated from web/index.html by tools/embed_web.py\n")
    f.write("#pragma once\n#include <Arduino.h>\n")
    f.write("static const size_t WEB_INDEX_GZ_LEN = %d;\n" % len(data))
    f.write("static const uint8_t WEB_INDEX_GZ[] PROGMEM = {\n")
    for i in range(0, len(data), 20):
        f.write("  " + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
    f.write("};\n")
print("embed_web: %s -> %d bytes gz" % (src, len(data)))
