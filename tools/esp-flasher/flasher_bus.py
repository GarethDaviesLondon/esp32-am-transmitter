"""Watching the USB bus, and spotting a board that will not stay on it.

A blank ESP32-S3 on native USB reboots every couple of seconds, so its port
appears and disappears. `BusWatcher` turns that into events, and a board that
drops off twice inside the window is called boot-looping, which is `LL-06` and
the reason this tool exists at all.
"""

import time
from collections import deque

from flasher_config import LOOP_WINDOW_SECONDS, monitor


# =========================

def port_key(port):
    """A board's identity across re-enumeration: its USB serial number if it has one.

    The S3's native USB reports its MAC as the serial number, so a board keeps
    its key even if Windows hands it a different COM number after a reset.
    """
    return port["serial_number"] or port["device"]


def find_port(key, ports=None):
    for port in ports if ports is not None else monitor.list_serial_ports():
        if port_key(port) == key or port["device"] == key:
            return port
    return None


class BusWatcher:
    """Tracks boards appearing and vanishing, to spot one that is boot-looping.

    A blank or corrupt ESP32-S3 on native USB resets every couple of seconds,
    and every reset takes the USB device off the bus. Two or more drops inside
    the window is the signature.
    """

    def __init__(self, window=LOOP_WINDOW_SECONDS):
        self.window = window
        self.boards = {}

    def poll(self, ports, now=None):
        now = time.monotonic() if now is None else now
        present = {port_key(p): p for p in ports}
        events = []
        for key, port in present.items():
            board = self.boards.setdefault(key, {"info": port, "present": False, "drops": deque(),
                                                 "looping_reported": False, "last_seen": now})
            board["info"] = port
            board["last_seen"] = now
            if not board["present"]:
                board["present"] = True
                events.append(("appeared", key, port))
        for key, board in self.boards.items():
            if board["present"] and key not in present:
                board["present"] = False
                board["drops"].append(now)
                events.append(("vanished", key, board["info"]))
            while board["drops"] and now - board["drops"][0] > self.window:
                board["drops"].popleft()
            looping = self.is_looping(key)
            if looping and not board["looping_reported"]:
                board["looping_reported"] = True
                events.append(("looping", key, board["info"]))
            elif not looping and board["looping_reported"] and board["present"]:
                board["looping_reported"] = False
                events.append(("settled", key, board["info"]))
        return events

    def is_looping(self, key):
        board = self.boards.get(key)
        return board is not None and len(board["drops"]) >= 2

    def loop_period(self, key):
        drops = list(self.boards[key]["drops"])
        if len(drops) < 2:
            return None
        return (drops[-1] - drops[0]) / (len(drops) - 1)

    def state(self, key):
        board = self.boards.get(key)
        if board is None:
            return ""
        if self.is_looping(key):
            period = self.loop_period(key)
            return "boot-looping (resets every {:.1f}s)".format(period) if period else "boot-looping"
        if not board["present"]:
            return "gone"
        return "on the bus"
