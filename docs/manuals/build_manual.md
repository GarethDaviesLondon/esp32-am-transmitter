# Build Manual

How to build one of these from parts: print the case, wind the antenna, wire
it, flash it, and get it playing. Allow an evening, plus printing time.

It assumes you can solder two joints and use a screwdriver. It does not assume
you have built anything with an ESP32 before.

For using it once it runs, see the User Manual. For how it works inside, see
the Technical Manual. For building the firmware from source rather than
flashing the supplied binary, see the Installation Manual.

## Contents

1. What you need
2. Print the case
3. Fit the heat-set inserts
4. Wind the antenna
5. Wire it up
6. Fit the board
7. Close the box
8. Flash the firmware
9. First boot
10. Check it works
11. Before you transmit
12. If it goes wrong

## 1. What you need

### Parts

| Part | Notes |
| ---- | ----- |
| Waveshare ESP32-S3-Zero | The board this is built around. 24 x 18mm, USB-C. Other ESP32-S3 boards will run the firmware but will not fit the case |
| 2 x 220 ohm resistors | Quarter watt, ordinary carbon or metal film. One per transmitter |
| 0.3mm enamelled copper wire | About 1.5m. Sometimes sold as magnet wire or transformer wire |
| 4 x M3 heat-set inserts | Brass, for 3mm screws, about 4mm outside diameter and 5 to 6mm long |
| 4 x M3 countersunk screws | 8 to 10mm long |
| 2 x M3 self-tapping screws | Short, for the cable clamp. Optional: the clamp is only needed if a cable leaves the box |
| USB-C cable | A data cable, not a charge-only one |
| An AM receiver | Longwave capable for the default 200 kHz, or retune the transmitter to medium wave |

### Tools

A 3D printer and about 40g of PLA. A soldering iron, and a way to melt the
inserts in: a spare iron tip is ideal, the tip of a cheap iron is fine. Snips,
a small screwdriver, fine sandpaper or a scalpel for the enamel.

## 2. Print the case

Three parts: `base`, `lid` and `clamp`. Print each **as supplied and the right
way up**: the models are already in their print orientations, and the whole
design exists to avoid supports.

| Setting | Value |
| ------- | ----- |
| Material | PLA |
| Layer height | 0.2mm |
| Infill | 25% |
| Supports | **None.** Nothing in these parts needs them |
| Orientation | As supplied. Do not let the slicer "lay flat" anything |

**Load each part as its own object.** The base and the lid share an origin and
a footprint, and a slicer told to load them as one object will print them
inside each other. That has happened on this project: `PF-01`. If your slicer
offers "load as a single object with multiple parts", do not use it here.
There is also a `sprue` file with all three joined by thin tags, for a printing
service that charges a minimum per part; snap the tags and clean the stubs.

The lid prints outer-face-down, so the lettering ends up on the build plate and
comes out with the plate's finish. The pillars point up.

## 3. Fit the heat-set inserts

Four, one in the foot of each of the lid's pillars. The pillar feet are the
flat-bottomed bosses on what will be the inside of the lid.

1. Set the iron to about 200 C for PLA.
2. Sit an insert, narrow end down, on a pillar foot.
3. Press it in slowly with the iron, square to the surface. It should sink
   under its own weight plus a light push. If you have to lean on it, it is
   too cold.
4. Stop when the insert's top is flush. Let it set before touching it.

Square matters more than speed: a crooked insert means a screw that will not
start, and there is no second attempt in the same hole.

## 4. Wind the antenna

The lid's four pillars are the former. Each pillar has **two channels**
separated by a flange, so the lid carries **two independent windings**, one per
transmitter. Both are **5 turns** of 0.3mm enamelled copper.

1. Start with the lower channel, nearest the lid face. Leave a 100mm tail.
2. Wind 5 turns around all four pillars, keeping the wire in the same channel
   the whole way round. Pull it snug but do not stretch it: the wire runs in
   the 3.5mm gap between the pillars and the wall, and it should sit in the
   channel without bowing.
3. Bring the other end back with another 100mm tail.
4. Repeat in the upper channel for the second winding, leaving two tails again.

The hooks on the long walls hold the wire away from the lid joint. Tuck each
run under them as you go, and the lid will close without trapping anything.

**Leave the tails long.** You can always cut wire off, and the tails have to
reach the resistor uprights and then the board.

Five turns on this former encloses about 15 square centimetres, which is what
was tested and heard on a receiver. More turns is not obviously better: it
raises the inductance and the resistance together.

## 5. Wire it up

One transmitter is: **a GPIO pin, through a 220 ohm resistor, through a 5-turn
winding, back to ground.** There are two of them, and they share only the
ground.

```
GPIO 4 ──── 220R ──── winding A (5 turns) ──── GND
GPIO 5 ──── 220R ──── winding B (5 turns) ──── GND
```

The lid has two pairs of **resistor uprights** near the rear pillars, one pair
per winding. Each pair holds one resistor:

1. Bend both of the resistor's legs square at the shoulders of its body, so
   they point the same way, like a staple.
2. Push the legs into the two bores. They run front to back, towards the back
   stop. The body ends up lying on the lid between the uprights.
3. The legs now stick out towards the back stop, ready to solder to.

Then, per transmitter:

- **Scrape the enamel** off about 8mm of each winding tail. Enamelled wire
  looks like bare copper and is not: if you skip this the joint will look
  perfect and conduct nothing. Sandpaper, a scalpel back, or a hot iron and
  solder will all do it. It is shiny copper when it is clean.
- **Solder one winding tail to one resistor leg.**
- **Solder the other resistor leg to a flying lead** long enough to reach the
  board's GPIO pad, and the other winding tail to a lead for ground.
- Repeat for the second winding and the second resistor.

Finally solder the four leads to the board: **GPIO 4** and **GPIO 5** for the
two transmitters, and both grounds to any **GND** pad. The pads are labelled on
the board's silkscreen.

It does not matter which end of a winding goes to the resistor and which to
ground: it is a loop either way. It does matter that the two windings do not
touch each other, which is what the flange between the channels is for.

## 6. Fit the board

1. **Slide the board in** from the open end, component side up, into the two
   slots along the long walls. It stops against the USB-C wall, with the
   socket lined up with the opening. Nothing clips or springs: if it does not
   slide, something is in the way.
2. **Route the leads.** Four thin wires have to get from the lid to the board.
   They go under the board and through the arch at the foot of the back stop,
   or out sideways through the window each pillar cuts in the slot. Two small
   ribs on the floor under the back of the board hold a cable on the
   centreline so the lid does not sweep it aside as it closes.
3. **If a cable leaves the box**, lay it in the saddle on the lid, put the
   clamp bar over it and fix it with the two self-tapping screws. Weave it
   past the three ledges on the lid and the hooks on the base walls first: the
   point of the weave is that a pull on the cable outside the box is taken by
   the box and not by the solder joints.

## 7. Close the box

Offer the lid up so its four pillars drop into the four corners and the back
stop slides down its guides behind the board. It should sit down without
pressure. **If it does not, stop and look**: a trapped winding tail is the
usual reason, and forcing it is how a pillar snaps.

Turn the whole thing over and drive the four M3 countersunk screws up through
the base floor into the inserts. Snug, not tight: they are pulling brass into
plastic. The heads sit below the underside so the box still stands flat.

## 8. Flash the firmware

Two ways in. Either works; the first needs nothing but Python.

### With the supplied binary

The release carries `amtx-vX.Y-merged.bin`, which is bootloader, partition
table and application in one file at offset 0.

```
pip install esptool
python -m esptool --chip esp32s3 -p COM5 write_flash 0x0 amtx-vX.Y-merged.bin
```

Replace `COM5` with your port: `/dev/ttyACM0` or similar on Linux and macOS.
**Use the board's USB-C socket**, which is the ESP32-S3's own USB: this
firmware puts its console there.

If the board is brand new and keeps dropping off the USB bus while you try,
that is a known trait of a blank ESP32-S3 (`LL-06`). Hold the **B** (BOOT)
button while you plug it in, which forces the ROM's download mode, and flash
then.

### With the graphical flasher

`tools/esp-flasher/esp_flasher.py` finds the board, tells you what is on it,
recommends a full or partial flash and does it, then opens a serial monitor.
`tools/amtx-programmer/amtx_programmer.py` is the same plus a tab that sets the
board up over USB afterwards.

```
pip install -r tools/esp-flasher/requirements.txt
python tools/esp-flasher/esp_flasher.py
```

### From source

See the Installation Manual: ESP-IDF v5.3.x, `idf.py set-target esp32s3`,
`idf.py build`, `idf.py -p PORT flash`.

## 9. First boot

With no saved network the board raises its own access point.

1. Join the Wi-Fi network **`AMTX-Setup`** from a phone or laptop. Give it
   about 30 seconds to appear after power-up.
2. A setup page should open by itself. If not, browse to `http://192.168.4.1`.
3. **Network** tab, **Scan for networks**, pick yours from the list, type the
   password, **Save and join**. Pick from the list rather than typing the name:
   the board has no 5 GHz radio, and a hand-typed 5 GHz name will never join.
4. The board restarts to join, so the setup page disconnects. That is the
   success path, not a failure.
5. Rejoin your own network and open **`http://amtx.local`**. If you have more
   than one of these, give this one its own name on the Network tab.

It comes up playing Radio Swiss Jazz on transmitter A and the BBC World
Service on transmitter B, on 200 kHz and 250 kHz.

## 10. Check it works

Work down the list; each line is something you can see or hear.

| Check | What you should see |
| ----- | ------------------- |
| It has joined | The Network tab shows a state of `connected` and an address |
| It is playing | Now playing shows a station name and `playing`, and the modulation meter moves |
| It is decoding | Diagnostics shows decoded frames climbing, about 40 per second |
| The modulator is fed | Modulator FIFO near half full, underruns not climbing |
| A radio hears it | Tune a receiver to 200 kHz with the loop beside it |

If the receiver hears nothing, check the header says **ON AIR**, then check
your solder joints, then scrape the enamel again. A winding that is not
electrically connected is the most common fault and it looks exactly like one
that is.

If it is distorted, lower the modulation depth, then the programme gain.

## 11. Before you transmit

Three things, none of them optional.

**This is a transmitter in a licensed band.** 200 kHz is inside the longwave
broadcast band, 2 kHz from BBC Radio 4 on 198 kHz. Deliberate radiation there
is licensed in the UK and in most other places, and this project holds no
licence. Keep the field inside the room: the loop is meant to sit beside the
receiver, not to be an antenna.

**There is no filter in this build.** The carrier is a square wave, and a
square wave radiates odd harmonics at 600 kHz, 1.0 MHz and upward, right across
the medium-wave band. The unit described here drives the loop through a single
resistor and nothing else. It works at this power with the receiver in the same
room, and it has never been measured on a spectrum analyser. If you raise the
drive, lengthen the antenna or drop the resistor, the harmonics rise with it
and you should fit the low-pass network in the Technical Manual.

**Give each transmitter its own loop.** Two carriers sharing one output network
intermodulate. This design gives them separate windings for that reason.

## 12. If it goes wrong

| Symptom | Try |
| ------- | --- |
| Nothing on the receiver | The enamel. Then the joints. Then that the header says ON AIR |
| The lid will not sit down | A winding tail trapped between a pillar and the wall, or a crooked insert |
| A screw will not start | The insert went in crooked, or the hole is full of melted plastic |
| It will not join the Wi-Fi | 2.4 GHz only. Check the password with the eye button. The console prints the real reason |
| `amtx.local` does not resolve | Some networks do not do mDNS. Use the address from the console's `status` line |
| It is set up wrong and unreachable | Hold the **B** button for five seconds while it runs. It forgets everything and comes back as new |

The serial console answers when the network does not. Connect the USB cable,
open a terminal at any baud rate, press Enter for the `amtx>` prompt, and type
`help`.
