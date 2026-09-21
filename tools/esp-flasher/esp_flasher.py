#!/usr/bin/env python3
"""ESP flasher: find a board, work out what is on it, and put a build on it.

A blank ESP32-S3 on native USB boot-loops, so its serial port appears and
disappears every couple of seconds and a plain `idf.py flash` loses it
mid-connect. This tool watches the bus, catches the board in download mode,
reads back the bootloader, partition table and app, compares them with a build
byte for byte, recommends a full or app-only flash, writes it, and opens a
monitor tab that survives the reset.

Run with no arguments for the GUI. `--list`, `--watch`, `--builds` and
`--probe` do the same discovery from the command line.

This file is the command line and the front door. The tool is split by job:

| Module | Holds |
| ------ | ----- |
| `flasher_config.py` | constants, the monitor and esptool imports, `Cancelled` |
| `flasher_images.py` | parsing an image header, an app description, a partition table |
| `flasher_firmware.py` | a build on disk, and finding builds |
| `flasher_bus.py` | watching the USB bus, spotting a boot loop (`LL-06`) |
| `flasher_probe.py` | catching the board, reading it back, comparing with a build |
| `flasher_esptool.py` | running esptool, and its v4/v5 differences |
| `flasher_ui.py` | the window, and the monitor tab that reconnects |

amtx-programmer subclasses `FlasherApp` and imports several of these names
through this module, so what is re-exported below is a surface other code
depends on.
"""

import argparse
import sys
import time
from datetime import datetime

import tkinter as tk

from flasher_bus import BusWatcher, find_port, port_key
from flasher_config import (
    DEFAULT_FLASH_BAUD,
    DEFAULT_PARTITION_TABLE_OFFSET,
    MONO_FONT,
    PORT_POLL_SECONDS,
    REPO_ROOT,
    TOOL_DIR,
    Cancelled,
    esptool,
    monitor,
)
from flasher_esptool import (
    build_flash_command,
    first_line,
    flash_board,
    select_images,
    wait_for_port,
)
from flasher_firmware import Firmware, discover_builds
from flasher_probe import assess, connect_board, probe_board
from flasher_ui import FlasherApp, FlasherMonitorTab

# Named so a reader can see what amtx-programmer depends on, and so a linter
# does not quietly remove an import that looks unused in this file.
__all__ = [
    "BusWatcher", "Cancelled", "DEFAULT_FLASH_BAUD", "Firmware", "FlasherApp",
    "FlasherMonitorTab", "MONO_FONT", "REPO_ROOT", "TOOL_DIR", "assess",
    "build_flash_command", "cli_resolve_board", "connect_board",
    "discover_builds", "esptool", "find_port", "flash_board", "main",
    "monitor", "port_key", "probe_board", "select_images", "wait_for_port",
]


# =========================

def cli_log(text, tag=None):
    sys.stdout.write(text)
    sys.stdout.flush()


def cli_resolve_board(name, wait=4.0):
    """The board named, or the only ESP board seen over a few seconds (a boot-looping one blinks)."""
    if name:
        port = find_port(name)
        if port is None:
            deadline = time.monotonic() + wait
            while port is None and time.monotonic() < deadline:
                time.sleep(PORT_POLL_SECONDS)
                port = find_port(name)
        if port is None:
            print("Port {} not found.".format(name))
            return None
        return port_key(port)

    seen = {}
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        for port in monitor.list_serial_ports(esp_only=True):
            seen[port_key(port)] = port
        if len(seen) == 1 and time.monotonic() > deadline - wait + 1.0:
            break
        time.sleep(PORT_POLL_SECONDS)
    if len(seen) == 1:
        return next(iter(seen))
    print("Name the board with --port. Seen:" if seen else "No ESP board found on the bus.")
    for key, port in seen.items():
        print("  {:<8} {}  {}".format(port["device"], key, port["adapter"]))
    return None


def cli_firmware(args):
    if args.bin:
        return Firmware.from_bin(args.bin, int(args.offset, 0) if args.offset else None)
    if args.build:
        return Firmware.from_build(args.build)
    builds = discover_builds()
    if not builds:
        return None
    return Firmware.from_build(builds[0])


def cli_watch(seconds):
    watcher = BusWatcher()
    end = time.monotonic() + seconds
    print("Watching the bus for {:.0f}s...".format(seconds))
    while time.monotonic() < end:
        for kind, key, info in watcher.poll(monitor.list_serial_ports(esp_only=True)):
            print("{}  {:<8} {:<9} {}  [{}]".format(datetime.now().strftime("%H:%M:%S.%f")[:-3],
                                                   info["device"], kind, key, watcher.state(key)))
        time.sleep(PORT_POLL_SECONDS)
    for key in watcher.boards:
        print("{}: {}".format(key, watcher.state(key)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Flash ESP boards: watch the bus, read what is on the board, write what it needs, "
                    "then monitor it. No arguments opens the GUI.")
    parser.add_argument("--list", action="store_true", help="list likely ESP boards and exit")
    parser.add_argument("--watch", type=float, metavar="SECONDS",
                        help="log boards appearing and vanishing, to spot a boot loop")
    parser.add_argument("--builds", action="store_true", help="list the build directories found and exit")
    parser.add_argument("--probe", action="store_true", help="read what is on the board and exit")
    parser.add_argument("--flash", action="store_true", help="flash the board and exit")
    parser.add_argument("--port", help="port or USB serial number (default: the only ESP board)")
    parser.add_argument("--build", help="ESP-IDF build directory (default: the newest build*/ found)")
    parser.add_argument("--bin", help="a single image instead of a build directory")
    parser.add_argument("--offset", help="where --bin goes (default 0x10000, or 0x0 for a merged image)")
    parser.add_argument("--mode", choices=["auto", "full", "app"], default="auto",
                        help="what to write (default: decided by probing)")
    parser.add_argument("--erase", action="store_true", help="erase the whole flash first")
    parser.add_argument("--baud", type=int, default=DEFAULT_FLASH_BAUD)
    parser.add_argument("--stay-in-download", action="store_true",
                        help="with --probe, leave the board in download mode even if it has an app")
    parser.add_argument("-y", "--yes", action="store_true", help="do not ask before flashing")
    args = parser.parse_args(argv)

    if monitor.serial is None or esptool is None:
        print("Missing dependencies. Install them with:\n"
              "    pip install -r tools/esp-flasher/requirements.txt", file=sys.stderr)
        return 2

    if args.list:
        monitor.print_ports(esp_only=True)
        return 0
    if args.watch is not None:
        return cli_watch(args.watch)
    if args.builds:
        for path in discover_builds():
            try:
                print(Firmware.from_build(path).label())
            except Exception as exc:
                print("{}  [unusable: {}]".format(path, first_line(exc)))
        return 0

    if args.probe or args.flash:
        try:
            firmware = cli_firmware(args)
        except Exception as exc:
            print("Firmware: {}".format(exc))
            return 1
        if firmware:
            print("Firmware: {}".format(firmware.source))
            for line in firmware.summary_lines():
                print("  " + line)
        key = cli_resolve_board(args.port)
        if key is None:
            return 1

        probe = None
        if args.probe or args.mode == "auto":
            pt_offset = firmware.partition_table_offset if firmware else DEFAULT_PARTITION_TABLE_OFFSET
            reset_after = False if (args.flash or args.stay_in_download) else None
            try:
                probe = probe_board(key, cli_log, pt_offset, reset_after)
            except Exception as exc:
                print("Probe failed: {}".format(exc))
                return 1
            report = assess(probe, firmware)
            print()
            for line in report["lines"]:
                print(line)
            if firmware:
                print("Recommended: {} ({})".format(report["mode"], "; ".join(report["reasons"])))
                for blocker in report["blockers"]:
                    print("Cannot flash: " + blocker)
        if not args.flash:
            return 0
        if firmware is None:
            print("No firmware: give --build or --bin.")
            return 1

        report = assess(probe, firmware) if probe else {"mode": args.mode, "blockers": []}
        mode = report["mode"] if args.mode == "auto" else args.mode
        if report["blockers"]:
            return 1
        images = select_images(firmware, mode)
        print("\nWill write{}:".format(" after erasing all flash" if args.erase else ""))
        for img in images:
            print("  0x{:06x}  {}".format(img["offset"], img["path"]))
        if not args.yes and input("Proceed? [y/N] ").strip().lower() not in ("y", "yes"):
            return 1
        ok = flash_board(key, firmware, mode, args.erase, args.baud, cli_log)
        print("Flash complete." if ok else "Flash did not complete.")
        return 0 if ok else 1

    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
