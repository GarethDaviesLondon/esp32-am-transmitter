"""Small text helpers the views share: key names, escapes, hex rows, timestamps.

None of it knows about serial ports or widgets. It lives together because it is
all "turn this into something a person can read", and apart from the views
because both the system pane and the console pane use it.
"""

from datetime import datetime



# Keys a terminal sends as escape sequences rather than characters.
SPECIAL_KEYS = {
    "BackSpace": b"\x08",
    "Delete": b"\x1b[3~",
    "Tab": b"\t",
    "Escape": b"\x1b",
    "Up": b"\x1b[A",
    "Down": b"\x1b[B",
    "Right": b"\x1b[C",
    "Left": b"\x1b[D",
    "Home": b"\x1b[H",
    "End": b"\x1b[F",
    "Prior": b"\x1b[5~",
    "Next": b"\x1b[6~",
}

ESCAPE_MAP = {
    "n": b"\n", "r": b"\r", "t": b"\t", "0": b"\x00",
    "a": b"\x07", "b": b"\x08", "e": b"\x1b", "\\": b"\\",
}


def expand_escapes(text):
    """Expand the backslash escapes a user can type into the send box.

    Supports \\n \\r \\t \\0 \\a \\b \\e \\\\ and \\xHH. Anything else is left
    alone, so a lone backslash in a command line is transmitted as typed.
    """
    out = bytearray()
    i = 0
    while i < len(text):
        ch = text[i]
        if ch != "\\" or i + 1 >= len(text):
            out.extend(ch.encode("utf-8", errors="replace"))
            i += 1
            continue

        nxt = text[i + 1]
        if nxt == "x" and i + 3 < len(text):
            try:
                out.append(int(text[i + 2:i + 4], 16))
                i += 4
                continue
            except ValueError:
                pass
        if nxt in ESCAPE_MAP:
            out.extend(ESCAPE_MAP[nxt])
            i += 2
            continue

        out.extend(ch.encode("utf-8", errors="replace"))
        i += 1
    return bytes(out)


def hex_rows(data, offset):
    """Format bytes as classic 16-column hex dump rows. Returns (rows, consumed)."""
    rows = []
    consumed = 0
    while len(data) - consumed >= 16:
        chunk = data[consumed:consumed + 16]
        hex_part = " ".join("{:02X}".format(b) for b in chunk)
        text_part = "".join(chr(b) if 32 <= b <= 126 else "." for b in chunk)
        rows.append("{:08X}  {}  |{}|".format(offset + consumed, hex_part, text_part))
        consumed += 16
    return rows, consumed


def timestamp():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]
