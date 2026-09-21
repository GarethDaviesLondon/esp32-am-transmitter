"""Constants, and the sibling imports the whole tool builds on.

Separate from the window so that a panel can ask for the RF field table or the
transmitter names without importing the tab that owns it, which would be a
circle: the tab imports the panels.

The flasher is imported here, once, and the monitor comes through it. Module
names are prefixed `programmer_` because these tools are scripts on `sys.path`
rather than packages, and this tool puts three tool directories on it.
"""

import sys
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOL_DIR))
sys.path.insert(0, str(TOOL_DIR.parent / "esp-flasher"))
import esp_flasher as flasher  # noqa: E402

monitor = flasher.monitor


RECONNECT_TIMEOUT = 20.0        # seconds to wait for a board to come back after a reset
RECONNECT_RETRY_SECONDS = 1.0   # between automatic reconnect attempts
AUTO_REFRESH_SECONDS = 5.0
MONO_FONT = monitor.MONO_FONT

# One entry per rf key, in design 07's order: label, widget kind, range.
RF_FIELDS = [
    ("on", "Carrier output", "check", None),
    ("hz", "Carrier frequency (Hz)", "spin", (60000, 312500, 1000)),
    ("depth", "Modulation depth (%)", "spin", (5, 90, 1)),
    ("lpf", "Audio low-pass (Hz)", "spin", (500, 9500, 100)),
    ("gain", "Programme gain (%)", "spin", (0, 800, 5)),
    ("level", "Carrier level (%)", "spin", (5, 100, 1)),
    ("agc", "Loudness AGC", "check", None),
    ("agct", "AGC target (%)", "spin", (5, 90, 1)),
    ("fade", "Fading", "fade", None),
    ("faderate", "Fade rate (mHz)", "spin", (10, 12000, 10)),
    ("fadedepth", "Fade depth (%)", "spin", (0, 100, 1)),
]

# Where each rf key lives in the /api/state document's chain.rf object.
STATE_RF_FIELD = {"on": "on", "hz": "hz", "depth": "depth", "lpf": "lpf", "gain": "gain", "level": "level",
                  "agc": "agc", "agct": "agct", "fade": "fdm", "faderate": "fdr", "fadedepth": "fdd"}

LEGACY_COMMANDS = ("status", "wifi", "log", "reboot", "help")
TX_NAMES = ("A", "B")
