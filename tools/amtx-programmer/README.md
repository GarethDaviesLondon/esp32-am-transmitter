# Tool: amtx-programmer

## Purpose

Sets up an AM transmitter board from a PC, over USB, without the web page.

It is `esp-flasher` with one more tab. The flasher finds the board, reads what
is on it and writes firmware. The **Program** tab then configures the running
transmitter with buttons and dialogs: what each transmitter plays, its RF
settings, the station list, Wi-Fi, the hostname, and whether the web interface
is on at all. Every button sends a command to the board's `amtx>` serial
console and shows the reply.

It is for the cases the web page cannot cover: a board with the web interface
turned off, a board not yet on any network, or several boards to set up one
after another.

## In this project

- The commands are the ones in `docs/design/07_console_and_access.md` §2. The
  firmware must include them. On older firmware the tab connects, but most
  buttons answer "unrecognised command (is the firmware older than design
  07?)". Flash a current build first; the Flash section above the tab does
  that.
- The board's console is on native USB. The tab holds the port while it is
  connected, so it closes any monitor tab on the same board when you press
  **Connect**, and lets go while the flasher probes or flashes. Opening a
  monitor tab on the board takes the port back from the Program tab.
- After a reboot or a flash the board drops off the bus. The tab reconnects by
  itself when it comes back, for up to 20 seconds.

## Usage

```
pip install -r tools/amtx-programmer/requirements.txt
python tools/amtx-programmer/amtx_programmer.py
```

1. Plug the board in and select it in the board list at the top.
2. Open the **Program** tab and press **Connect**. The tab reads the board's
   state and fills in every sub-tab.
3. Use the sub-tabs:
   - **Transmitters**: what A and B are playing, Stop, Play a URL, every RF
     setting with **Apply and save**, and the test tone.
   - **Stations**: the shared list. Play on A or B, Add, Delete, Move up and
     down, level Trim, and the power-on station for each transmitter.
   - **Discover**: search Radio-Browser by name, genre or country, then play a
     result or save it to the list.
   - **Network and access**: Wi-Fi scan, join, save and forget; the hostname;
     turning the web interface on or off; the board's log output.
   - **Console log**: every command sent and every reply line, with log lines
     from the board marked `[SYS]`. A box at the bottom sends any command by
     hand.
4. **Refresh** reads the state again. It also happens after every change.
   Tick "Refresh every 5 s" to keep the view live.

Every reply shows in the status bar at the bottom of the tab. A command that
fails also opens a dialog saying why.

Without a board:

```
python tools/amtx-programmer/amtx_programmer.py --sim      # the GUI against a simulated board
python tools/amtx-programmer/amtx_programmer.py --check    # build the window, connect to the simulation, exit
```

One command from the command line, no window:

```
python tools/amtx-programmer/amtx_programmer.py --port COM5 --run "state"
python tools/amtx-programmer/amtx_programmer.py --run "station list"
```

## Files

The Program tab is split by panel: each mixin owns the widgets it builds, the
part of the state document it renders, and what its buttons do. Adding a
control means opening the file named after the thing it controls.

| File | Holds |
| ---- | ----- |
| `amtx_programmer.py` | the command line and the window |
| `amtx_console.py` | the console as a request and reply protocol, no Tk in it |
| `programmer_config.py` | constants, and the flasher and monitor imports |
| `programmer_tab.py` | the Program tab: connection, worker thread, refresh |
| `programmer_transmitters.py` | the two transmitter panels and the test tone |
| `programmer_stations.py` | the station list panel |
| `programmer_discover.py` | the Radio-Browser panel |
| `programmer_network.py` | Wi-Fi, hostname, web interface, reboot |
| `programmer_dialogs.py` | the form dialog they all ask with |
| `fake_board.py` | a simulated board, for the tests and `--sim` |

## How it talks to the console

`amtx_console.py` is the protocol, with no GUI in it:

- **A reply ends at `ok` or `error:`** for every command design 07 added.
- **`wifi`** ends with one too. `wifi join` and `wifi scan` wait up to 25
  seconds for it, because a join can be silent for 15.
- **`status`, `log` and `reboot`** print no such line. Their reply ends when
  the console prints its `amtx>` prompt again, which the REPL does before
  reading every line, or after a short quiet period if the prompt never shows.
  `reboot` ends at "rebooting" or when the port drops.
- **The console asks the terminal where its cursor is** (`ESC[6n`) before
  every line, and waits for the answer with no timeout, reading anything sent
  meanwhile as the answer. The reader answers those queries itself, as the
  monitor's terminal does. Without that, commands are swallowed.
- **Log lines are separated** with `esp-serial-monitor`'s `ConsoleSplitter`,
  imported, not copied.
- **On connecting** it sends an empty line and waits for a fresh prompt, which
  also clears anything half-typed in the console.

`fake_board.py` is a model of that console, for the tests and for `--sim`: a
smart and a dumb terminal mode, coloured log lines in the middle of replies,
the REPL's own error trailer, and a reboot that takes the port away. Where the
firmware's wording differs from it, the firmware is right.

## Tests

The repo's root `python -m pytest` collects only `cad/` and `designs/`, so run
these by path:

```
python -m pytest tools/amtx-programmer
```

34 tests: text parsing, every command in both terminal modes against the
fake board, a failure not leaking into the next reply, a disconnect in the
middle of a command, reboot and reconnect, and the Program tab itself driven
with its window hidden. The GUI tests skip where Tk cannot open a window.

## Dependencies

- `pyserial` and `esptool`, through `tools/esp-flasher/requirements.txt`.
- `tools/esp-flasher/` and `tools/esp-serial-monitor/` must sit beside this
  directory; the program imports both.
- `pytest`, for the tests only.

## Redeployable?

`partly`. `amtx_console.py`'s reply handling suits any ESP-IDF `esp_console`
REPL whose commands print a final `ok` or `error:` line; `PROMPT` and the
legacy command list are the project-specific parts. The Program tab and
`fake_board.py` are this firmware's command set and nothing else.

## What has been verified

- **In this session, with no board:** the test suite passes (34 tests, run
  more than twenty times in a row after the last change). `--check` builds the
  window, connects to the simulated board and shows its stations.
- **Not verified:** nothing has run against a real board. The firmware with
  the design 07 commands had not been flashed anywhere when this was written,
  so every reply format here comes from the design document and ESP-IDF's
  REPL source, not from a board. The dialogs have been built and driven by the
  tests but not looked at by a person.
- One test run failed, once, before the `--check` test moved into its own
  process, and its output was not captured. A second Tk window in the same
  process had also been failing to start intermittently at that point, which
  is the likely cause, but it is not proven.
