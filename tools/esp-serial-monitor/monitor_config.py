"""Constants, and the one place that knows whether pyserial is installed.

Separate so that any other module can ask for a default baud rate or the
VID/PID table without dragging tkinter in with it, and so the pyserial import
is attempted exactly once. `serial` and `list_ports` are None when it is not
installed; `main()` reports that properly rather than every call site guessing.

Module names in this tool are prefixed `monitor_` on purpose. These tools are
plain scripts on `sys.path`, not packages, and esp-flasher and amtx-programmer
put their own directory and this one on the path together: two modules called
`config` would be one import away from shadowing each other.
"""

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # reported properly in main(); nothing here works without it
    serial = None
    list_ports = None



# USB VID/PID pairs for the USB-to-serial bridges ESP dev boards ship with, plus
# the native USB peripheral on the S2/S3/C3 parts. Used only to sort likely
# boards to the top of the list; an unlisted adapter still works.
KNOWN_ADAPTERS = {
    (0x303A, 0x1001): "Espressif native USB (JTAG/serial)",
    (0x303A, 0x0002): "Espressif native USB (CDC)",
    (0x303A, 0x4001): "Espressif native USB (CDC)",
    (0x10C4, 0xEA60): "Silicon Labs CP210x",
    (0x10C4, 0xEA70): "Silicon Labs CP2105",
    (0x10C4, 0xEA71): "Silicon Labs CP2108",
    (0x1A86, 0x7522): "WCH CH340",
    (0x1A86, 0x7523): "WCH CH340",
    (0x1A86, 0x5523): "WCH CH341",
    (0x1A86, 0x55D4): "WCH CH9102",
    (0x0403, 0x6001): "FTDI FT232R",
    (0x0403, 0x6010): "FTDI FT2232",
    (0x0403, 0x6014): "FTDI FT232H",
    (0x0403, 0x6015): "FTDI FT-X",
}

ESPRESSIF_VID = 0x303A

# Probe order: most likely first, so a confident early hit ends the sweep fast.
# 74880 is the ESP8266 ROM bootloader rate; keep it high in the list.
DEFAULT_PROBE_BAUDS = [
    115200, 74880, 9600, 921600, 460800, 230400,
    57600, 38400, 19200, 250000, 1000000, 2000000,
]

# Rates offered in the monitor baud box.
COMMON_BAUDS = [
    9600, 19200, 38400, 57600, 74880, 115200, 230400,
    250000, 460800, 921600, 1000000, 2000000,
]

DEFAULT_BAUD = 115200
DEFAULT_LISTEN_SECONDS = 1.5
CONFIDENT_SCORE = 0.95          # stop probing once a rate scores this well
CONFIDENT_BYTES = 40            # ...and produced at least this much output
MAX_LINES_IN_VIEW = 5000        # per tab; older lines are trimmed

LINE_ENDINGS = [
    ("CRLF (\\r\\n)", b"\r\n"),
    ("LF (\\n)", b"\n"),
    ("CR (\\r)", b"\r"),
    ("None", b""),
]
DEFAULT_LINE_ENDING = "CRLF (\\r\\n)"

MONO_FONT = ("Consolas", 10)    # tk falls back to a default font where absent
