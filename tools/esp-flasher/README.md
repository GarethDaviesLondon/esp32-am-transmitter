# Tool: esp-flasher

## Purpose

A flashing console for ESP32 boards that works out what the board needs
instead of making you know. It:

1. **Watches the USB bus** and flags a board that keeps dropping off it, which
   is what a blank or corrupt ESP32-S3 on native USB does.
2. **Catches the board in download mode**, retrying on every reappearance, so a
   boot-looping board can be reached without holding BOOT.
3. **Reads back what is on the flash** (bootloader, partition table, app
   description) and compares each with the build you picked.
4. **Recommends what to write**: bootloader, partition table and app when the
   board is blank or its layout differs, the app alone when the rest already
   matches. It refuses a build for the wrong chip or a bigger flash.
5. **Flashes it** with esptool, and **opens a serial monitor tab** that follows
   the board through resets, so you see it boot and can use the `amtx>` CLI.

The monitor tabs are `tools/esp-serial-monitor`'s, imported rather than copied,
with reconnection added.

## In this project

- Pick the `build/` directory that `idf.py build` produced. The tool reads
  `build/flasher_args.json`, so it writes the same three images at the same
  offsets and flash settings `idf.py flash` would.
- The board is on native USB (`303A:1001`). The tool tracks it by the USB
  serial number, which is the chip's MAC, so it follows the board even if
  Windows hands it a different COM number after a reset. The baud rate is
  cosmetic on this port.
- A **new, blank board** shows `boot-looping (resets every 2.6s)` in the board
  list. On the bus for about 2.2 s, then gone for 0.4 s, while the ROM prints
  `invalid header: 0xffffffff` until the watchdog resets it. See `LL-06`.

## Usage

GUI (the normal way):

```
python tools/esp-flasher/esp_flasher.py
```

1. **Board.** Plug the board in; it appears in the list (a looping one too).
   Press **Probe board**. The probe is read-only; it reports the chip, flash
   size, MAC, and the state of each region against the selected build. A board
   with an app is reset back into it; a blank one is left in download mode,
   where it stops looping and stays on the bus.
2. **Firmware.** The newest `build*/` directory under the repo is preselected.
   **Build folder...** picks another, **.bin file...** picks a single image
   (a merged image goes at 0x0, anything else at the offset in the box).
3. **Flash.** Leave **Auto** on and press **Flash**. If the board has not been
   probed, it is probed first. A confirmation lists exactly what will be
   written and why. **Erase whole flash first** also wipes NVS: saved Wi-Fi
   networks and settings.
4. When the board comes back on the bus, a **Monitor** tab opens on it: logs
   and boot output in the `[SYS]` pane on top, the `amtx>` console in the
   terminal pane below (click it and type). If the board resets, the tab
   reconnects by itself unless you closed the port.

Command line, for the same operations without the window:

```
python tools/esp-flasher/esp_flasher.py --list              # ESP boards on the bus
python tools/esp-flasher/esp_flasher.py --watch 10          # appear/vanish log, spots a boot loop
python tools/esp-flasher/esp_flasher.py --builds            # build directories found
python tools/esp-flasher/esp_flasher.py --probe             # what is on the board vs the newest build
python tools/esp-flasher/esp_flasher.py --flash             # probe, confirm, write what it needs
python tools/esp-flasher/esp_flasher.py --flash --mode app --port COM5 --build build -y
```

A probe of a blank board, as seen on the second bench board (2026-09-16):

```
ESP32-S3 (QFN56) (revision v0.2)  MAC aa:bb:cc:dd:ee:ff  flash 4MB
Features: Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded Flash 4MB (XMC), Embedded PSRAM 2MB (AP_3v3)
Bootloader: blank
Partition table: blank
App: no app partition to look in
Board left in download mode (it will not run until flashed or reset).
Recommended: full (the board has no bootloader)
```

## Files

| File | Holds |
| ---- | ----- |
| `esp_flasher.py` | the command line, and the names amtx-programmer imports |
| `flasher_config.py` | constants, the monitor and esptool imports, `Cancelled` |
| `flasher_images.py` | parsing an image header, an app description, a partition table |
| `flasher_firmware.py` | a build on disk, and finding builds |
| `flasher_bus.py` | watching the USB bus, spotting a boot loop (`LL-06`) |
| `flasher_probe.py` | catching the board, reading it back, comparing with a build |
| `flasher_esptool.py` | running esptool, and its v4/v5 differences |
| `flasher_ui.py` | the window, and the monitor tab that reconnects |

## Dependencies

```
pip install -r tools/esp-flasher/requirements.txt
```

- `esptool` (v4.7 or v5) and `pyserial`. Probing uses esptool as a library;
  flashing runs `python -m esptool` as a child process, so Stop can kill it and
  its output streams into the log.
- `tools/esp-serial-monitor/esp_serial_monitor.py` must sit beside this
  directory; the monitor tabs come from it.
- The ESP-IDF Python environment already has both packages, so the tool also
  runs from an `export.ps1` shell.

## Redeployable?

`yes`, together with `esp-serial-monitor`. Nothing is specific to this project:
the image layout comes from the build's `flasher_args.json` and the image
headers. Worth a `TUNE:` look for another board:

- `CHIP_IDS`: the image-header chip ids recognised; an unknown one is shown by
  number and still flashes.
- `CONNECT_TIMEOUT`, `PORT_POLL_SECONDS`, `LOOP_WINDOW_SECONDS`: tuned for an
  S3 that is off the bus for 0.4 s per loop.

## Notes

- **The tool closes its own monitor tab before probing or flashing** and
  reopens it afterwards. A terminal in another program still blocks the port;
  close it first.
- **Auto recommends app-only only when the bootloader and partition table on
  the board are byte-identical to the build's.** Anything else gets a full
  flash, which is always safe.
- **A changed partition table can orphan NVS.** The recommendation says so;
  tick Erase if the board misbehaves after a layout change.
- **Early boot lines can be missed.** Native USB re-enumerates after the reset
  that follows a flash, and nothing is read until the port is back. Press the
  board's RESET with the tab open to see a whole boot.
- **If the board cannot be caught** within 25 s, put it in download mode by
  hand: hold BOOT, press and release RESET, release BOOT, then probe again.
- **What has been exercised on hardware:** listing, bus watch, probing a blank
  board (CLI and GUI), the GUI's plan, and a monitor tab opening. Not
  exercised: catching a board **while** it boot-loops using this finished tool
  (the same retry loop did catch it in a prototype on the second attempt),
  the flash itself, the post-flash reconnect, and esptool v4.
