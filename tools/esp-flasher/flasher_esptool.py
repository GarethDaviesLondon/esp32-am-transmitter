"""Running esptool, and papering over the difference between v4 and v5.

esptool renamed its command-line words between major versions, so every
invocation goes through `esptool_word`, and it is run as a child process rather
than called in-process: a flash that fails half way should not take the GUI
with it. `TextSink` is how its output reaches the log.
"""

import io
import os
import re
import subprocess
import sys
import time

from flasher_config import (
    Cancelled,
    DEFAULT_FLASH_BAUD,
    FLASH_ATTEMPTS,
    REAPPEAR_TIMEOUT,
    esptool,
)
from flasher_bus import find_port


# =========================

def esptool_major():
    try:
        return int(esptool.__version__.split(".")[0])
    except Exception:
        return 4


def esptool_word(name):
    """esptool v5 spells commands and options with hyphens, v4 with underscores."""
    return name if esptool_major() >= 5 else name.replace("-", "_")


def first_line(exc):
    text = str(exc).strip() or type(exc).__name__
    return text.splitlines()[0]


class TextSink(io.TextIOBase):
    """A file-like object that forwards writes to a callback (for esptool's prints)."""

    def __init__(self, emit):
        self.emit = emit

    def write(self, text):
        if text:
            self.emit(text)
        return len(text)

    def flush(self):
        pass


# =========================

def select_images(firmware, mode):
    if mode == "app" and firmware.image("app"):
        return [firmware.image("app")]
    return list(firmware.images)


def build_flash_command(firmware, device, mode, erase=False, baud=DEFAULT_FLASH_BAUD):
    word = esptool_word
    argv = [sys.executable, "-u", "-m", "esptool",
            "--chip", firmware.chip or "auto", "-p", device, "-b", str(baud),
            "--before", word("default-reset"), "--after", word("hard-reset"),
            word("write-flash")]
    if erase:
        argv.append("--erase-all")
    settings = firmware.flash_settings
    if settings:
        argv += [word("--flash-mode"), settings["flash_mode"],
                 word("--flash-freq"), settings["flash_freq"],
                 word("--flash-size"), settings["flash_size"]]
    for img in select_images(firmware, mode):
        argv += ["0x{:x}".format(img["offset"]), img["path"]]
    return argv


def run_esptool(argv, log, cancel=None, holder=None):
    """Run esptool as a child process, streaming its output. Returns (returncode, output)."""
    env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, env=env, creationflags=flags)
    if holder is not None:
        holder["proc"] = proc
    decoder = io.IncrementalNewlineDecoder(None, translate=False)
    captured = []
    try:
        while True:
            chunk = proc.stdout.read1(512) if hasattr(proc.stdout, "read1") else proc.stdout.read(1)
            if not chunk:
                break
            text = chunk.decode("utf-8", errors="replace")
            captured.append(text)
            log(text)
            if cancel is not None and cancel.is_set():
                proc.terminate()
    finally:
        proc.wait()
        if holder is not None:
            holder["proc"] = None
    return proc.returncode, "".join(captured)


def flash_board(key, firmware, mode, erase, baud, log, cancel=None, holder=None):
    """Write the images, retrying when esptool loses the board before writing anything."""
    for attempt in range(1, FLASH_ATTEMPTS + 1):
        if cancel is not None and cancel.is_set():
            raise Cancelled()
        port = find_port(key)
        deadline = time.monotonic() + 5
        while port is None and time.monotonic() < deadline:
            time.sleep(0.05)
            port = find_port(key)
        if port is None:
            log("The board is not on the bus.\n")
            continue
        argv = build_flash_command(firmware, port["device"], mode, erase, baud)
        log("$ {}\n".format(" ".join('"{}"'.format(a) if " " in a else a for a in argv[1:])))
        code, output = run_esptool(argv, log, cancel, holder)
        if code == 0:
            return True
        if cancel is not None and cancel.is_set():
            raise Cancelled()
        if re.search(r"Writing at|Wrote \d+|Erasing flash", output):
            log("esptool failed after it had started writing; not retrying. Flash again, and erase "
                "first if the board does not boot.\n")
            return False
        log("esptool failed before writing anything (attempt {} of {}).\n".format(attempt, FLASH_ATTEMPTS))
        time.sleep(0.5)
    return False


def wait_for_port(key, timeout=REAPPEAR_TIMEOUT, cancel=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cancel is not None and cancel.is_set():
            return None
        port = find_port(key)
        if port is not None:
            return port
        time.sleep(0.1)
    return None
