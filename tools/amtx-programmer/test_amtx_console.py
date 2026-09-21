"""Tests for the console protocol layer, against fake_board.FakeBoard.

Run with:  python -m pytest tools/amtx-programmer
"""

import threading
import time

import pytest

import amtx_console as ac
from fake_board import FakeBoard


class Recorder:
    def __init__(self):
        self.events = []
        self.lock = threading.Lock()

    def __call__(self, kind, text):
        with self.lock:
            self.events.append((kind, text))

    def of(self, kind):
        with self.lock:
            return [text for k, text in self.events if k == kind]


def connect(board, recorder=None):
    console = ac.AmtxConsole(on_event=recorder)
    assert ac.attach_when_ready(console, board.open, timeout=3.0)
    return console, ac.AmtxBoard(console)


@pytest.fixture(params=["smart", "dumb"])
def rig(request):
    board = FakeBoard(smart=request.param == "smart", noisy=True)
    recorder = Recorder()
    console, amtx = connect(board, recorder)
    yield board, console, amtx, recorder
    console.detach()


# ---- text helpers ----

def test_render_line_keeps_what_the_last_redraw_shows():
    raw = "\ramtx> st\x1b[0K\r\x1b[8C\r\x1b[0;32mamtx> \x1b[0mstate\x1b[0K\r"
    assert ac.render_line(raw) == "amtx> state"
    assert ac.render_line("ok\r") == "ok"
    assert ac.render_line("\x1b[6n\x00\x1b[999C\x00\x1b[0;32mamtx> \x1b[0m") == "amtx>"


def test_arguments_are_quoted_the_way_split_argv_reads_them():
    assert ac.build_command("station", "add", "Radio 4", "http://x/y") == 'station add "Radio 4" http://x/y'
    assert ac.quote_arg('say "hi"') == '"say \\"hi\\""'
    assert ac.quote_arg("back\\slash") == '"back\\\\slash"'
    assert ac.quote_arg("") == '""'


def test_transmitter_names():
    assert ac.tx_arg(0) == "a" and ac.tx_arg("B") == "b" and ac.tx_arg("1") == "b"
    with pytest.raises(ValueError):
        ac.tx_arg("c")


def test_key_value_lines():
    assert ac.parse_kv('on=1 hz=200000 name="two words"') == {"on": "1", "hz": "200000", "name": "two words"}


def test_hostname_rules_follow_design_07():
    assert ac.validate_hostname("AMTX-Kitchen") == ("amtx-kitchen", None)
    assert ac.validate_hostname("a")[0] == "a"
    for bad in ("", "-amtx", "amtx-", "am tx", "am_tx", "x" * 32):
        assert ac.validate_hostname(bad)[0] is None, bad
    assert ac.validate_hostname("x" * 31)[0] == "x" * 31


def test_discover_lines_split_from_the_right():
    hits = ac.parse_discover_lines([" 0  Jazz FM | Classics | United Kingdom | 128 kbit/s | http://j/mp3",
                                    "not a result"])
    assert hits == [{"n": 0, "name": "Jazz FM | Classics", "country": "United Kingdom",
                     "kbps": 128, "url": "http://j/mp3"}]


def test_wifi_scan_lines():
    nets = ac.parse_wifi_scan([" 1  Cafe Guest                         -71 dBm  open"])
    assert nets == [{"ssid": "Cafe Guest", "rssi": -71, "secure": False}]


# ---- commands against the fake board, smart and dumb terminals ----

def test_state_parses_and_log_lines_stay_out_of_the_reply(rig):
    board, console, amtx, recorder = rig
    reply, doc = amtx.state()
    assert reply.ok is True and reply.complete, reply.summary()
    assert doc["sys"]["host"] == "amtx" and doc["sys"]["web"] is True
    assert [s["name"] for s in doc["stations"]] == ["Radio Swiss Jazz", "BBC World Service"]
    assert doc["stations"][1]["def"] == [False, True]
    assert all("wifi up" not in line for line in reply.lines)
    assert any("wifi up" in line for line in recorder.of("log"))
    assert not any(c.startswith("<swallowed") for c in board.commands)


def test_smart_terminal_queries_are_answered():
    board = FakeBoard(smart=True)
    console, amtx = connect(board)
    try:
        assert amtx.state()[0].ok
        assert "\x1b[1;999R" in board.terminal_replies
        assert "\x1b[1;1R" in board.terminal_replies
    finally:
        console.detach()


def test_a_failed_command_reports_its_error_and_does_not_leak_into_the_next(rig):
    board, console, amtx, _ = rig
    reply = amtx.station_delete(99)
    assert reply.ok is False and reply.error == "ESP_ERR_INVALID_ARG"
    follow = amtx.hostname_get()
    assert follow[0].ok is True and follow[1] == "amtx"
    assert follow[0].lines == ["hostname=amtx"]


def test_station_operations(rig):
    board, console, amtx, _ = rig
    assert amtx.station_add('Radio "Quoted" Name', "http://example/stream").ok
    assert board.nvs["stations"][-1]["name"] == 'Radio "Quoted" Name'
    assert amtx.station_move(2, 0).ok
    assert board.nvs["stations"][0]["url"] == "http://example/stream"
    assert amtx.station_default("b", None).ok and board.nvs["default"][1] == -1
    assert amtx.station_default("a", 2).ok and board.nvs["default"][0] == 2
    assert amtx.station_trim(1, 150).ok and board.nvs["stations"][1]["gain"] == 150
    assert amtx.station_trim(1, 5).ok is False
    listing = amtx.station_list()
    assert listing.ok and listing.detail == "3 station(s)" and len(listing.lines) == 3
    assert amtx.play("a", 1).ok and board.playing[0] == 1
    assert amtx.play(1, "http://direct/url").ok and board.play_url[1] == "http://direct/url"
    assert amtx.stop("b").ok and board.play_url[1] is None


def test_rf_round_trip(rig):
    board, console, amtx, _ = rig
    reply, values = amtx.rf_get("b")
    assert reply.ok and values["hz"] == "250000" and list(values) == ac.RF_KEYS
    assert amtx.rf_set("b", {"hz": 225000, "fade": "flutter", "agc": "off", "on": "on"}).ok
    _, values = amtx.rf_get(1)
    assert values["hz"] == "225000" and values["fade"] == "2" and values["agc"] == "0"
    assert amtx.rf_set("a", {"depth": 99}).ok is False
    with pytest.raises(ValueError):
        amtx.rf_set("a", {"volume": 3})


def test_tone(rig):
    board, console, amtx, _ = rig
    assert amtx.tone("a", True, 440).ok and board.tone[0] == 440
    assert amtx.tone("a", False).ok and board.tone[0] is None


def test_discover_then_save_and_play(rig):
    board, console, amtx, _ = rig
    reply, hits = amtx.discover("tag", "smooth jazz")
    assert reply.ok and reply.detail == "2 found"
    assert hits[0]["name"] == "Jazz FM | Classics" and hits[1]["kbps"] == 64
    assert board.commands[-1] == "discover tag smooth jazz"
    assert amtx.discover_save(0).ok and board.nvs["stations"][-1]["name"] == "Jazz FM | Classics"
    assert amtx.discover_play("b", 1).ok and board.play_url[1] == "http://smooth.example/stream"
    assert amtx.discover_save(7).ok is False


def test_hostname_and_web(rig):
    board, console, amtx, _ = rig
    with pytest.raises(ValueError):
        amtx.hostname_set("not valid!")
    reply = amtx.hostname_set("AMTX-Shed")
    assert reply.ok and "amtx-shed.local" in reply.detail and board.nvs["host"] == "amtx-shed"
    assert amtx.web_set(False).ok
    assert amtx.web_get() [1] is False
    assert amtx.web_set(True).ok and amtx.web_get()[1] is True


def test_wifi_commands_end_at_their_reply_line(rig):
    board, console, amtx, _ = rig
    started = time.monotonic()
    reply, saved = amtx.wifi_list()
    assert reply.complete and reply.ok is True and saved == ["HomeNet"]
    assert time.monotonic() - started < 1.0, "should end at ok, not a timeout"
    reply, nets = amtx.wifi_scan()
    assert reply.ok is True and [n["ssid"] for n in nets] == ["HomeNet", "Cafe Guest"]
    reply = amtx.wifi_join("Cafe Guest", "")
    assert reply.ok is True
    assert any(line.startswith("joined") for line in reply.lines)
    reply = amtx.wifi_join("HomeNet", "wrong")
    assert reply.ok is False and "wrong password" in reply.error
    assert amtx.wifi_save("Other", "pw").ok is True
    reply = amtx.wifi_forget()
    assert reply.ok is True and board.nvs["wifi"] == []
    status = amtx.wifi_status()
    assert status.ok is True and status.lines[0].startswith("state=connected")


def test_unknown_command_says_the_firmware_may_be_old(rig):
    board, console, amtx, _ = rig
    reply = amtx.run("frobnicate")
    assert reply.ok is False and "older than design 07" in reply.error


def test_a_heartbeat_log_after_the_prompt_does_not_break_replies():
    board = FakeBoard(smart=True, log_after_prompt=True)
    recorder = Recorder()
    console, amtx = connect(board, recorder)
    try:
        for _ in range(3):
            reply, doc = amtx.state()
            assert reply.ok and doc is not None
        reply, saved = amtx.wifi_list()
        assert reply.complete and saved == ["HomeNet"]
        assert len([t for t in recorder.of("log") if "wifi up" in t]) >= 3
    finally:
        console.detach()


def test_reboot_then_reconnect():
    board = FakeBoard(smart=True, reboot_seconds=0.4)
    recorder = Recorder()
    console, amtx = connect(board, recorder)
    try:
        reply = amtx.reboot()
        assert reply.ok is True and reply.complete
        deadline = time.monotonic() + 2.0
        while console.connected and time.monotonic() < deadline:
            time.sleep(0.02)
        assert not console.connected
        assert "disconnected" in [k for k, _ in recorder.events]
        with pytest.raises(ac.NotConnected):
            amtx.state()
        assert ac.attach_when_ready(console, board.open, timeout=3.0)
        assert board.boots == 1
        reply, doc = amtx.state()
        assert reply.ok and doc["sys"]["host"] == "amtx"
        assert "\x1b[0n" in board.terminal_replies       # the boot-time probe was answered
        assert any("ESP-ROM" in t for t in recorder.of("log"))
    finally:
        console.detach()


def test_a_command_too_long_for_the_console_is_refused_before_sending(rig):
    board, console, amtx, _ = rig
    with pytest.raises(ValueError):
        amtx.station_add("x" * 40, "http://" + "y" * 300)
    assert amtx.state()[0].ok


def test_disconnect_during_a_command_is_reported():
    board = FakeBoard(smart=False, join_seconds=1.0)
    console, amtx = connect(board)
    try:
        threading.Timer(0.3, board.reboot).start()
        reply = amtx.wifi_join("HomeNet", "secret")
        assert reply.ok is False and reply.disconnected
    finally:
        console.detach()
