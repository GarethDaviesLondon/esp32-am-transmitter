"""Constants, the sibling imports, and the one flag that says "the user stopped".

Every other module here starts from this one, which is also where the two
optional imports live: the monitor (a sibling tool, imported rather than
forked, so a fix there reaches both) and esptool itself. Both are attempted
once; `main()` reports a missing esptool properly rather than each call site
guessing.

Module names are prefixed `flasher_` because these tools are scripts on
`sys.path` rather than packages, and the flasher puts its own directory and the
monitor's on the path together.
"""

import sys
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parent.parent

# The monitor is a sibling tool; import it rather than fork it, so a fix there
# reaches both.
sys.path.insert(0, str(TOOL_DIR.parent / "esp-serial-monitor"))
import esp_serial_monitor as monitor  # noqa: E402

try:
    import esptool
    from esptool.cmds import detect_chip
except ImportError:  # reported properly in main()
    esptool = None
    detect_chip = None


# =========================

# chip_id field of the ESP-IDF image header, per esp_app_format.h.
CHIP_IDS = {
    0x0000: "ESP32",
    0x0002: "ESP32-S2",
    0x0005: "ESP32-C3",
    0x0009: "ESP32-S3",
    0x000C: "ESP32-C2",
    0x000D: "ESP32-C6",
    0x0010: "ESP32-H2",
    0x0012: "ESP32-P4",
}

IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 32                 # 24-byte image header + 8-byte segment header
PARTITION_ENTRY_MAGIC = b"\xaa\x50"
PARTITION_MD5_MAGIC = b"\xeb\xeb"
PARTITION_TABLE_SIZE = 0xC00
DEFAULT_PARTITION_TABLE_OFFSET = 0x8000
DEFAULT_APP_OFFSET = 0x10000

DEFAULT_FLASH_BAUD = 460800          # cosmetic on native USB, real on a UART bridge
FLASH_BAUDS = [115200, 230400, 460800, 921600, 1500000, 2000000]
CONNECT_TIMEOUT = 25.0               # seconds to keep trying to catch the board
PORT_POLL_SECONDS = 0.15             # a boot-looping S3 is off the bus for ~0.4 s
LOOP_WINDOW_SECONDS = 12.0           # drops within this window mean "boot-looping"
FLASH_ATTEMPTS = 3                   # retries when esptool fails before writing
REAPPEAR_TIMEOUT = 15.0              # seconds to wait for the port after a reset

MONO_FONT = monitor.MONO_FONT


class Cancelled(Exception):
    """The operator pressed Stop."""
