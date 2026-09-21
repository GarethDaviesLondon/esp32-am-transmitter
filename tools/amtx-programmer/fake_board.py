"""A pretend AM transmitter console, for testing the programmer without a board.

It speaks the design 07 command set (docs/design/07_console_and_access.md §2)
the way ESP-IDF's REPL does, including the parts that make a real console
awkward to drive from a program:

- linenoise redraws the line on every keystroke and asks the terminal for its
  cursor position (`ESC[6n`) before each prompt; in "smart" mode the fake
  refuses to read a command until that question is answered, as the real one
  does.
- ESP-IDF log lines, coloured, arrive between command output lines and in the
  middle of a prompt.
- A failing command prints `error: ...` and then the REPL's own
  "Command returned non-zero error code" line.
- `reboot` takes the port away for a moment, then the board boots again.

It is a model of the contract, not of the firmware. Where the firmware's
wording differs from this file, the firmware is right and this file is stale.
"""

import copy
import json
import re
import shlex
import threading
import time

try:
    from amtx_console import TransportClosed
except ImportError:  # imported from elsewhere; keep the module usable
    class TransportClosed(Exception):
        pass


PROMPT_COLOURED = "\x1b[0;32mamtx> \x1b[0m"
PROMPT_PLAIN = "amtx> "

FACTORY_STATIONS = [
    {"name": "Radio Swiss Jazz", "url": "http://stream.srg-ssr.ch/m/rsj/mp3_128", "gain": 100},
    {"name": "BBC World Service", "url": "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service", "gain": 100},
]

DISCOVER_RESULTS = [
    {"name": "Jazz FM | Classics", "country": "United Kingdom", "kbps": 128, "url": "http://jazz.example/mp3"},
    {"name": "Smooth Jazz Florida", "country": "United States", "kbps": 64, "url": "http://smooth.example/stream"},
]

FADE_NAMES = ["off", "slow", "flutter", "random"]


def log_line(tag, message, level="I", tick=1234):
    colour = {"E": "0;31", "W": "0;33", "I": "0;32"}.get(level, "0")
    return "\x1b[{}m{} ({}) {}: {}\x1b[0m\r\n".format(colour, level, tick, tag, message)


class FakeBoard:
    """The board. Open a transport on it with `open()`; `reboot()` drops it."""

    def __init__(self, smart=True, noisy=True, reboot_seconds=0.3, join_seconds=0.4, log_after_prompt=False):
        self.smart = smart
        self.noisy = noisy
        self.log_after_prompt = log_after_prompt
        self.boots_seen = 0
        self.reboot_seconds = reboot_seconds
        self.join_seconds = join_seconds
        self.lock = threading.Lock()
        self.boots = 0
        self.down_until = 0.0
        self.transport = None
        self.commands = []            # every command line received, for tests
        self.terminal_replies = []    # every ESC[...n / R answer received
        self.nvs = {
            "stations": copy.deepcopy(FACTORY_STATIONS),
            "default": [0, 1],
            "rf": [self._rf_defaults(200000), self._rf_defaults(250000)],
            "wifi": [("HomeNet", "secret")],
            "host": "amtx",
            "web": True,
        }
        self._boot_ram()

    @staticmethod
    def _rf_defaults(hz):
        return {"on": 1, "hz": hz, "depth": 70, "lpf": 4500, "gain": 140, "level": 100,
                "agc": 1, "agct": 25, "fade": 0, "faderate": 200, "fadedepth": 60}

    def _boot_ram(self):
        self.playing = [self.nvs["default"][0], self.nvs["default"][1]]
        self.play_url = [None, None]
        self.tone = [None, None]
        self.last_discover = []
        self.log_on = True
        self.uptime0 = time.monotonic()

    # ---- transport ----

    def open(self):
        with self.lock:
            if time.monotonic() < self.down_until:
                raise TransportClosed("the port is not there (board rebooting)")
            if self.transport is not None and not self.transport.closed:
                raise TransportClosed("port busy")
            self.transport = FakeTransport(self)
            transport = self.transport
        if self.boots_pending():
            self._finish_boot(transport)
        else:
            transport.emit(self._prompt_sequence())
        return transport

    def boots_pending(self):
        return self.boots_seen < self.boots

    def _finish_boot(self, transport):
        self.boots_seen = self.boots
        self._boot_ram()
        transport.emit("ESP-ROM:esp32s3-20210327\r\n")
        transport.emit("rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n")
        transport.emit(log_line("amtx", "ESP32-S3 AM Transmitter", tick=312))
        if self.smart:
            transport.awaiting_probe = True
            transport.emit("\x1b[5n")
        transport.emit("\r\nType 'help' to get the list of commands.\r\n")
        transport.emit(self._prompt_sequence())

    def reboot(self):
        with self.lock:
            self.boots += 1
            self.down_until = time.monotonic() + self.reboot_seconds
            if self.transport is not None:
                self.transport.closed = True

    def _prompt_sequence(self):
        if self.smart:
            prompt = "\x1b[6n\x00\x1b[999C\x00\x1b[6n\x00\x1b[998D" + PROMPT_COLOURED
        else:
            prompt = PROMPT_PLAIN
        if self.log_after_prompt:
            # A heartbeat landing while the prompt sits waiting, as on a real board.
            prompt += log_line("wifi", "wifi up: connected to \"HomeNet\"", tick=30000)
        return prompt

    # ---- command handling ----

    def handle(self, transport, line):
        self.commands.append(line)
        out = []
        say = out.append
        if self.noisy:
            say(log_line("wifi", "wifi up: connected to \"HomeNet\", ip 192.168.1.50, rssi -52 dBm, drops 0"))
        try:
            argv = shlex.split(line, posix=True)
        except ValueError:
            argv = line.split()
        if not argv:
            transport.emit(self._prompt_sequence())
            return
        name, args = argv[0], argv[1:]
        handler = getattr(self, "cmd_" + name, None)
        delayed = []
        if handler is None:
            say("Unrecognized command\r\n")
        else:
            code = handler(args, say, delayed)
            if code:
                say("Command returned non-zero error code: 0x1 (ERROR)\r\n")
        transport.emit("".join(out))
        if delayed:
            when = time.monotonic() + self.join_seconds
            transport.emit("".join(delayed), at=when)
            transport.emit(self._prompt_sequence(), at=when)
        elif name != "reboot":
            transport.emit(self._prompt_sequence())

    @staticmethod
    def _chain(value):
        text = str(value).lower()
        return {"a": 0, "0": 0, "b": 1, "1": 1}.get(text)

    @staticmethod
    def _int(value):
        try:
            return int(value)
        except (TypeError, ValueError):
            return None

    def _error(self, say, reason):
        say("error: {}\r\n".format(reason))
        return 1

    def cmd_help(self, args, say, delayed):
        say("status\r\n  One parseable line of board state\r\n")
        return 0

    def cmd_state(self, args, say, delayed):
        say(json.dumps(self.state_doc(), separators=(",", ":")) + "\r\n")
        if self.noisy:
            say(log_line("stream", "[1] buffer 62%", tick=5000))
        say("ok\r\n")
        return 0

    def state_doc(self):
        stations = self.nvs["stations"]
        chains = []
        for ch in range(2):
            rf = self.nvs["rf"][ch]
            index = self.playing[ch]
            station = stations[index] if index is not None and 0 <= index < len(stations) else None
            name = station["name"] if station else ("Direct URL" if self.play_url[ch] else "")
            url = station["url"] if station else (self.play_url[ch] or "")
            state = "test tone" if self.tone[ch] else ("playing" if url else "stopped")
            chains.append({
                "ch": ch,
                "play": {"index": index if station else -1, "name": name, "url": url, "state": state},
                "rf": {"on": bool(rf["on"]), "hz": rf["hz"], "depth": rf["depth"], "lpf": rf["lpf"],
                       "gain": rf["gain"], "level": rf["level"], "levelnow": rf["level"],
                       "agc": bool(rf["agc"]), "agct": rf["agct"], "fdm": rf["fade"], "fdr": rf["faderate"],
                       "fdd": rf["fadedepth"], "fifo": 1024, "fifocap": 2048, "underruns": 0},
                "audio": {"peak": 0.42, "rate": 44100, "ch": 2, "kbps": 128, "frames": 1000, "errors": 0,
                          "resyncs": 0, "trim": 12, "agcdb": 3.5},
                "stream": {"buffered": 40000, "capacity": 65536, "status": 200, "reconnects": 0, "error": ""},
            })
        return {
            "chains": chains,
            "net": {"state": "connected", "ssid": "HomeNet", "ip": "192.168.1.50", "rssi": -52, "drops": 0},
            "sys": {"heap": 8000000, "heapint": 18800, "heapintmax": 9000,
                    "uptime": int(time.monotonic() - self.uptime0), "version": "v2.1-fake",
                    "host": self.nvs["host"], "web": self.nvs["web"]},
            "stations": [{"name": s["name"], "url": s["url"], "gain": s["gain"],
                          "def": [self.nvs["default"][c] == i for c in range(2)]}
                         for i, s in enumerate(stations)],
        }

    def cmd_play(self, args, say, delayed):
        if len(args) != 2 or self._chain(args[0]) is None:
            say("usage: play <tx> <index|url>\r\n")
            return self._error(say, "ESP_ERR_INVALID_ARG")
        ch = self._chain(args[0])
        if "://" in args[1]:
            self.playing[ch], self.play_url[ch] = None, args[1]
        else:
            index = self._int(args[1])
            if index is None or not 0 <= index < len(self.nvs["stations"]):
                return self._error(say, "ESP_ERR_INVALID_ARG")
            self.playing[ch], self.play_url[ch] = index, None
        self.tone[ch] = None
        say("ok\r\n")
        return 0

    def cmd_stop(self, args, say, delayed):
        ch = self._chain(args[0]) if args else None
        if ch is None:
            return self._error(say, "ESP_ERR_INVALID_ARG")
        self.playing[ch], self.play_url[ch] = None, None
        say("ok\r\n")
        return 0

    def cmd_station(self, args, say, delayed):
        stations = self.nvs["stations"]
        if not args:
            return self._error(say, "usage: station list|add|delete|move|default|trim")
        verb = args[0]
        if verb == "list":
            for i, s in enumerate(stations):
                flags = "".join("AB"[c] if self.nvs["default"][c] == i else "-" for c in range(2))
                say("{:2d}  {}  {:3d}%  {}  {}\r\n".format(i, flags, s["gain"], s["name"], s["url"]))
            say("ok: {} station(s)\r\n".format(len(stations)))
            return 0
        if verb == "add" and len(args) == 3:
            if len(stations) >= 20:
                return self._error(say, "ESP_ERR_NO_MEM")
            stations.append({"name": args[1] or args[2], "url": args[2], "gain": 100})
            say("ok\r\n")
            return 0
        if verb == "delete" and len(args) == 2:
            index = self._int(args[1])
            if index is None or not 0 <= index < len(stations):
                return self._error(say, "ESP_ERR_INVALID_ARG")
            del stations[index]
            self.nvs["default"] = [-1 if d == index else (d - 1 if d > index else d) for d in self.nvs["default"]]
            say("ok\r\n")
            return 0
        if verb == "move" and len(args) == 3:
            source, target = self._int(args[1]), self._int(args[2])
            if source is None or target is None or not (0 <= source < len(stations) and 0 <= target < len(stations)):
                return self._error(say, "ESP_ERR_INVALID_ARG")
            stations.insert(target, stations.pop(source))
            say("ok\r\n")
            return 0
        if verb == "default" and len(args) == 3 and self._chain(args[1]) is not None:
            ch = self._chain(args[1])
            if args[2] == "none":
                self.nvs["default"][ch] = -1
            else:
                index = self._int(args[2])
                if index is None or not 0 <= index < len(stations):
                    return self._error(say, "ESP_ERR_INVALID_ARG")
                self.nvs["default"][ch] = index
            say("ok\r\n")
            return 0
        if verb == "trim" and len(args) == 3:
            index, percent = self._int(args[1]), self._int(args[2])
            if index is None or percent is None or not 0 <= index < len(stations) or not 10 <= percent <= 400:
                return self._error(say, "ESP_ERR_INVALID_ARG")
            stations[index]["gain"] = percent
            say("ok\r\n")
            return 0
        return self._error(say, "ESP_ERR_INVALID_ARG")

    def cmd_rf(self, args, say, delayed):
        ch = self._chain(args[0]) if args else None
        if ch is None:
            return self._error(say, "ESP_ERR_INVALID_ARG")
        rf = self.nvs["rf"][ch]
        pairs = args[1:]
        if not pairs:
            say(" ".join("{}={}".format(k, rf[k]) for k in rf) + "\r\n")
            say("ok\r\n")
            return 0
        if len(pairs) % 2:
            return self._error(say, "every key needs a value")
        updated = dict(rf)
        for key, value in zip(pairs[::2], pairs[1::2]):
            if key not in rf:
                return self._error(say, "unknown key {}".format(key))
            if key in ("on", "agc"):
                if value not in ("on", "off", "1", "0"):
                    return self._error(say, "{} is on or off".format(key))
                updated[key] = 1 if value in ("on", "1") else 0
            elif key == "fade":
                if value in FADE_NAMES:
                    updated[key] = FADE_NAMES.index(value)
                elif value in ("0", "1", "2", "3"):
                    updated[key] = int(value)
                else:
                    return self._error(say, "fade is off, slow, flutter or random")
            else:
                number = self._int(value)
                if number is None:
                    return self._error(say, "{} needs a number".format(key))
                updated[key] = number
        if not 5 <= updated["depth"] <= 90:
            return self._error(say, "ESP_ERR_INVALID_ARG")
        self.nvs["rf"][ch] = updated
        say(" ".join("{}={}".format(k, updated[k]) for k in updated) + "\r\n")
        say("ok\r\n")
        return 0

    def cmd_tone(self, args, say, delayed):
        ch = self._chain(args[0]) if args else None
        if ch is None or len(args) < 2 or args[1] not in ("on", "off"):
            return self._error(say, "ESP_ERR_INVALID_ARG")
        self.tone[ch] = (self._int(args[2]) if len(args) > 2 else 1000) if args[1] == "on" else None
        say("ok\r\n")
        return 0

    def cmd_discover(self, args, say, delayed):
        if len(args) >= 3 and args[0] == "play":
            ch, n = self._chain(args[1]), self._int(args[2])
            if ch is None or n is None or not 0 <= n < len(self.last_discover):
                return self._error(say, "no such result; search first")
            self.playing[ch], self.play_url[ch] = None, self.last_discover[n]["url"]
            say("ok\r\n")
            return 0
        if len(args) == 2 and args[0] == "save":
            n = self._int(args[1])
            if n is None or not 0 <= n < len(self.last_discover):
                return self._error(say, "no such result; search first")
            hit = self.last_discover[n]
            self.nvs["stations"].append({"name": hit["name"], "url": hit["url"], "gain": 100})
            say("ok\r\n")
            return 0
        if len(args) >= 2 and args[0] in ("name", "tag", "country"):
            say("searching Radio-Browser by {} for \"{}\" ...\r\n".format(args[0], " ".join(args[1:])))
            self.last_discover = list(DISCOVER_RESULTS)
            for i, hit in enumerate(self.last_discover):
                say("{:2d}  {} | {} | {} kbit/s | {}\r\n".format(i, hit["name"], hit["country"], hit["kbps"], hit["url"]))
            say("ok: {} found\r\n".format(len(self.last_discover)))
            return 0
        return self._error(say, "usage: discover <name|tag|country> <query>")

    def cmd_hostname(self, args, say, delayed):
        if not args:
            say("hostname={}\r\n".format(self.nvs["host"]))
            say("ok\r\n")
            return 0
        name = args[0].lower()
        if len(name) > 31 or not re.match(r"^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$", name):
            return self._error(say, "ESP_ERR_INVALID_ARG")
        self.nvs["host"] = name
        say("ok: reachable as {}.local now; the router's list updates after a reboot\r\n".format(name))
        return 0

    def cmd_web(self, args, say, delayed):
        if not args:
            say("web={}\r\n".format("on" if self.nvs["web"] else "off"))
            say("ok\r\n")
            return 0
        if args[0] not in ("on", "off"):
            return self._error(say, "usage: web on|off")
        self.nvs["web"] = args[0] == "on"
        say("ok: web interface {}\r\n".format(args[0]))
        return 0

    # ---- the commands that predate design 07: no ok/error line ----

    def cmd_status(self, args, say, delayed):
        say("state=connected ssid=\"HomeNet\" ip=192.168.1.50 rssi=-52 drops=0 saved=1 host={} web={}\r\n".format(
            self.nvs["host"], "on" if self.nvs["web"] else "off"))
        return 0

    def cmd_wifi(self, args, say, delayed):
        # As main/cli/console.c prints it: each subcommand ends with ok or error:.
        verb = args[0] if args else ""
        if verb == "status" and len(args) == 1:
            self.cmd_status([], say, delayed)
            say("ok\r\n")
            return 0
        if verb == "scan" and len(args) == 1:
            say(" 0  HomeNet                            -52 dBm  secured\r\n")
            say(" 1  Cafe Guest                         -71 dBm  open\r\n")
            say("ok: 2 network(s) found\r\n")
            return 0
        if verb == "list" and len(args) == 1:
            for i, (ssid, _) in enumerate(self.nvs["wifi"]):
                say("{:2d}  {}\r\n".format(i, ssid))
            say("ok: {} saved\r\n".format(len(self.nvs["wifi"])))
            return 0
        if verb == "forget" and len(args) == 1:
            count = len(self.nvs["wifi"])
            self.nvs["wifi"] = []
            say("ok: deleted {} credential(s)\r\n".format(count))
            return 0
        if verb in ("join", "save") and len(args) in (2, 3):
            ssid, password = args[1], args[2] if len(args) > 2 else ""
            if verb == "save":
                self.nvs["wifi"].insert(0, (ssid, password))
                say("ok: saved {} ({} stored), tried at the next boot\r\n".format(
                    ssid, len(self.nvs["wifi"])))
                return 0
            say("joining \"{}\" ...\r\n".format(ssid))
            if password == "wrong":
                delayed.append("run 'reboot' to bring the AMTX-Setup portal back\r\n")
                delayed.append("error: join failed: reason 15, handshake timed out: wrong password\r\n")
                return 0
            self.nvs["wifi"].insert(0, (ssid, password))
            delayed.append("joined \"{}\", ip 192.168.1.51, rssi -60 dBm\r\n".format(ssid))
            delayed.append("ok: joined {}, ip 192.168.1.51, saved ({} stored)\r\n".format(
                ssid, len(self.nvs["wifi"])))
            return 0
        say("error: usage: wifi status | scan | join <ssid> [pass] | save <ssid> [pass] | list | forget\r\n")
        return 0

    def cmd_log(self, args, say, delayed):
        self.log_on = not args or args[0] != "off"
        say("log {}\r\n".format("on" if self.log_on else "off"))
        return 0

    def cmd_reboot(self, args, say, delayed):
        say("rebooting\r\n")
        return 0


class FakeTransport:
    """The byte stream to and from a FakeBoard, shaped like SerialTransport."""

    def __init__(self, board):
        self.board = board
        self.closed = False
        self.pending = []             # (release time, bytes)
        self.inbuf = ""
        self.edit = ""
        self.awaiting_probe = False
        self.awaiting_cursor = 0
        self.lock = threading.Lock()

    def emit(self, text, at=None):
        if not text:
            return
        data = text.encode("utf-8")
        with self.lock:
            self.pending.append((at or time.monotonic(), data))
            # A prompt sequence asks for the cursor twice before reading input.
            if self.board.smart and "\x1b[6n" in text:
                self.awaiting_cursor += text.count("\x1b[6n")

    def read(self, timeout=0.05):
        deadline = time.monotonic() + timeout
        while True:
            now = time.monotonic()
            with self.lock:
                ready = [p for p in self.pending if p[0] <= now]
                self.pending = [p for p in self.pending if p[0] > now]
            if ready:
                return b"".join(data for _, data in ready)
            if self.closed:
                raise TransportClosed("device disconnected")
            if now >= deadline:
                return b""
            time.sleep(0.005)

    def write(self, data):
        if self.closed:
            raise TransportClosed("device disconnected")
        text = data.decode("utf-8", errors="replace")
        with self.lock:
            self.inbuf += text
        self._process()

    def _process(self):
        while True:
            with self.lock:
                buf = self.inbuf
            if not buf:
                return
            # Terminal answers. Until the cursor queries are answered, a smart
            # console reads nothing else, which is exactly the real hazard.
            match = re.match(r"\x1b\[(\d*;?\d*)([nR])", buf)
            if match:
                self.board.terminal_replies.append(match.group(0))
                with self.lock:
                    self.inbuf = buf[match.end():]
                    if match.group(2) == "n":
                        self.awaiting_probe = False
                    elif self.awaiting_cursor:
                        self.awaiting_cursor -= 1
                continue
            if buf.startswith("\x1b"):
                if len(buf) < 8:
                    return        # wait for the rest of the answer
            if self.board.smart and (self.awaiting_cursor or self.awaiting_probe):
                # The real getCursorPosition() would swallow these bytes as its
                # answer. Model that: consume up to an 'R' and lose them.
                with self.lock:
                    if self.pending and any(b"\x1b[6n" in d for _, d in self.pending):
                        return    # the question has not been "sent" yet; hold input
                    cut = buf.find("R")
                    self.inbuf = buf[cut + 1:] if cut >= 0 else ""
                    self.awaiting_cursor = max(0, self.awaiting_cursor - 1)
                    self.board.commands.append("<swallowed by cursor query: {!r}>".format(buf[:cut + 1] if cut >= 0 else buf))
                continue
            ch = buf[0]
            with self.lock:
                self.inbuf = buf[1:]
            if ch in "\r\n":
                line, self.edit = self.edit, ""
                self.emit("\r\n")
                if self.closed:
                    return
                self.board.handle(self, line)
                if line.strip().split()[:1] == ["reboot"]:
                    # Let "rebooting" leave before the port drops.
                    threading.Timer(0.1, self.board.reboot).start()
                continue
            self.edit += ch
            if self.board.smart:
                self.emit("\r" + PROMPT_COLOURED + self.edit + "\x1b[0K\r\x1b[{}C".format(len(self.edit) + 6))
            else:
                self.emit(ch)

    def close(self):
        self.closed = True
