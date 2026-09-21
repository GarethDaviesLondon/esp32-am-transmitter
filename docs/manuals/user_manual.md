# User Manual

Everything you can do with the transmitter once it is built and on your
network. To get to that point, see the Installation Manual.

The board has no display, no buttons and no knobs. You drive all of it from a
web page it serves itself, at `http://amtx.local` or at its IP address, or
from the serial console, which can do everything the page can.

## Contents

1. The page at a glance
2. Now playing
3. Stations
4. Discover
5. Transmitter
6. Network
7. Diagnostics
8. Tuning a receiver
9. The serial console
10. Before you transmit

![The transmitter, open](../images/tx-case-open-from-above.jpg)

The built transmitter: the lid carries the two loop windings and their series
resistors, the base carries the board. `docs/images/wiring.svg` is what is
wired to what.

## 1. The page at a glance

There are **two independent transmitters**, A and B. Each plays its own
station on its own carrier frequency, out of its own pin, with its own
modulation settings. They share your station list and your Wi-Fi.

Six tabs across the top, and a header that always shows both carriers.

```
A ON AIR 200.0 kHz 70%    B ON AIR 250.0 kHz 70%
```

Amber is A, blue is B. Grey and reading `OFF` means that carrier is not
running. The numbers are the carrier frequency and the modulation depth.

Below the tabs is an **Acting on** bar with two buttons. Whichever transmitter
is selected there is the one the Stations, Discover and Transmitter tabs act
on. Now playing and Diagnostics always show both, because what is on air is
the question you want answered without clicking anything.

Every button reports back. Press one and it indents, then a message slides up
from the bottom of the screen telling you what happened. A red message means
the transmitter refused, and it says why. This matters more than it sounds:
with no display on the board, that message is the only confirmation you get
that a press did anything at all.

Two actions restart the board, so they raise a dialog you have to dismiss
rather than a message that fades. Both are on the Network tab.

## 2. Now playing

The station name, its URL, and a **Modulation** meter.

The meter shows the peak level going into the modulator. Green is normal.
Amber above 80% means you are running hot. Red above 97% means the limiter is
working hard and the sound will suffer. If it sits red, lower the programme
gain on the Transmitter tab.

One card per transmitter, each with its own controls.

**Stop** stops that transmitter's audio. Its carrier stays up, unmodulated, so
a receiver tuned to it hears silence rather than losing the station. The other
transmitter is untouched.

**Carrier** switches that output on and off. The button says what pressing it
will do.

**Play a URL directly** takes any HTTP MP3 stream address and plays it without
saving it. Use it to try a station before you keep it.

## 3. Stations

Your saved list, up to 20 stations.

Each row has a drag grip, the name and URL, and five controls.

| Control | What it does |
| ------- | ------------ |
| ★ | Makes this the station the transmitter plays at power-on |
| Play | Tune to it now |
| % | Level trim for this station, 10 to 400%. For when the AGC gets one wrong |
| ↑ ↓ | Move it one place |
| × | Delete it |

### Ordering

Drag a row by any part of it and drop it where you want it. The order is saved
as you go, so it survives a power cut.

The arrows do the same job. Keep them in mind on a phone or tablet, where
dragging does not work: HTML5 drag-and-drop is a mouse feature and touch
browsers do not implement it.

### The power-on station

Press the star on a row and that station becomes the one **the selected
transmitter** tunes when it powers up. The star turns amber. Press it again to
clear it.

Each transmitter has its own star. Switch between A and B on the Acting on bar
and the stars change to show that transmitter's choice, so you can have A come
up on one station and B on another.

With no star set, the board comes back on whatever it was playing when it lost
power. With a star set, it always comes back on that station, whatever was
playing before.

The star follows its station when you reorder the list. Drag the starred
station from the bottom to the top and it stays starred. Delete the starred
station and the star clears rather than jumping to a neighbour, because a
default nobody chose is worse than none.

### Adding by hand

Type a name and a stream URL and press **Add**. The URL must be plain HTTP or
HTTPS to an MP3 stream. Playlist files (`.pls`, `.m3u`) are not parsed, and
HLS streams (`.m3u8`) will connect and then stop, because the firmware expects
a continuous MP3 byte stream.

## 4. Discover

Search a community directory of internet radio stations rather than hunting
for URLs yourself.

Type something, choose whether it searches by name, genre or country, and
press **Search**. Results come back ordered by how many people listen to them.
Each row offers:

- **Play**, which tunes to it now without saving.
- **Save**, which adds it to the end of your station list.

Results are limited to ten. That was a memory limit: a secure connection to
the directory takes tens of kB while it runs, and each result is roughly
1.5 kB of data, which once exhausted the internal RAM. PSRAM is enabled now,
so the limit could probably be raised, but it has not been retested and the
cap still stands. Narrow the search rather than expecting more rows.

Two filters are applied for you, and both reflect what the firmware can
actually play:

- **MP3 only.** It is the only decoder compiled in. An AAC or OGG station
  would save cleanly and then play silence.
- **Plain HTTP only.** Encrypted streams are filtered out at the directory.

## 5. Transmitter

The RF settings. Press **Apply and save** to commit them. They persist.

| Setting | Range | What it does |
| ------- | ----- | ------------ |
| Carrier | 60 to 300 kHz | The frequency you tune the receiver to |
| Modulation depth | 5 to 90% | How far the envelope swings |
| Audio low-pass | 500 to 9500 Hz | Where the audio is cut off |
| Programme gain | 0 to 400% | Level into the limiter |
| Carrier output | On or off | The output itself |

These act on the transmitter selected in the Acting on bar. The panel border
shows which: amber for A, blue for B.

**Carrier.** A defaults to 200 kHz and B to 250 kHz, both in the longwave
band. Not every frequency you type will be accepted. The hardware divides an
80 MHz clock and needs 256 duty steps per cycle, so **312.5 kHz is a hard
ceiling** and the medium-wave band is out of reach at this duty resolution.
Ask for 800 kHz or 1.2 MHz and the transmitter refuses and keeps the frequency
it had. The message tells you.

Keep the two carriers apart, and mind the harmonics: 200 kHz has its third
harmonic at 600 kHz, so a second carrier is not the only thing sharing the
air with the first.

**Modulation depth.** 70% is a good starting point. Above 75% the trough
pinches the carrier and an envelope detector, which is what a cheap AM radio
is, turns that into distortion. The setting allows more so you can hear the
effect. It is not an improvement.

**Audio low-pass.** 4500 Hz keeps the transmission inside a 9 kHz channel.
Raising it makes the audio brighter and puts energy into the next channel
along, where it becomes someone else's problem.

**Programme gain.** Broadcast AM runs hot, and 140% is the default for that
reason. Watch the modulation meter on the Now playing tab. If it sits red,
come down.

**Carrier level.** How hard the transmitter drives, 5 to 100%. This is the
honest way to turn the power down: it shrinks the whole radiated envelope,
rather than turning the audio down and leaving the carrier at full strength.
It costs resolution, because the envelope then uses fewer duty steps, so at
25% there is a quarter of the amplitude resolution there was at 100%. Use it
to keep the field small; use programme gain and the AGC to control loudness.

### Loudness AGC

Stations differ by more than 10 dB in average level. Measured on this board:
Jazz Blues averaged 0.738 while Radio Swiss Jazz averaged 0.213, on identical
settings. A compressed music service runs near full scale, a jazz service with
real dynamic range averages far lower, and speech with pauses lower still.

The limiter cannot fix that, because a limiter only pushes peaks down and never
lifts quiet material up. The AGC tracks the programme average and rides the
gain to a target, which is what broadcast AM processing does and why AM
stations all sound equally loud. With it on, those two stations came to within
1.9 dB of each other.

It shows what it is doing beside the switch, as a gain in dB.

**Target level** is what it aims the average at. 25% is a good default. Higher
is louder and closer to the limiter.

It trades dynamic range for loudness, which is the same bargain every
broadcaster makes. Switch it off per transmitter if you would rather keep the
dynamics. For the odd station it gets wrong, use the per-station trim: the
percentage button on the station row.

### Fading

For fun, and for realism. Wanders the carrier level so the signal comes and
goes the way a distant station does. It acts on the carrier rather than the
audio, so the noise floor rises and falls with it, exactly as propagation
does.

| Mode | What it sounds like |
| ---- | ------------------- |
| Slow fade | Long, lazy fades. Try 0.05 to 0.2 Hz |
| Flutter | Faster wobble. Aircraft flutter, aurora. Try 3 to 10 Hz |
| Random drift | Three unrelated rates summed, so it never repeats. The most convincing |

**Rate** sets the speed and **Depth** how far down it dips. 100% depth takes
the carrier to nothing at the bottom of the fade.

It costs nothing when off.

### Test tone

Replaces the programme with a steady tone at the modulator's own rate. Put a
scope on the RF pin and you see the envelope with the decoder and the network
taken out of the picture. Useful for checking the output stage without
wondering whether the stream is at fault.

## 6. Network

The top panel shows the state, the network, the address, the signal strength
and how many times the connection has dropped.

**Scan for networks** lists what the board can see, with signal strengths.
Press **Use** on one to fill in the name. Only 2.4 GHz networks appear, because
that is all the radio has.

**Password** has an eye button beside it. Press it to read what you typed
before you commit, which saves a reboot cycle spent wondering.

**Save and join** stores the credentials and restarts to join. The page will
lose contact. That is expected, and the dialog says so.

**Reboot** restarts the transmitter.

**Hostname** is the name the board answers to, `http://<name>.local`. Every
board starts as `amtx`, so with two on one network, give each its own name or
only one of them answers. Letters, digits and hyphens, up to 31. The new name
works at once; the router's device list shows it after a reboot.

**Turn off the web interface** stops this page altogether, for a network you
do not trust: the page has no password. It asks first, because the page
cannot turn itself back on. Only the serial console can, with `web on`, and
while the web is off there is no setup portal either.

On a phone held upright a banner asks you to turn it sideways. The page works
upright, but it is laid out for landscape. Dismiss the banner with its ×.

### Starting again from nothing

**Hold the board's BOOT button for five seconds** while it is running. The
button is marked **B**; the other one, **R**, is RESET and cannot do this.

The transmission stops, and the board restarts as if it were new: no stations
but the two it was built with, no saved networks, default transmitter
settings, the name `amtx`, and the web interface back on. Then it raises the
`AMTX-Setup` access point and waits to be set up again, as in the Installation
Manual §6.

Let go before five seconds and nothing happens at all. If the serial console
is connected it counts down while you hold it.

The same thing, from further away:

| Where | How |
| ----- | --- |
| The board | Hold **B** for five seconds |
| This page | Network tab, **Erase everything and start again** |
| The console | `factory-reset confirm` |

It is the way back when the page cannot be reached at all: a board whose saved
network no longer exists, or one whose web interface was turned off on a
network that has since gone. It does not touch the firmware, only what the
board remembers.

## 7. Diagnostics

Everything the board knows about itself. Worth reading when something sounds
wrong.

| Row | What to look for |
| --- | ---------------- |
| Stream buffer | Should hover, not sit empty or pinned full |
| HTTP status | 200 when playing |
| Reconnects | Climbing means an unreliable station |
| Last error | The reason the stream last stopped |
| Decoded frames | Should climb steadily, roughly 40 per second |
| Decode errors | A few at the start are normal. Climbing is not |
| Stream format | Sample rate, channels and bitrate the station is sending |
| Resampler trim | Parts per million of correction. Near 5000 means the source rate is far from what the chain expects |
| AGC gain | What the loudness AGC is adding or removing right now, in dB |
| Modulator FIFO | Should sit near half full |
| Modulator underruns | Should stop climbing once a station is playing |
| Free heap | Falling steadily over hours would suggest a leak |
| Uptime | Resets to zero if the board restarted without you asking |

The most useful pair is **decoded frames** and **modulator underruns**. Frames
climbing with underruns static means the whole chain is healthy.

## 8. Tuning a receiver

Set the receiver to the carrier frequency shown in the header. With the
default 200 kHz you need a longwave radio: most portable radios sold outside
Europe are medium-wave and FM only, and will not reach it.

Hold the receiver near the loop. Signal falls off very quickly with distance,
which is the point.

If you hear the station on the receiver but the sound is distorted, try
lowering the modulation depth, then the programme gain.

If you hear nothing, check the header says `ON AIR`, check Now playing shows a
station and the modulation meter is moving, then check your output network.

## 9. The serial console

Connect the native USB port and open a terminal at 115200 baud. Press Enter
for the `amtx>` prompt. It works whether or not Wi-Fi is up, which is the
point of it: the web page is unreachable exactly when the network is the thing
that is broken.

| Command | What it does |
| ------- | ------------ |
| `help` | Lists the commands |
| `status` | One line of state, meant to be read by a script. Shared fields first, then `carrier0`, `hz0`, `playing0` and so on per transmitter, then `host` and `web` |
| `state` | Everything the page shows, as one line of JSON |
| `play a 0` | Play station 0 on transmitter A. `play b http://...` plays a URL on B |
| `stop a` | Stop transmitter A's stream |
| `station list` | The stations, numbered from 0 |
| `station add "Name" http://...` | Add one. Quote a name with spaces |
| `station delete 3`, `station move 3 0` | Delete, reorder |
| `station default a 0` | Station 0 plays on A at power-on. `none` clears it |
| `station trim 2 120` | Station 2 plays 20% louder |
| `rf a` | Transmitter A's settings |
| `rf a hz 198000 depth 60` | Change any of `on hz depth lpf gain level agc agct fade faderate fadedepth` |
| `tone a on 1000`, `tone a off` | Test tone |
| `discover tag jazz` | Search the directory by `name`, `tag` or `country` |
| `discover play a 2`, `discover save 2` | Play or save result 2 |
| `hostname`, `hostname kitchen` | Show or set the hostname |
| `web`, `web off`, `web on` | Show, stop or start the web interface. Saved |
| `factory-reset` | Explains what would be lost and does nothing |
| `factory-reset confirm` | Erases everything saved and restarts |
| `wifi scan` | What the radio can see |
| `wifi join <ssid> <pass>` | Join now, and save it if it works |
| `wifi save <ssid> <pass>` | Save without joining |
| `wifi list` | Saved networks |
| `wifi forget` | Delete all saved networks |
| `log off` | Stop the log scrolling past |
| `log on` | Start it again |
| `reboot` | Restart |

`wifi join` tries for up to 15 seconds. It retries by itself when the router
misses a frame or a scan misses the network. When it gives up, it says why:

| Message | Usually means |
| ------- | ------------- |
| `reason 201, no network with that name` | A typo. Names are case-sensitive; copy it from `wifi scan` |
| `reason 15` or `204`, `wrong password` | The password, or its case |
| `reason 2, router did not answer authentication` | The router ignores the board. Check for a MAC filter on the router; the board's MAC is in the boot log |
| `reason 210` or `211, network security not supported` | The router is WPA3-only. Allow WPA2 as well |

Every command except `status`, `log` and `reboot` finishes with a line
starting `ok` or `error:`, which says whether it worked. `tools/amtx-programmer/` drives the
board through them with buttons and dialogs.

`log off` is there so the console can be driven by a script. With the log
quiet, `status` returns a single parseable line and nothing else. Logging
comes back on at every boot, because a silent board looks like a dead one.

The baud rate does not really matter. The console is a USB device rather than
a real serial port, so the setting is negotiated away.

## 10. Before you transmit

This is a transmitter, and two facts about it are not negotiable.

**Fit an output low-pass network to each pin.** Without it, a square-wave
carrier radiates harmonics across the whole medium-wave band and interferes
with every station a domestic radio can tune. Design 03 §4 has a network that
works. With two transmitters you need two of them: coupling both pins into one
network lets the carriers intermodulate, and the products land where neither
carrier is.

**Keep the field inside the room.** The default 200 kHz sits in the longwave
broadcast band, 2 kHz from BBC Radio 4 on 198 kHz. Use an inductive loop
beside the receiver, not an antenna. Deliberate radiation there is licensed in
the UK and in most other places, and this project holds no licence. What you
radiate, and the rules where you live, are yours to check.

Nothing in this project has been measured off-air. The audio chain is tested
and the firmware runs, but what the pin actually radiates has never been
observed on a receiver or an analyser.
