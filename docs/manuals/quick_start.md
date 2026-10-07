# Quick Start

From a downloaded release to a transmitter playing internet radio, with no
compiler and no ESP-IDF. About twenty minutes, most of it waiting for
downloads.

This is the short path. It flashes a firmware binary that is already built.
If you want to change the firmware and build it yourself, read the
Installation Manual instead. To make the case and wind the antennas, read the
Build Manual.

## Contents

1. What you need
2. Get the release
3. Install Python
4. Plug the board in
5. Flash it
6. Talk to the board
7. Put it on your Wi-Fi
8. Hear something
9. Before you leave it running
10. When it goes wrong

## 1. What you need

| Item | Notes |
| ---- | ----- |
| An ESP32-S3 board | 8 MB flash. A bare "mini" dev board is fine. No display, no audio hardware, no buttons needed |
| A USB cable | Data, not charge-only. A charge-only cable looks like a dead board |
| A Wi-Fi network | 2.4 GHz. The board has no 5 GHz radio |
| An AM receiver | Longwave capable, if you keep the default 200 kHz and 250 kHz carriers |
| A PC | Windows, macOS or Linux |

You do **not** need the 3D-printed case, the loop antennas or any soldering to
get to the end of this guide. The board will transmit a few centimetres on its
own, which is enough to prove it works with a receiver sitting next to it.

### One thing to get right before anything else

Your board probably has two USB sockets. One is a USB-to-serial bridge
(CH343, CP210x or similar). The other is the ESP32-S3's own USB peripheral.

**Use the native USB port.** The firmware puts its console there. On the
bridge you will see the ROM bootloader banner and nothing else, and you will
conclude the board is broken when it is not.

On Windows the native port appears in Device Manager as
`USB JTAG/serial debug unit`, with a `USB Serial Device` COM number. On Linux
it is usually `/dev/ttyACM0`, on macOS `/dev/cu.usbmodem*`.

If the board has only one socket, that is the native one.

## 2. Get the release

Go to the [latest release](https://github.com/GarethDaviesLondon/esp32-am-transmitter/releases/latest)
and download the **source code zip**, not just the loose `.bin` files. The zip
carries the flashing tool as well as the firmware, and you want both.

Unzip it anywhere. You will end up with a directory holding, among other
things:

```
binaries/
  amtx-v1.0-merged.bin        the firmware, everything in one file
tools/
  esp-flasher/                the flashing tool you are about to run
  esp-serial-monitor/         the serial terminal it uses
docs/
  manuals/                    this guide and the other four
```

The version number in the `.bin` filename will match the release you
downloaded, so `amtx-v1.0-merged.bin` may read `v1.1` or later. Use whatever
is actually there.

## 3. Install Python

The flashing tool is a Python program. It needs **Python 3.10 or newer**.

- **Windows:** install from [python.org](https://www.python.org/downloads/).
  On the first screen of the installer, tick **Add python.exe to PATH**. This
  is the step everybody misses, and without it the commands below report
  `python: command not found`.
- **macOS:** `brew install python`, or the python.org installer.
- **Linux:** Python 3 is already there. You also need Tk, which is packaged
  separately: `sudo apt install python3-tk` on Debian or Ubuntu. Without it
  the tool stops with `ModuleNotFoundError: No module named 'tkinter'`.

Check it:

```
python --version
```

If that reports Python 2, or nothing, try `python3 --version` and use
`python3` everywhere below.

Now install the two packages the tool needs. From the directory you unzipped:

```
pip install -r tools/esp-flasher/requirements.txt
```

That is `pyserial`, which finds the board, and `esptool`, which writes the
flash. Nothing else.

## 4. Plug the board in

Connect the board's native USB port to the PC.

**A brand-new board will look broken.** A blank ESP32-S3 has nothing to boot,
so the ROM gives up and the watchdog resets it, about every 2.6 seconds. On
native USB that means the board appears on the bus, vanishes, and appears
again, over and over. Windows may chime repeatedly. This is correct behaviour
for an empty board and the flasher is built to cope with it.

## 5. Flash it

Change into the flasher's directory and run it:

```
cd tools/esp-flasher
python esp_flasher.py
```

Note the underscore in `esp_flasher.py`.

A window opens with three sections: Board, Firmware, Flash.

1. **Board.** Your board is in the list. A new one is labelled something like
   `boot-looping (resets every 2.6s)`, which is the tool telling you it has
   noticed the loop in §4. Select it and press **Probe board**.

   The probe only reads. It reports the chip, the flash size, the MAC address,
   and what is currently in each region. A blank board reports
   `Bootloader: blank` and is left in download mode, where it stops looping
   and sits still on the bus.

2. **Firmware.** The tool preselects the newest `build*/` directory it can
   find. **A release download has no `build/` directory**, so there will be
   nothing selected or the wrong thing selected. Press **`.bin` file...** and
   choose `binaries/amtx-v1.0-merged.bin` from the unzipped release.

   The tool recognises a merged image and places it at offset `0x0` by itself.
   You do not need to touch the offset box.

3. **Flash.** Leave **Auto** ticked and press **Flash**. A confirmation lists
   exactly what it is about to write and why. Accept it.

   Writing takes a few seconds. Leave **Erase whole flash first** unticked:
   on a new board there is nothing to erase, and on a used board it also wipes
   saved Wi-Fi networks and settings.

When the board comes back on the bus, a **Monitor** tab opens on it by itself.

### If you would rather not use the tool

The flasher is the friendlier route, but the plain `esptool` command does the
same job in one line, and it is the route that has had the most use on real
hardware:

```
pip install esptool
python -m esptool --chip esp32s3 -p COM5 write_flash 0x0 binaries/amtx-v1.0-merged.bin
```

Replace `COM5` with your port. On Linux that is `/dev/ttyACM0`, on macOS
`/dev/cu.usbmodem*`.

## 6. Talk to the board

The Monitor tab has two panes. Boot output and log messages scroll in the
`[SYS]` pane on top. The console is the terminal pane below.

**Click the lower pane before you type.** Keystrokes go to whichever pane has
focus, and typing into the log pane does nothing at all, which is a
dispiriting way to start.

You should see the board boot, join nothing yet, and settle. Press Enter and
you get the prompt:

```
amtx>
```

Log messages keep scrolling past while you type, which makes the console hard
to read. Turn them off:

```
amtx> log off
```

Now check the firmware is really running and talking to you:

```
amtx> help
```

That lists every command the board understands, in alphabetical order. You
should see `play`, `station`, `rf`, `tone`, `discover`, `wifi`, `hostname`,
`web`, `status`, `state`, `log`, `factory-reset` and `reboot`.

**If `help` lists those commands, the firmware is on the board and working.**
That is the milestone. Everything from here is configuration.

One more worth trying, because it prints the whole state of the board on one
line:

```
amtx> status
```

## 7. Put it on your Wi-Fi

The board has no network yet. There are two ways to give it one. They reach
the same result and save to the same place, so use whichever suits you.

### Option A: the setup page, from a phone

Best if you are away from the PC, or setting up a board that is already in its
case.

With no saved network, the board raises its own open Wi-Fi access point called
**`AMTX-Setup`**.

1. On a phone or laptop, join `AMTX-Setup`. It has no password.
2. The setup page opens by itself, as a captive portal. If it does not, open a
   browser and go to `http://192.168.4.1`.
3. Press **Scan**, pick your network, type its password, and save.
4. The board reboots and joins.

Once it has joined, the access point goes away and the board is at
**`http://amtx.local`**, or at the address shown on the page's Network tab.
That page is the full interface: stations, both transmitters, RF settings,
diagnostics.

### Option B: the console, from the PC

Best if you are already sitting at the Monitor tab, which you are.

See what is in range:

```
amtx> wifi scan
```

You get a numbered list with signal strengths:

```
 0  MyNetwork                        -52 dBm  secured
 1  BTWifi-X                         -78 dBm  open
ok: 2 network(s) found
```

Then join, which also saves:

```
amtx> wifi join MyNetwork mypassword
```

It prints `joining "MyNetwork" ...`, takes up to fifteen seconds, and then
tells you what happened:

```
joined "MyNetwork", ip 192.168.1.42, rssi -52 dBm
ok: joined MyNetwork, ip 192.168.1.42, saved (1 stored)
```

Put double quotes round anything containing a space:

```
amtx> wifi join "My Home Network" "pass phrase with spaces"
```

Useful companions:

| Command | Does |
| ------- | ---- |
| `wifi status` | Where it is now, and its address |
| `wifi list` | The networks it has saved |
| `wifi save <ssid> [pass]` | Save for next boot without joining now |
| `wifi forget` | Delete every saved network |

A joined board prints its address. Open `http://amtx.local` in a browser and
you have the web interface, the same as Option A.

## 8. Hear something

A factory-fresh board comes up with two stations already in its list, one on
each transmitter:

| Transmitter | Default station | Default carrier |
| ----------- | --------------- | --------------- |
| A | Radio Swiss Jazz | 200 kHz |
| B | BBC World Service | 250 kHz |

So once it is on your network it should already be transmitting. Check with:

```
amtx> status
```

Now tune a receiver. Put it next to the board, switch it to **longwave**, and
tune slowly around 200 kHz. Without an antenna the range is a few centimetres,
so the receiver needs to be touching or nearly touching the board.

If you are hunting for the carrier and want something unmistakable, a steady
tone is far easier to find than music:

```
amtx> tone a on 1000
```

That replaces the programme on transmitter A with a 1 kHz tone. Find it on the
dial, then put the music back:

```
amtx> tone a off
```

Heard it? That is the whole chain working: Wi-Fi, the stream, the decoder, the
modulator and the carrier.

For everything you can do from here, read the User Manual.

## 9. Before you leave it running

Two things to understand, and they are not formalities.

**It transmits in a licensed broadcast band.** Longwave runs from 148.5 kHz to
283.5 kHz. The default 200 kHz carrier sits inside it, a couple of kilohertz
from BBC Radio 4's allocation on 198 kHz. Deliberate radiation there needs a
licence in the UK and in most other places, and this project has none. Keep
the field inside the room: no outdoor wire, nothing connected to mains wiring,
nothing resembling a real antenna. What you radiate, and the rules where you
live, are yours to check.

**There is no output filter in a bare-board build.** The carriers are square
waves, and a square wave at 200 kHz radiates odd harmonics at 600 kHz, 1.0 MHz
and upward, across the whole medium-wave band. For a few minutes on a bench
with a receiver beside it, that is tolerable. For anything left running, fit
the series resistor and low-pass network for each carrier pin, as the Build
Manual describes. Each carrier needs its **own** filter: one filter shared
between both lets them intermodulate.

## 10. When it goes wrong

| What you see | What it is |
| ------------ | ---------- |
| `python: command not found` on Windows | Python was installed without **Add python.exe to PATH**. Re-run the installer and tick it |
| `No module named 'tkinter'` | Linux, missing Tk. `sudo apt install python3-tk` |
| No board in the list | Charge-only cable, or the wrong USB socket. See §1 |
| Only the ROM banner, no `amtx>` prompt | You are on the USB-to-serial bridge, not the native port. See §1 |
| Board appears and vanishes repeatedly | Normal for a blank board. Flash it. See §4 |
| `Could not open COM5, the port is busy` | Something else holds the port. Close other terminals. A terminal whose window you closed can leave its process running |
| Nothing selected in the Firmware section | Expected on a release download: there is no `build/` directory. Press **`.bin` file...**. See §5 |
| The flasher cannot catch the board | Put it in download mode by hand: hold BOOT, press and release RESET, release BOOT, then probe again |
| `unrecognised command` from a command in §6 | The firmware on the board is older than this guide. Flash the release binary |
| Typing does nothing in the Monitor tab | Click the lower pane first. See §6 |
| Joined Wi-Fi but `amtx.local` will not open | Use the IP address from `wifi status`. Some networks and some Windows setups do not resolve `.local` names |
| No audio, carrier present | The station URL may be dead. Try `station list`, then `play a <index>` with another, or `discover name jazz` to find one |

### Starting over

To wipe everything saved, including Wi-Fi networks and station changes, and
return the board to how it arrived:

```
amtx> factory-reset confirm
```

## Where to go next

| Manual | For |
| ------ | --- |
| User Manual | Driving it: the page, the stations, both transmitters, tuning a receiver |
| Build Manual | Making the real thing: printing the case, winding the antennas, wiring, the output filters |
| Installation Manual | Building the firmware from source, with ESP-IDF |
| Technical Manual | How the firmware works inside, for changing it |
