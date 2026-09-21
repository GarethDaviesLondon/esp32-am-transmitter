"""The amtx> console as a request and reply protocol, with no GUI in it.

The board's serial console was written for a person at a terminal. This module
lets a program use it: send one command, wait for its reply, and hand back the
reply's lines with log output taken out. The contract is
docs/design/07_console_and_access.md §2.

How a reply ends:

- Commands added by design 07 print exactly one final line starting `ok` or
  `error:`. That line ends the reply.
- `wifi` prints one too, since the firmware gave it reply lines.
- `status`, `log` and `reboot` print no such line. Their reply ends when the REPL prints its prompt again, which it
  does before reading every line. If the prompt never comes back (it was
  swallowed, or the firmware has changed), an optional quiet period ends it,
  and failing that the timeout does.
- `reboot` ends when the board says "rebooting" or the port drops.

Two things about ESP-IDF's console this layer has to deal with:

- linenoise asks the terminal where its cursor is (`ESC[6n`) before every line,
  and waits for the answer with no timeout. Anything sent before that answer is
  read as the answer. So the reader answers `ESC[5n` and `ESC[6n` itself, the
  same way the monitor's TerminalView does.
- Log lines from other tasks share the port. They are separated out with
  esp-serial-monitor's ConsoleSplitter, imported rather than copied.

The transport is anything with read(timeout) -> bytes, write(bytes) and close().
SerialTransport wraps pyserial; tests use fake_board.FakeTransport.
"""

import codecs
import json
import re
import sys
import threading
import time
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOL_DIR.parent / "esp-serial-monitor"))
import esp_serial_monitor as monitor  # noqa: E402


PROMPT = "amtx>"
MAX_COMMAND_CHARS = 319          # the console's line is 320 bytes, terminator included
DEFAULT_TIMEOUT = 5.0
SLOW_TIMEOUT = 25.0              # discover and wifi join: an HTTPS round trip, a 15 s join
TRAILER_GRACE = 0.5              # after ok/error, time allowed for the prompt to come back

OK_LINE = re.compile(r"^ok(?::\s*(.*))?$")
ERROR_LINE = re.compile(r"^error:\s*(.*)$")
TERMINAL_QUERIES = ("\x1b[999C", "\x1b[5n", "\x1b[6n")
TERMINAL_QUERY = re.compile(r"\x1b\[(999C|5n|6n)")
TERMINAL_COLUMNS = 999

REPL_ERROR_LINES = (
    "Unrecognized command",
    "Command returned non-zero error code",
    "Internal error:",
)

RF_KEYS = ["on", "hz", "depth", "lpf", "gain", "level", "agc", "agct", "fade", "faderate", "fadedepth"]
FADE_NAMES = ["off", "slow", "flutter", "random"]
DISCOVER_BY = ["name", "tag", "country"]
HOSTNAME_MAX = 31
HOSTNAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$")


class TransportClosed(Exception):
    """The port went away: the board reset, or was unplugged."""


class NotConnected(Exception):
    """A command was asked for with no board attached."""


# =========================
# TRANSPORT
# =========================

class SerialTransport:
    """A pyserial port, opened the way the monitor opens one (DTR and RTS low)."""

    def __init__(self, device, baud=monitor.DEFAULT_BAUD):
        self.device = device
        self.serial = monitor.open_port(device, baud, timeout=0.05)

    def read(self, timeout=0.05):
        try:
            self.serial.timeout = timeout
            waiting = self.serial.in_waiting
            return self.serial.read(waiting if waiting else 1)
        except Exception as exc:
            raise TransportClosed(str(exc))

    def write(self, data):
        try:
            self.serial.write(data)
        except Exception as exc:
            raise TransportClosed(str(exc))

    def close(self):
        try:
            self.serial.close()
        except Exception:
            pass


# =========================
# TEXT HELPERS
# =========================

def render_line(raw):
    """What a terminal would show for one line of console output.

    linenoise redraws the line being typed by returning to column 0 (`\\r`) and
    printing the prompt and buffer again, and a CRLF line ending leaves a `\\r`
    at the end. The last non-empty stretch between carriage returns is what
    the line finally reads.
    """
    text = monitor.strip_ansi(raw).replace("\x00", "")
    segments = [s for s in text.split("\r") if s != ""]
    return segments[-1].rstrip() if segments else ""


def quote_arg(value):
    """Quote one argument the way esp_console_split_argv() reads it."""
    value = str(value)
    if value and not re.search(r'[\s"\\]', value):
        return value
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_command(*parts):
    return " ".join(quote_arg(p) for p in parts)


def tx_arg(tx):
    """A transmitter as the console names it: 0/'a' -> 'a', 1/'b' -> 'b'."""
    text = str(tx).strip().lower()
    if text in ("0", "a"):
        return "a"
    if text in ("1", "b"):
        return "b"
    raise ValueError("a transmitter is A or B, not {!r}".format(tx))


def parse_kv(line):
    """`key=value key="quoted value" ...` into a dict of strings."""
    out = {}
    for match in re.finditer(r'(\w+)=("(?:[^"\\]|\\.)*"|\S*)', line):
        value = match.group(2)
        if value.startswith('"') and value.endswith('"') and len(value) >= 2:
            value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        out[match.group(1)] = value
    return out


def validate_hostname(name):
    """(normalised name, None) or (None, reason), per design 07 §3."""
    name = (name or "").strip().lower()
    if not name:
        return None, "a hostname cannot be empty"
    if len(name) > HOSTNAME_MAX:
        return None, "a hostname is at most {} characters".format(HOSTNAME_MAX)
    if not HOSTNAME_RE.match(name):
        return None, "letters, digits and hyphens only, not starting or ending with a hyphen"
    return name, None


def parse_discover_lines(lines):
    """Result lines `<n>  <name> | <country> | <kbps> kbit/s | <url>`.

    Split from the right, so a station name containing " | " survives.
    """
    results = []
    for line in lines:
        match = re.match(r"^\s*(\d+)\s+(.*)$", line)
        if not match:
            continue
        parts = match.group(2).rsplit(" | ", 3)
        if len(parts) != 4:
            continue
        name, country, rate, url = parts
        kbps = re.match(r"^\s*(\d+)", rate)
        results.append({"n": int(match.group(1)), "name": name.strip(), "country": country.strip(),
                        "kbps": int(kbps.group(1)) if kbps else 0, "url": url.strip()})
    return results


def parse_wifi_scan(lines):
    results = []
    for line in lines:
        match = re.match(r"^\s*(\d+)\s+(.+?)\s+(-?\d+) dBm\s+(secured|open)\s*$", line)
        if match:
            results.append({"ssid": match.group(2), "rssi": int(match.group(3)),
                            "secure": match.group(4) == "secured"})
    return results


def parse_wifi_list(lines):
    results = []
    for line in lines:
        match = re.match(r"^\s*(\d+)\s{2}(.+)$", line)
        if match:
            results.append(match.group(2))
    return results


# =========================
# REPLY
# =========================

class Reply:
    """One command's outcome.

    `ok` is True, False, or None when the command has no ok/error convention
    and nothing in its output said it failed. `complete` is False when the
    reply was cut off by the timeout rather than ended by the board.
    """

    def __init__(self, command):
        self.command = command
        self.lines = []
        self.ok = None
        self.detail = ""
        self.error = ""
        self.complete = False
        self.disconnected = False

    @property
    def failed(self):
        return self.ok is False

    def summary(self):
        if self.disconnected and not self.complete:
            return "{}: the board disconnected".format(self.command)
        if not self.complete:
            return "{}: no reply within the timeout".format(self.command)
        if self.ok is False:
            return "{}: error: {}".format(self.command, self.error or "failed")
        return "{}: ok{}".format(self.command, ": " + self.detail if self.detail else "")

    def __repr__(self):
        return "Reply({!r}, ok={}, complete={}, lines={})".format(
            self.command, self.ok, self.complete, len(self.lines))


# =========================
# CONSOLE
# =========================

class AmtxConsole:
    """One board's console. Thread-safe; one command runs at a time.

    `on_event(kind, text)` is called from the reader thread with kind "log"
    (a system line), "cli" (a finished console line), "sent", or "disconnected".
    """

    def __init__(self, on_event=None, prompt=PROMPT):
        self.on_event = on_event or (lambda kind, text: None)
        self.prompt = prompt
        self._transport = None
        self._reader_thread = None
        self._stop = threading.Event()
        self._cond = threading.Condition()
        self._command_lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._lines = []
        self._line_base = 0          # index of self._lines[0] in the whole stream
        self._partial = ""
        self._partial_prompt_counted = False
        self._prompt_count = 0
        self._connected = False

    # ---- attach and detach ----

    @property
    def connected(self):
        return self._connected

    def attach(self, transport):
        self.detach()
        self._transport = transport
        self._stop.clear()
        with self._cond:
            self._partial = ""
            self._partial_prompt_counted = False
            self._connected = True
        self._reader_thread = threading.Thread(target=self._reader, args=(transport,), daemon=True)
        self._reader_thread.start()

    def detach(self):
        transport = self._transport
        if transport is None:
            return
        self._stop.set()
        transport.close()
        if self._reader_thread is not None and self._reader_thread is not threading.current_thread():
            self._reader_thread.join(timeout=1.0)
        self._transport = None
        with self._cond:
            self._connected = False
            self._cond.notify_all()

    # ---- reading ----

    def _reader(self, transport):
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        splitter = monitor.ConsoleSplitter()
        tail = ""
        after_far_right = False
        while not self._stop.is_set():
            try:
                data = transport.read(0.05)
            except TransportClosed:
                break
            now = time.monotonic()
            text = decoder.decode(data) if data else ""
            if text:
                # Answer the terminal queries first, before anything can queue
                # behind them. `tail` catches a query split across two reads.
                scan = tail + text
                for match in TERMINAL_QUERY.finditer(scan):
                    code = match.group(1)
                    if code == "999C":
                        after_far_right = True
                    elif code == "5n":
                        self._raw_write(transport, b"\x1b[0n")
                    else:
                        # getColumns() asks twice: where the cursor is, then
                        # where it lands after ESC[999C. A wide answer keeps
                        # linenoise from scrolling a long command line.
                        column = TERMINAL_COLUMNS if after_far_right else 1
                        after_far_right = False
                        self._raw_write(transport, "\x1b[1;{}R".format(column).encode("ascii"))
                last = scan.rfind("\x1b")
                suffix = scan[last:] if last >= 0 else ""
                tail = suffix if any(q.startswith(suffix) and q != suffix for q in TERMINAL_QUERIES) else ""
                pieces = splitter.feed(text, now)
            else:
                pieces = []
            pieces += splitter.flush_stale(now)
            for piece in pieces:
                if piece[0] == "sys":
                    self.on_event("log", piece[1])
                else:
                    self._take_cli(piece[1])
        with self._cond:
            self._connected = False
            self._cond.notify_all()
        if not self._stop.is_set():
            self.on_event("disconnected", "")

    def _raw_write(self, transport, data):
        try:
            with self._write_lock:
                transport.write(data)
        except TransportClosed:
            pass

    def _take_cli(self, text):
        with self._cond:
            self._partial += text
            while "\n" in self._partial:
                raw, self._partial = self._partial.split("\n", 1)
                self._partial_prompt_counted = False
                line = render_line(raw)
                # Each line keeps the prompt count at the moment it ended, so a
                # reply can tell a prompt that came after it from one before.
                self._lines.append((line, self._prompt_count))
                if len(self._lines) > 2000:
                    drop = len(self._lines) - 1000
                    del self._lines[:drop]
                    self._line_base += drop
                self.on_event("cli", line)
            if not self._partial_prompt_counted and render_line(self._partial) == self.prompt:
                self._partial_prompt_counted = True
                self._prompt_count += 1
            self._cond.notify_all()

    def sync(self, timeout=1.5):
        """Get a fresh prompt: send an empty line and wait for the prompt to come back.

        An empty line runs nothing, and it clears a half-typed line someone
        left in the console. Returns True if the prompt came back.
        """
        with self._command_lock:
            if not self._connected or self._transport is None:
                raise NotConnected("no board connected")
            with self._cond:
                before = self._prompt_count
            try:
                with self._write_lock:
                    self._transport.write(b"\r")
            except TransportClosed:
                return False
            deadline = time.monotonic() + timeout
            with self._cond:
                while self._prompt_count <= before and self._connected:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return False
                    self._cond.wait(timeout=min(remaining, 0.05))
                return self._prompt_count > before

    # ---- commands ----

    def command(self, text, timeout=DEFAULT_TIMEOUT, legacy=False, quiet=None, expect_reboot=False):
        """Send one command line and wait for its reply.

        `legacy` commands have no ok/error line; they end when the prompt
        returns, or after `quiet` seconds without a new line once output has
        started. `expect_reboot` ends the reply at "rebooting" or a disconnect.
        """
        if len(text) > MAX_COMMAND_CHARS:
            raise ValueError("the command is {} characters; the console takes {}".format(
                len(text), MAX_COMMAND_CHARS))
        if "\r" in text or "\n" in text:
            raise ValueError("a command is one line")
        with self._command_lock:
            if not self._connected or self._transport is None:
                raise NotConnected("no board connected")
            reply = Reply(text)
            with self._cond:
                start = self._line_base + len(self._lines)
            try:
                with self._write_lock:
                    self._transport.write(text.encode("utf-8") + b"\r")
            except TransportClosed:
                reply.disconnected = True
                reply.ok = False
                reply.error = "the board disconnected"
                return reply
            self.on_event("sent", text)
            self._wait(reply, start, timeout, legacy, quiet, expect_reboot)
            return reply

    def _wait(self, reply, start, timeout, legacy, quiet, expect_reboot):
        deadline = time.monotonic() + timeout
        seen = start
        echo_pending = True
        # The prompt that ends this reply is the first one after the echo. One
        # counted before it belongs to the previous command, if that command's
        # prompt arrived late.
        prompts_at_echo = None
        last_line_at = None
        finished_at = None
        with self._cond:
            while True:
                first = seen - self._line_base
                for line, prompts_then in self._lines[max(first, 0):]:
                    seen += 1
                    if echo_pending and (line.startswith(self.prompt) or line == reply.command):
                        # The board's echo of what was typed, with or without
                        # the prompt in front of it.
                        echo_pending = False
                        prompts_at_echo = prompts_then
                        continue
                    if not line.strip():
                        continue
                    if echo_pending:
                        echo_pending = False
                        prompts_at_echo = prompts_then
                    last_line_at = time.monotonic()
                    if finished_at is not None:
                        continue      # a trailer such as "Command returned non-zero error code"
                    reply.lines.append(line)
                    if not legacy:
                        ok = OK_LINE.match(line)
                        err = ERROR_LINE.match(line)
                        if ok or err:
                            reply.lines.pop()
                            reply.ok = bool(ok)
                            reply.detail = (ok.group(1) or "") if ok else ""
                            reply.error = err.group(1) if err else ""
                            reply.complete = True
                            finished_at = time.monotonic()
                            continue
                    if line.startswith(REPL_ERROR_LINES):
                        reply.ok = False
                        reply.error = reply.error or line
                        if line.startswith("Unrecognized command"):
                            reply.error = "unrecognised command (is the firmware older than design 07?)"
                    if expect_reboot and line.strip().lower().startswith("rebooting"):
                        reply.ok = True
                        reply.complete = True
                        return
                if prompts_at_echo is not None and self._prompt_count > prompts_at_echo:
                    if finished_at is None:
                        reply.complete = True
                        if reply.ok is None and not legacy:
                            # A new-style command that never printed ok/error.
                            reply.ok = False
                            reply.error = reply.error or "no ok/error line before the prompt returned"
                    return
                if not self._connected:
                    reply.disconnected = True
                    if expect_reboot:
                        reply.ok = True
                        reply.complete = True
                    elif finished_at is None:
                        reply.ok = False
                        reply.error = "the board disconnected"
                    return
                now = time.monotonic()
                if finished_at is not None and now - finished_at >= TRAILER_GRACE:
                    return
                if legacy and quiet is not None and last_line_at is not None and now - last_line_at >= quiet:
                    reply.complete = True
                    return
                if now >= deadline:
                    if finished_at is None:
                        reply.ok = False if reply.ok is None else reply.ok
                        reply.error = reply.error or "no reply within {:.0f} s".format(timeout)
                    return
                self._cond.wait(timeout=0.05)


def attach_when_ready(console, open_transport, timeout=15.0, poll=0.1, cancel=None):
    """Keep trying to open the port until the board is back, then attach and get a prompt.

    A board that has just reset is off the native USB bus for a moment, and
    opening its port fails until it returns. Returns True once attached with a
    fresh prompt, False on timeout or cancel.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cancel is not None and cancel.is_set():
            return False
        try:
            transport = open_transport()
        except Exception:
            time.sleep(poll)
            continue
        console.attach(transport)
        # The boot banner and the first prompt can take a moment after the port
        # appears; keep asking for a prompt until the deadline.
        while time.monotonic() < deadline and console.connected:
            if cancel is not None and cancel.is_set():
                console.detach()
                return False
            try:
                if console.sync(timeout=min(1.0, max(0.1, deadline - time.monotonic()))):
                    return True
            except NotConnected:
                break
        console.detach()
        time.sleep(poll)
    return False


# =========================
# THE BOARD, AS A SET OF OPERATIONS
# =========================

class AmtxBoard:
    """Design 07's commands as methods. Each returns a Reply, plus parsed data where there is any.

    `on_reply(reply)` is called after every command, so a front end can show
    every outcome without each caller remembering to.
    """

    def __init__(self, console, on_reply=None):
        self.console = console
        self.on_reply = on_reply or (lambda reply: None)

    def run(self, *parts, timeout=DEFAULT_TIMEOUT, legacy=False, quiet=None, expect_reboot=False):
        reply = self.console.command(build_command(*parts), timeout=timeout, legacy=legacy,
                                     quiet=quiet, expect_reboot=expect_reboot)
        self.on_reply(reply)
        return reply

    # ---- state ----

    def state(self):
        reply = self.run("state")
        doc = None
        for line in reversed(reply.lines):
            if line.startswith("{"):
                try:
                    doc = json.loads(line)
                    break
                except ValueError:
                    continue
        if doc is None and reply.ok is not False:
            reply.ok = False
            reply.error = "no JSON document in the reply"
        return reply, doc

    # ---- playing ----

    def play(self, tx, station):
        """`station` is a saved-station index or a URL."""
        return self.run("play", tx_arg(tx), station)

    def stop(self, tx):
        return self.run("stop", tx_arg(tx))

    # ---- stations ----

    def station_list(self):
        return self.run("station", "list")

    def station_add(self, name, url):
        return self.run("station", "add", name, url)

    def station_delete(self, index):
        return self.run("station", "delete", int(index))

    def station_move(self, source, target):
        return self.run("station", "move", int(source), int(target))

    def station_default(self, tx, index):
        return self.run("station", "default", tx_arg(tx), "none" if index is None else int(index))

    def station_trim(self, index, percent):
        return self.run("station", "trim", int(index), int(percent))

    # ---- transmitter ----

    def rf_get(self, tx):
        reply = self.run("rf", tx_arg(tx))
        values = {}
        for line in reply.lines:
            parsed = parse_kv(line)
            if "hz" in parsed:
                values = parsed
        return reply, values

    def rf_set(self, tx, settings):
        parts = ["rf", tx_arg(tx)]
        for key in RF_KEYS:
            if key in settings and settings[key] is not None:
                parts += [key, settings[key]]
        unknown = set(settings) - set(RF_KEYS)
        if unknown:
            raise ValueError("unknown rf key(s): {}".format(", ".join(sorted(unknown))))
        if len(parts) == 2:
            raise ValueError("nothing to change")
        return self.run(*parts)

    def tone(self, tx, on, hz=None):
        if on:
            return self.run("tone", tx_arg(tx), "on", *([int(hz)] if hz else []))
        return self.run("tone", tx_arg(tx), "off")

    # ---- discover ----

    def discover(self, by, query):
        if by not in DISCOVER_BY:
            raise ValueError("search by name, tag or country")
        words = str(query).split()
        if not words:
            raise ValueError("nothing to search for")
        reply = self.run("discover", by, *words, timeout=SLOW_TIMEOUT)
        return reply, parse_discover_lines(reply.lines)

    def discover_play(self, tx, n):
        return self.run("discover", "play", tx_arg(tx), int(n))

    def discover_save(self, n):
        return self.run("discover", "save", int(n))

    # ---- network ----
    # Every wifi command ends with ok or error:, so success is read from the
    # board rather than guessed from its wording. A join can take 15 s.

    def wifi_status(self):
        return self.run("wifi", "status")

    def wifi_scan(self):
        reply = self.run("wifi", "scan", timeout=SLOW_TIMEOUT)
        return reply, parse_wifi_scan(reply.lines)

    def wifi_list(self):
        reply = self.run("wifi", "list")
        return reply, parse_wifi_list(reply.lines)

    def wifi_join(self, ssid, password):
        return self.run("wifi", "join", ssid, *([password] if password else []),
                        timeout=SLOW_TIMEOUT)

    def wifi_save(self, ssid, password):
        return self.run("wifi", "save", ssid, *([password] if password else []))

    def wifi_forget(self):
        return self.run("wifi", "forget")

    def log(self, on):
        return self.run("log", "on" if on else "off", legacy=True, quiet=1.0)

    def reboot(self):
        return self.run("reboot", legacy=True, timeout=DEFAULT_TIMEOUT, expect_reboot=True)

    # ---- access ----

    def hostname_get(self):
        reply = self.run("hostname")
        name = None
        for line in reply.lines:
            parsed = parse_kv(line)
            if "hostname" in parsed:
                name = parsed["hostname"]
        return reply, name

    def hostname_set(self, name):
        normalised, problem = validate_hostname(name)
        if problem:
            raise ValueError(problem)
        return self.run("hostname", normalised)

    def web_get(self):
        reply = self.run("web")
        state = None
        for line in reply.lines:
            parsed = parse_kv(line)
            if "web" in parsed:
                state = parsed["web"] == "on"
        return reply, state

    def web_set(self, on):
        return self.run("web", "on" if on else "off")
