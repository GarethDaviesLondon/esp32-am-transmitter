# Tool: esp-serial-monitor

## Purpose

A serial console for ESP32/ESP8266 development boards that answers the two
questions a terminal program makes you answer first: **which port is the board
on, and what baud rate is it talking at?** It lists the ports, marks the ones
whose USB adapter belongs to an ESP board, probes a port to work out its baud
rate from the traffic itself, and opens a monitor tab per board with keyboard
entry back to the device.

Each monitor tab has **two panes**. The top one collects **system messages**,
each line prefixed `[SYS]`: ESP-IDF log lines (`I (1234) wifi: ...`, coloured
by level), ROM banners (`rst:`, `invalid header`) and crash dumps, plus the
monitor's own notices. The bottom one is a **terminal for the board's
console**: click in it and type, PuTTY style. It understands the escape
sequences ESP-IDF's line editor sends and answers its terminal probe, so arrow
keys, history and TAB completion work.

It removes the loop of guessing a COM port, guessing 115200, seeing garbage and
guessing again. It also watches several boards at once in one window, which
`pio device monitor` will not do and a mesh or multi-node bring-up needs.

## In this project

The board is an ESP32-S3 with the console on **USB Serial/JTAG**, not a UART
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`). Two
consequences when you use this tool here:

- The port shows up as `Espressif native USB (JTAG/serial)` (VID `0x303A`, PID
  `0x1001`), already in `KNOWN_ADAPTERS`, so it is flagged as a likely ESP board
  without any tuning.
- **The baud rate is cosmetic.** It is a USB CDC link, so any rate works and
  "Detect baud..." has nothing real to measure. Open the monitor at 115200 and
  ignore the rate; the tool says the same thing when it recognises the adapter.

Reset-pulse caveat: the native USB port has no DTR/RTS auto-reset transistors,
so "Reset board while probing" does nothing here. Press the board's EN button,
or reflash, to see the boot banner.

The value of the tool on this project is the multi-board case: with two
transmitters under test, or a board and a receiver-side jig, it watches both in
one window, which `idf.py monitor` will not do. It is also the way to read the
log when `idf.py monitor` is unavailable (no ESP-IDF export in that shell, or a
Windows session outside the IDF environment).

## Usage

GUI (the normal way):

```
python3 tools/esp-serial-monitor/esp_serial_monitor.py
```

1. **Boards** lists every serial port, likely ESP boards first, with the adapter
   it recognised (CP210x, CH340, FTDI, or the S2/S3/C3 native USB). Tick
   "Likely ESP boards only" to hide Bluetooth and modem ports.
2. Select a port and click **Detect baud...** if you do not know the rate. It
   tries each candidate rate in turn, scores what comes back for readability,
   and ranks them. Double-click the winning row to open a monitor at that rate.
3. Or set the rate yourself and click **Open monitor**. Select several ports and
   they all open, one tab each.
4. Click in the **Console** pane and type. Every keystroke goes to the board as
   you press it: the arrow keys as ANSI sequences, TAB for completion, Ctrl+C
   as `0x03` (or a copy, when text is selected), Ctrl+V pastes to the board.
   What appears is the board's own echo. **Enter sends** CR by default, which
   is what ESP-IDF's console reads as Enter; switch it to LF or CRLF for
   firmware that wants those. The **Send line** box under the pane sends a
   whole line at once, with its own history.
5. System messages land in the top pane as they arrive, even in the middle of a
   half-typed command, so the console stays readable. Untick **Split system
   messages** to see the raw interleaved stream in the terminal instead.

Command line, for the same discovery without the window:

```
$ python3 tools/esp-serial-monitor/esp_serial_monitor.py --list
PORT       ADAPTER                            ESP?    DESCRIPTION
COM18      Espressif native USB (JTAG/serial) yes     USB Serial Device (COM18)
           USB VID:PID=303A:1001 SER=AA:BB:CC:DD:EE:FF
COM3       Bluetooth virtual port             -       Standard Serial over Bluetooth link (COM3)

$ python3 tools/esp-serial-monitor/esp_serial_monitor.py --detect COM18
Probing COM18 (Espressif native USB (JTAG/serial))
  Trying 115200 baud...
  115200 baud: 412 bytes, score 1.10
  Confident match at 115200 baud; stopping.

BAUD      BYTES   PRINTABLE  SCORE  SAMPLE
115200    412     100%       1.10   I (325) cpu_start: Pro cpu up. | I (330) app_main: running

Best match: 115200 baud.
```

`--detect` with no port picks the board itself if exactly one is plugged in.
Other flags: `--bauds 115200,74880` to probe a specific list, `--seconds` to
listen longer at each rate, `--no-reset` to listen without restarting the board,
`--all-bauds` to probe every rate instead of stopping at the first confident
match, `--esp-only` to filter `--list`.

### What each monitor tab gives you

| Control | Does |
| ------- | ---- |
| Baud + Apply | change rate on a live port, without reopening it |
| DTR / RTS | drive the control lines by hand, for boards with unusual wiring |
| Reset board | pulse the auto-reset circuit so you see the boot banner |
| Send break | assert a UART break, for firmware that uses one as a signal |
| Close port / Reopen | free the port for a flash, then pick the session back up |
| Split system messages | route log lines, ROM output and crash dumps to the `[SYS]` pane; off sends everything to the terminal |
| Timestamps | prefix each system line with local time to the millisecond |
| Hex view | 16-column hex dump of every byte in the system pane, for binary protocols |
| Pause | freeze the display while the port keeps draining |
| Log file | append to a file as it arrives: `[SYS]` system lines, `[CLI]` finished console lines |
| Enter sends | CR, LF or CRLF, for the Enter key in the terminal and the Send line box |
| Escapes | interpret `\n`, `\r`, `\t`, `\0`, `\xHH` in the Send line box |
| Local echo | also show what the Send line box sent, for firmware that does not echo |

## Files

One job per module. `esp_serial_monitor.py` is the command line and the front
door, and the names other tools use are re-exported from it, so
`import esp_serial_monitor as monitor` keeps working whatever moves below.

| File | Holds |
| ---- | ----- |
| `esp_serial_monitor.py` | the command line, and the public surface |
| `monitor_config.py` | constants, and whether pyserial is installed |
| `monitor_ports.py` | finding, describing and opening a port |
| `monitor_baud.py` | working out a board's baud rate by listening |
| `monitor_session.py` | one open port, read on its own thread |
| `monitor_split.py` | telling log output from console output |
| `monitor_terminal.py` | the VT100 subset linenoise needs |
| `monitor_text.py` | key names, escapes, hex rows, timestamps |
| `monitor_tab.py` | one board's tab and its two panes |
| `monitor_app.py` | the window, the port list, the detect dialog |

Module names are prefixed `monitor_` because these tools are scripts on
`sys.path` rather than packages: the flasher puts its own directory and this
one on the path together, and two modules with the same name would shadow each
other.

## Dependencies

```
pip install -r tools/esp-serial-monitor/requirements.txt
```

- `pyserial` for port enumeration and I/O. Everything else is the standard
  library (`tkinter`; on some Linux distributions install it separately with
  `sudo apt install python3-tk`).
- Linux users need to be in the `dialout` group to open a port, or every open
  fails with a permission error.
- No network access. The tool writes to the board only what you type, plus the
  reset pulse when you ask for one.

## Redeployable?

`yes`. No project-specific paths and nothing to tune for a normal ESP project.
Two constants are worth a `TUNE:` look for an unusual board:

- `KNOWN_ADAPTERS`: add the VID/PID of a bridge chip the project's boards use if
  it is not in the list. An unlisted adapter still works; it just is not flagged
  as a likely ESP board.
- `DEFAULT_PROBE_BAUDS` / `COMMON_BAUDS`: trim to the rates the project's
  firmware actually uses to make detection quicker.

If the project pins a `monitor_speed` in `platformio.ini`, that is the rate the
firmware talks at; detection is for the boards where nobody wrote it down.

Neither was tuned for this project: `KNOWN_ADAPTERS` already carries the S3's
native-USB VID/PID, and the probe list is irrelevant on a USB CDC link.

The two-pane monitor (the system/console split and the terminal emulation) was
added here, after the tool was installed from the catalogue, so the live copy
is now ahead of the catalogue copy. None of it is project-specific: it keys on
the ESP-IDF log format and the escape sequences of ESP-IDF's `linenoise`, so it
back-ports as is.

## Notes

- **Detection needs the port free and the board talking.** It opens the port
  exclusively, so close any other monitor first (the tool says so plainly when
  the open fails). If the firmware prints nothing at boot, every rate comes back
  silent and the tool says that rather than guessing.
- **The reset pulse assumes the standard auto-reset wiring** (DTR to IO0, RTS to
  EN). It holds IO0 high so the board restarts into the application, not the ROM
  bootloader. A board without those transistors ignores it; untick "Reset board
  while probing" and detection just listens.
- **On the S2/S3/C3 native USB port the baud rate is cosmetic.** It is a USB CDC
  link, not a real UART, so any rate works and the tool says so. Baud only
  matters on a board with a separate bridge chip.
- **A board that sends non-ASCII log output scores lower** than a plain ASCII
  one, because the score rewards printable bytes. Read the sample column rather
  than trusting the score alone.
- Opening a port resets many boards, whatever this tool does with DTR/RTS: that
  is the adapter's wiring, not the software. Expect to see a boot banner.
- The tool never flashes firmware. Close the port (or the tab) before flashing,
  or the flasher cannot claim it.
- Each pane keeps the last 5000 lines. Turn on a log file for a long run.
- **How a line is judged a system message:** it starts with the ESP-IDF log
  format (`I (1234) tag: `, optionally coloured), or with a known ROM or panic
  prefix (`rst:`, `ESP-ROM:`, `Guru Meditation`, `Backtrace:` and so on), or a
  coloured log line starts in the middle of console text. Text that might be
  the start of one is held back for at most 0.15 s, so typing does not lag.
  With `CONFIG_LOG_COLORS` off, a log line printed over a half-typed prompt is
  not recognised and stays in the terminal.
- **The terminal emulates a small VT100 subset:** CR, LF, backspace, tab,
  erase in line, cursor movement, clear screen, colours, and the `ESC[5n` /
  `ESC[6n` status queries. That is what ESP-IDF's console uses. A full-screen
  program would need more.
- **Line editing depends on the probe at boot.** ESP-IDF asks the terminal
  whether it understands escape sequences when the console starts. If no
  terminal was listening then (the port was closed during boot), the console
  falls back to "dumb" mode for that boot: typing still works, but the arrow
  keys and history do not. Reboot with the tab open to get line editing.
