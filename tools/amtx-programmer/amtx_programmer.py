#!/usr/bin/env python3
"""amtx-programmer: the flasher, plus a tab that sets up a running transmitter.

esp-flasher gets firmware onto a board. This adds the step after it: telling
the board what to play, what to transmit on, what network to join and what to
call itself, over the `amtx>` serial console rather than the web page. Every
button here is a console command from design 07, and every reply is shown.

The flasher is imported, not copied, as the flasher imports the monitor.

Run with no arguments for the GUI. `--sim` runs against a simulated board,
`--check` builds the window and exits, and `--run` sends one command.

This file is the command line and the window that holds the tab. The tool is
split by job:

| Module | Holds |
| ------ | ----- |
| `amtx_console.py` | the console as a request and reply protocol, no Tk in it |
| `programmer_config.py` | constants, and the flasher and monitor imports |
| `programmer_tab.py` | the Program tab: connection, worker thread, refresh |
| `programmer_transmitters.py` | the two transmitter panels and the test tone |
| `programmer_stations.py` | the station list panel |
| `programmer_discover.py` | the Radio-Browser panel |
| `programmer_network.py` | Wi-Fi, hostname, web interface, reboot |
| `programmer_dialogs.py` | the form dialog they all ask with |
| `fake_board.py` | a simulated board, for the tests and `--sim` |
"""

import argparse
import sys
import time

import tkinter as tk
# messagebox is re-exported rather than used here: a caller patching
# amtx_programmer.messagebox patches the one tkinter module object every
# panel shows its warnings through, which is what the tests rely on.
from tkinter import messagebox  # noqa: F401

import amtx_console as ac

from programmer_config import LEGACY_COMMANDS, flasher, monitor
from programmer_tab import ProgramTab


class ProgrammerApp(flasher.FlasherApp):
    """The flasher, plus the Program tab."""

    def __init__(self, root, sim_board=None):
        super().__init__(root)
        self.root.title("AMTX Programmer")
        self.program = ProgramTab(self, self.notebook, sim_board=sim_board)
        # Straight after the flasher log: no monitor tab exists yet at start-up.
        self.notebook.add(self.program.frame, text="Program")
        self.notebook.select(self.program.frame)
        if sim_board is not None:
            self.log("Simulated board: the Program tab talks to fake_board.FakeBoard, not to hardware.\n", "info")

    def release_monitors(self, key):
        super().release_monitors(key)
        self.program.release_for_flasher(key)

    def hand_port_to_program(self, key):
        """Close monitor tabs on this board so the Program tab can open its port."""
        for tab in self.tabs.values():
            if tab.board_key == key and tab.session.is_open:
                tab.close_port()
                tab.emit("--- port handed to the Program tab; press Reopen port to take it back ---\n", "info")

    def open_monitor(self, port_info):
        self.program.yield_port(flasher.port_key(port_info))
        return super().open_monitor(port_info)

    def on_ports(self, ports):
        super().on_ports(ports)
        self.program.on_ports(self.ports)

    def on_flashed(self, key, ok, reappeared):
        if ok and self.program.wants(key):
            # The Program tab reconnects by itself; a monitor tab would take the port from it.
            self.log("Flash complete; the Program tab reconnects to the board.\n", "info")
            self._flashed_quietly(key)
            return
        super().on_flashed(key, ok, reappeared)

    def _flashed_quietly(self, key):
        saved = self.monitor_after_var.get()
        self.monitor_after_var.set(False)
        try:
            super().on_flashed(key, True, None)
        finally:
            self.monitor_after_var.set(saved)

    def on_close(self):
        self.program.shutdown()
        super().on_close()


def run_one_command(port_name, command_text):
    key = flasher.cli_resolve_board(port_name)
    if key is None:
        return 1
    console = ac.AmtxConsole(on_event=lambda kind, text: print("[SYS] " + text) if kind == "log" else None)

    def open_transport():
        port = flasher.find_port(key)
        if port is None:
            raise ac.TransportClosed("not on the bus")
        return ac.SerialTransport(port["device"])

    if not ac.attach_when_ready(console, open_transport, timeout=10.0):
        print("No amtx> prompt from the board. Is the amtx firmware running, and the port free?")
        return 1
    try:
        first = command_text.split()[0]
        reply = console.command(command_text, timeout=ac.SLOW_TIMEOUT if first in ("discover", "wifi")
                                else ac.DEFAULT_TIMEOUT, legacy=first in LEGACY_COMMANDS,
                                quiet=None, expect_reboot=first == "reboot")
    finally:
        console.detach()
    for line in reply.lines:
        print(line)
    print(reply.summary())
    return 1 if reply.ok is False or not reply.complete else 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Flash an AM transmitter board and set it up through its serial console. "
                    "No arguments opens the GUI.")
    parser.add_argument("--sim", action="store_true", help="open the GUI against a simulated board")
    parser.add_argument("--port", help="port or USB serial number, for --run (default: the only ESP board)")
    parser.add_argument("--run", metavar="COMMAND", help="send one console command, print the reply, exit")
    parser.add_argument("--check", action="store_true", help="build the window, close it again, and exit")
    args = parser.parse_args(argv)

    if monitor.serial is None or flasher.esptool is None:
        print("Missing dependencies. Install them with:\n"
              "    pip install -r tools/amtx-programmer/requirements.txt", file=sys.stderr)
        return 2

    if args.run:
        return run_one_command(args.port, args.run)

    sim_board = None
    if args.sim or args.check:
        from fake_board import FakeBoard
        sim_board = FakeBoard(smart=True, noisy=True)

    root = tk.Tk()
    app = ProgrammerApp(root, sim_board=sim_board)
    if args.check:
        return run_check(root, app)
    root.mainloop()
    return 0


def run_check(root, app):
    """Build the window, drive a few actions against the simulated board, and close."""
    root.withdraw()
    program = app.program
    program.connect()
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline and not (program.console.connected and program.doc):
        root.update()
        time.sleep(0.02)
    ok = program.console.connected and program.doc is not None
    stations = len(program.station_tree.get_children()) if ok else 0
    app.on_close()
    print("check: window built; simulated board {}; {} station(s) shown".format(
        "connected" if ok else "NOT connected", stations))
    return 0 if ok and stations else 1


if __name__ == "__main__":
    sys.exit(main())
