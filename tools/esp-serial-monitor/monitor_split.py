"""Telling the board's log output apart from its console output.

This is the heart of the two-pane monitor: one stream of bytes arrives, and
some of it is ESP-IDF log lines, ROM banners and panic dumps, while the rest is
the firmware's own console talking back to whoever is typing. Getting it wrong
is what made the single-pane monitor unreadable, so the rules are written down
next to the code that applies them.

amtx-programmer imports ConsoleSplitter directly: it drives the console
programmatically and has the same problem of log lines arriving mid-reply.
"""

import re
import time



# An ESP-IDF log line: optional colour, level letter, "(ticks)" or
# "(hh:mm:ss.mmm)", then the tag. The bootloader logs the same way.
LOG_LINE = re.compile(r"(?:\x1b\[[0-9;]*m)?([EWIDV]) \([\d:.]+\) ")
COLOURED_LOG_LINE = re.compile(r"\x1b\[[0-9;]*m([EWIDV]) \([\d:.]+\) ")

# Lines the ROM, the panic handler and the reset path print without the log
# format. Matched at the start of a line only.
SYSTEM_PREFIXES = (
    "ESP-ROM:", "Build:", "rst:", "boot:", "SPIWP:", "mode:", "load:", "entry 0x", "configsip:",
    "clk_drv:", "invalid header", "Saved PC:", "ets ", "waiting for download",
    "Guru Meditation", "Core ", "PC ", "PS ", "A0 ", "A4 ", "A8 ", "A12 ", "EXCCAUSE", "EXCVADDR",
    "Backtrace:", "ELF file SHA256", "Rebooting...", "abort()", "assert failed", "***ERROR***",
    "Stack smashing", "CPU halted", "Debug exception reason",
)

ANSI_SEQUENCE = re.compile(r"\x1b\[[0-9;?]*[@-~]")

HOLD_SECONDS = 0.15          # longest a possible start of a system line is held back
SYSTEM_LINE_TIMEOUT = 2.0    # a system line with no newline after this is flushed anyway


def strip_ansi(text):
    return ANSI_SEQUENCE.sub("", text)


def _could_become_log(text, coloured_only=False):
    """True if `text` is an incomplete start of a log line: wait for more bytes."""
    colour = re.match(r"\x1b\[[0-9;]*m", text)
    if colour:
        rest = text[colour.end():]
    elif text.startswith("\x1b"):
        return re.fullmatch(r"\x1b(\[[0-9;]*)?", text) is not None
    elif coloured_only:
        return False
    else:
        rest = text
    return re.fullmatch(r"([EWIDV]( (\([\d:.]*\)?)?)?)?", rest) is not None


def _could_become_system(text):
    return any(p.startswith(text) and p != text for p in SYSTEM_PREFIXES)


class ConsoleSplitter:
    """Splits a board's output into system messages and the interactive console.

    System messages are ESP-IDF log lines (`I (1234) wifi: ...`) and what the ROM
    and panic handler print (`rst:0x1 ...`, `Guru Meditation ...`). Everything
    else is the console: the `amtx>` prompt, command output, echoed keystrokes
    and the escape sequences line editing uses.

    System lines are recognised at the start of a line, or mid-line when they
    carry log colour codes (a log line printed over a half-typed prompt). Text
    that could still turn out to be the start of one is held back for at most
    `HOLD_SECONDS`, so typing echoes without a visible lag.

    `feed()` returns a list of ("sys", line, level) and ("cli", text).
    """

    def __init__(self):
        self.buf = ""
        self.buf_since = None
        self.channel = None        # None at the start of a line, else "sys" or "cli"
        self.sys_line = ""
        self.sys_since = None

    def feed(self, text, now=None):
        now = time.monotonic() if now is None else now
        self.buf += text
        out = []
        self._route(out, now)
        self.buf_since = (self.buf_since or now) if self.buf else None
        return out

    def flush_stale(self, now=None):
        """Release anything held back for too long. Call a few times a second."""
        now = time.monotonic() if now is None else now
        out = []
        if self.buf and self.buf_since is not None and now - self.buf_since > HOLD_SECONDS:
            out.append(("cli", self.buf))
            if self.channel is None:
                self.channel = "cli"
            self.buf, self.buf_since = "", None
        if self.channel == "sys" and self.sys_since is not None and now - self.sys_since > SYSTEM_LINE_TIMEOUT:
            self._end_sys_line(out)
        return out

    def _end_sys_line(self, out):
        line = strip_ansi(self.sys_line).rstrip("\r")
        match = re.match(r"([EWIDV]) \(", line)
        out.append(("sys", line, match.group(1) if match else ""))
        self.sys_line, self.sys_since, self.channel = "", None, None

    def _route(self, out, now):
        while self.buf:
            if self.channel == "sys":
                newline = self.buf.find("\n")
                if newline < 0:
                    self.sys_line += self.buf
                    self.sys_since = self.sys_since or now
                    self.buf = ""
                    return
                self.sys_line += self.buf[:newline]
                self.buf = self.buf[newline + 1:]
                self._end_sys_line(out)
                continue

            if self.channel is None:
                if LOG_LINE.match(self.buf) or self.buf.startswith(SYSTEM_PREFIXES):
                    self.channel = "sys"
                    self.sys_since = now
                    continue
                if _could_become_log(self.buf) or _could_become_system(self.buf):
                    return          # wait for more, or for flush_stale()
                self.channel = "cli"
                continue

            # Console: pass text through up to a newline or a coloured log line.
            cut = self._find_coloured_log(self.buf)
            newline = self.buf.find("\n")
            if newline >= 0 and (cut is None or newline < cut):
                out.append(("cli", self.buf[:newline + 1]))
                self.buf = self.buf[newline + 1:]
                self.channel = None
                continue
            if cut is None:
                out.append(("cli", self.buf))
                self.buf = ""
                return
            if cut > 0:
                out.append(("cli", self.buf[:cut]))
                self.buf = self.buf[cut:]
            if COLOURED_LOG_LINE.match(self.buf):
                self.channel = "sys"
                self.sys_since = now
                continue
            return                  # an escape that might become a log line: wait

    @staticmethod
    def _find_coloured_log(text):
        start = text.find("\x1b")
        while start >= 0:
            tail = text[start:]
            if COLOURED_LOG_LINE.match(tail) or _could_become_log(tail, coloured_only=True):
                return start
            start = text.find("\x1b", start + 1)
        return None
