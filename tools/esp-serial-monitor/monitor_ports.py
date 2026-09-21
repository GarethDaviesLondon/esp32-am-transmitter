"""What ports exist, which look like an ESP board, and how to open one.

The knowledge here is about USB adapters and pyserial, and nothing else needs
it: baud detection, the session and the UI all come through these functions
rather than touching `serial` themselves.
"""

import time

from monitor_config import ESPRESSIF_VID, KNOWN_ADAPTERS, list_ports, serial



def describe_port(info):
    """Turn a pyserial ListPortInfo into the flat record the rest of the tool uses."""
    vid, pid = info.vid, info.pid
    description = info.description or ""
    adapter = ""
    likely_esp = False

    if vid is not None and pid is not None:
        adapter = KNOWN_ADAPTERS.get((vid, pid), "")
        if adapter:
            likely_esp = True
        elif vid == ESPRESSIF_VID:
            adapter = "Espressif (unrecognised PID)"
            likely_esp = True

    if "bluetooth" in description.lower():
        # A Bluetooth virtual COM port is never a plugged-in board; saying so
        # saves the user probing it for twenty seconds.
        adapter = adapter or "Bluetooth virtual port"
        likely_esp = False

    return {
        "device": info.device,
        "description": description,
        "hwid": info.hwid or "",
        "vid": vid,
        "pid": pid,
        "serial_number": info.serial_number or "",
        "adapter": adapter or "unknown adapter",
        "likely_esp": likely_esp,
        "native_usb": vid == ESPRESSIF_VID,
    }


def list_serial_ports(esp_only=False):
    """Every serial port on the machine, likely ESP boards first."""
    if list_ports is None:
        return []
    ports = [describe_port(p) for p in list_ports.comports()]
    ports.sort(key=lambda p: (not p["likely_esp"], p["device"]))
    if esp_only:
        ports = [p for p in ports if p["likely_esp"]]
    return ports


def open_port(device, baud, dtr=False, rts=False, timeout=0.05, write_timeout=2.0):
    """Open a port with DTR/RTS set *before* the open.

    On the usual auto-reset wiring DTR drives IO0 and RTS drives EN, so opening
    a port with them asserted can hold the board in reset or drop it into the
    ROM bootloader. pyserial applies the states at open time when they are set
    on an unopened instance, which avoids that glitch.
    """
    ser = serial.Serial()
    ser.port = device
    ser.baudrate = baud
    ser.timeout = timeout
    ser.write_timeout = write_timeout
    ser.dtr = dtr
    ser.rts = rts
    ser.open()
    return ser


def pulse_reset(ser, hold=0.1, settle=0.05):
    """Reset the board into its application using the standard auto-reset circuit.

    IO0 (DTR) is held high so the chip runs the firmware rather than entering the
    ROM bootloader; EN (RTS) is pulsed low to restart it. Boards without the
    auto-reset transistors simply ignore this.
    """
    ser.dtr = False   # IO0 high: boot the application
    ser.rts = True    # EN low: hold in reset
    time.sleep(hold)
    ser.rts = False   # EN high: release
    time.sleep(settle)


def print_ports(esp_only=False):
    ports = list_serial_ports(esp_only=esp_only)
    if not ports:
        print("No serial ports found." if not esp_only else "No likely ESP boards found.")
        return
    print("{:<10} {:<34} {:<7} {}".format("PORT", "ADAPTER", "ESP?", "DESCRIPTION"))
    for port in ports:
        print("{:<10} {:<34} {:<7} {}".format(
            port["device"], port["adapter"][:34],
            "yes" if port["likely_esp"] else "-", port["description"]))
        if port["hwid"]:
            print("{:<10} {}".format("", port["hwid"]))
