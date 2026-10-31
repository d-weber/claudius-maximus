#pragma once
// Firmware version: reported in the hello ({"t":"hello","fw":...}) and on the Settings tile. Single source,
// also read by tools/fw_bundle.py (manifest.json `fw_version`); the desktop app compares it with the version it
// ships (companion/flasher.py needs_update) to offer an update. Bump it on every release that changes firmware.
#define FW_VERSION "0.7.1"
