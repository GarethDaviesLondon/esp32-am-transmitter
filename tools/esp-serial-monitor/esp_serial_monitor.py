#!/usr/bin/env python3
"""ESP serial monitor.

A tkinter serial console aimed at ESP32/ESP8266 development boards: it finds
which port a board is on, works out what baud rate the firmware is talking at,
and opens one monitor tab per board with keyboard entry back to the device.
Think PuTTY, but it knows what an ESP board looks like and it can watch several
boards at once.

Run with no arguments for the GUI. `--list` and `--detect` give the same
discovery from the command line.

This file is the command line and the front door. The tool is split by job:

| Module | Holds |
| ------ | ----- |
| `monitor_config.py` | constants, and whether pyserial is installed |
| `monitor_ports.py` | finding, describing and opening a port |
| `monitor_baud.py` | working out a board's baud rate by listening |
| `monitor_session.py` | one open port, read on its own thread |
| `monitor_split.py` | telling log output from console output |
| `monitor_terminal.py` | the VT100 subset linenoise needs |
| `monitor_text.py` | key names, escapes, hex rows, timestamps |
| `monitor_tab.py` | one board's tab and its two panes |
| `monitor_app.py` | the window, the port list, the detect dialog |

esp-flasher and amtx-programmer `import esp_serial_monitor as monitor`, so the
names they use are re-exported below and this module is their stable surface:
moving something between the modules above must not move it out of here.
"""

import argparse
import re
import sys

import tkinter as tk

from monitor_app import DetectDialog, SerialMonitorApp
from monitor_baud import detect_baud, summarise_results
from monitor_config import (
    COMMON_BAUDS,
    DEFAULT_BAUD,
    DEFAULT_LISTEN_SECONDS,
    MAX_LINES_IN_VIEW,
    MONO_FONT,
    serial,
)
from monitor_ports import list_serial_ports, open_port, print_ports
from monitor_session import SerialSession
from monitor_split import ConsoleSplitter, strip_ansi
from monitor_tab import MonitorTab
from monitor_text import expand_escapes, hex_rows, timestamp

# Named so a reader can see what the other tools depend on, and so a linter
# does not quietly remove an import that looks unused in this file.
__all__ = [
    "COMMON_BAUDS", "ConsoleSplitter", "DEFAULT_BAUD", "DEFAULT_LISTEN_SECONDS",
    "DetectDialog", "MAX_LINES_IN_VIEW", "MONO_FONT", "MonitorTab",
    "SerialMonitorApp", "SerialSession", "detect_baud", "expand_escapes",
    "hex_rows", "list_serial_ports", "main", "open_port", "print_ports",
    "serial", "strip_ansi", "summarise_results", "timestamp",
]



def resolve_detect_target(name):
    """Pick the port to probe: the one named, or the only obvious board."""
    if name:
        for port in list_serial_ports():
            if port["device"].lower() == name.lower():
                return port
        print("Port {} not found. Ports available:".format(name))
        print_ports()
        return None

    candidates = list_serial_ports(esp_only=True)
    if len(candidates) == 1:
        return candidates[0]
    print("Name the port to probe. Candidates:" if candidates else "No likely ESP board found.")
    print_ports()
    return None


def run_cli_detect(args):
    port = resolve_detect_target(args.detect)
    if port is None:
        return 1

    bauds = None
    if args.bauds:
        bauds = [int(b) for b in re.split(r"[,\s]+", args.bauds.strip()) if b]

    print("Probing {} ({}){}".format(
        port["device"], port["adapter"],
        "  -  native USB CDC: baud is cosmetic on this port" if port["native_usb"] else ""))
    results = detect_baud(
        port["device"], bauds=bauds, listen_seconds=args.seconds, reset=args.reset,
        stop_when_confident=not args.all_bauds, progress=lambda msg: print("  " + msg),
    )
    if not results:
        print("No result.")
        return 1

    print()
    print("{:<9} {:<7} {:<10} {:<6} {}".format("BAUD", "BYTES", "PRINTABLE", "SCORE", "SAMPLE"))
    for row in results:
        print("{:<9} {:<7} {:<10} {:<6} {}".format(
            row["baud"], row["bytes"], "{:.0%}".format(row["printable_ratio"]),
            "{:.2f}".format(row["score"]), row["error"] or row["sample"] or "(silence)"))

    print()
    print(summarise_results(results))
    return 0 if not all(r["error"] for r in results) else 1


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Serial monitor for ESP boards: find the port, find the baud rate, watch and type.")
    parser.add_argument("--list", action="store_true",
                        help="list serial ports and which look like ESP boards, then exit")
    parser.add_argument("--esp-only", action="store_true",
                        help="with --list, show only likely ESP boards")
    parser.add_argument("--detect", nargs="?", const="", metavar="PORT",
                        help="probe a port for its baud rate and exit "
                             "(port optional if exactly one board is plugged in)")
    parser.add_argument("--bauds", metavar="LIST",
                        help="comma-separated rates to probe instead of the defaults")
    parser.add_argument("--seconds", type=float, default=DEFAULT_LISTEN_SECONDS,
                        help="seconds to listen at each rate (default {})".format(DEFAULT_LISTEN_SECONDS))
    parser.add_argument("--no-reset", dest="reset", action="store_false",
                        help="do not pulse the board reset while probing; just listen")
    parser.add_argument("--all-bauds", action="store_true",
                        help="probe every rate instead of stopping at the first confident match")
    args = parser.parse_args(argv)

    if serial is None:
        print("pyserial is not installed. Install it with:\n"
              "    pip install -r tools/esp-serial-monitor/requirements.txt\n"
              "or:\n"
              "    pip install pyserial", file=sys.stderr)
        return 2

    if args.list:
        print_ports(esp_only=args.esp_only)
        return 0

    if args.detect is not None:
        return run_cli_detect(args)

    root = tk.Tk()
    SerialMonitorApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
