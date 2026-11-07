# PlatformIO pre-script: stamps the firmware version.
# CI sets FW_VERSION (GitHub run number). Local builds default to 0 = dev build,
# which disables automatic OTA so USB-flashed test firmware is not overwritten.
import os
Import("env")

raw = os.environ.get("FW_VERSION", "0").strip() or "0"
try:
    version = int(raw)
except ValueError:
    version = 0
env.Append(CPPDEFINES=[("FW_VERSION", version)])
print("FW_VERSION=%d" % version)
