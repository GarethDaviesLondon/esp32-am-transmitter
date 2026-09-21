"""A small VT100 screen over a Tk text widget.

Enough of the escape sequences for linenoise, which is what gives the board's
console its line editing, history and TAB completion. Without this the firmware
sees a terminal that cannot answer its cursor queries and drops to dumb mode.
"""

import tkinter.font as tkfont

from monitor_split import strip_ansi



ANSI_COLOURS = {
    30: "#6b7280", 31: "#ff8f8f", 32: "#8fd18f", 33: "#e5c07b",
    34: "#7fb3ff", 35: "#d19ae8", 36: "#6fd3d3", 37: "#d8dee9",
}


class TerminalView:
    """A small VT100 subset on a tk Text widget, enough for ESP-IDF's console.

    Handles CR, LF, backspace, tab, erase in line (`ESC[K`), cursor movement
    (`ESC[nA/B/C/D/G/H`), clear screen (`ESC[2J`), colour (`ESC[...m`) and
    answers the device status queries (`ESC[5n`, `ESC[6n`) that linenoise uses
    to decide whether the terminal can do line editing. Characters overwrite
    at the cursor, as on a real terminal, which is what line editing expects.
    """

    def __init__(self, widget, reply, on_line=None, max_lines=5000):
        self.text = widget
        self.reply = reply            # callable(bytes) to answer status queries
        self.on_line = on_line        # callable(str) with each finished line
        self.max_lines = max_lines
        self.state = "text"
        self.params = ""
        self.colour_tag = None
        self.font = None
        widget.mark_set("term", "end-1c")
        widget.mark_gravity("term", "right")
        for code, colour in ANSI_COLOURS.items():
            widget.tag_configure("fg{}".format(code), foreground=colour)

    # ---- geometry ----

    def cursor(self):
        row, col = self.text.index("term").split(".")
        return int(row), int(col)

    def last_row(self):
        return int(self.text.index("end-1c").split(".")[0])

    def size(self):
        """(rows, columns) the widget shows, for cursor reports and ESC[999C."""
        try:
            if self.font is None:
                self.font = tkfont.Font(font=self.text.cget("font"))
            font = self.font
            cols = max(20, self.text.winfo_width() // max(1, font.measure("0")) - 1)
            rows = max(5, self.text.winfo_height() // max(1, font.metrics("linespace")))
            return rows, cols
        except Exception:
            return 24, 120

    def screen_top(self):
        rows, _ = self.size()
        return max(1, self.last_row() - rows + 1)

    # ---- output ----

    def feed(self, text):
        run = []
        for ch in text:
            if self.state == "text":
                if ch >= " " and ch != "\x7f":
                    run.append(ch)
                    continue
                self._write("".join(run))
                run = []
                self._control(ch)
            elif self.state == "esc":
                self.state = "csi" if ch == "[" else "text"
                self.params = ""
            elif self.state == "csi":
                if "@" <= ch <= "~":
                    self._csi(self.params, ch)
                    self.state = "text"
                else:
                    self.params += ch
        self._write("".join(run))
        self._trim()
        self.text.mark_set("insert", "term")
        self.text.see("term")

    def _write(self, run):
        if not run:
            return
        line_end = self.text.index("term lineend")
        overwrite_end = self.text.index("term + {} chars".format(len(run)))
        if self.text.compare(overwrite_end, ">", line_end):
            overwrite_end = line_end
        self.text.delete("term", overwrite_end)
        self.text.insert("term", run, self.colour_tag or ())

    def _control(self, ch):
        row, col = self.cursor()
        if ch == "\r":
            self.text.mark_set("term", "term linestart")
            self._drop_padding()
        elif ch == "\n":
            if self.on_line is not None:
                self.on_line(strip_ansi(self.text.get("term linestart", "term lineend")))
            if row >= self.last_row():
                self.text.insert("end-1c", "\n")
                self.text.mark_set("term", "end-1c")
            else:
                self.text.mark_set("term", "{}.0".format(row + 1))
        elif ch == "\b":
            if col > 0:
                self.text.mark_set("term", "term - 1 chars")
        elif ch == "\t":
            self._write(" " * (8 - col % 8))
        elif ch == "\x1b":
            self.state = "esc"

    def _move_to(self, row, col):
        row = max(1, min(row, self.last_row()))
        self.text.mark_set("term", "{}.0".format(row))
        length = int(self.text.index("term lineend").split(".")[1])
        if col > length:
            self.text.mark_set("term", "term lineend")
            self.text.insert("term", " " * (col - length))
        self.text.mark_set("term", "{}.{}".format(row, col))
        self._drop_padding()

    def _drop_padding(self):
        """Remove a run of nothing but spaces right of the cursor.

        A Text widget cannot put the cursor past the end of a line, so moving
        right pads with spaces. linenoise measures the width with ESC[999C and
        moves back, which would otherwise leave a line of spaces after every
        prompt. Trailing spaces look the same as nothing, so dropping them is safe.
        """
        tail = self.text.get("term", "term lineend")
        if tail and not tail.strip(" "):
            self.text.delete("term", "term lineend")

    def _csi(self, params, final):
        numbers = [int(p) if p.isdigit() else 0 for p in params.lstrip("?").split(";")] if params else []
        n = numbers[0] if numbers and numbers[0] > 0 else 1
        row, col = self.cursor()
        _, columns = self.size()
        if final == "K":
            mode = numbers[0] if numbers else 0
            if mode == 0:
                self.text.delete("term", "term lineend")
            elif mode == 1:
                self.text.delete("term linestart", "term")
                self.text.insert("term linestart", " " * col)
            else:
                self.text.delete("term linestart", "term lineend")
                self.text.insert("term linestart", " " * col)
        elif final == "C":
            self._move_to(row, min(col + n, columns - 1))
        elif final == "D":
            self._move_to(row, max(0, col - n))
        elif final == "A":
            self._move_to(max(self.screen_top(), row - n), col)
        elif final == "B":
            self._move_to(row + n, col)
        elif final == "G":
            self._move_to(row, n - 1)
        elif final in "Hf":
            target_row = numbers[0] if numbers and numbers[0] > 0 else 1
            target_col = numbers[1] if len(numbers) > 1 and numbers[1] > 0 else 1
            self._move_to(self.screen_top() + target_row - 1, target_col - 1)
        elif final == "J" and (numbers[0] if numbers else 0) >= 2:
            self.text.delete("1.0", "end")
            self.text.mark_set("term", "1.0")
        elif final == "n":
            if numbers and numbers[0] == 5:
                self.reply(b"\x1b[0n")
            elif numbers and numbers[0] == 6:
                self.reply("\x1b[{};{}R".format(row - self.screen_top() + 1, col + 1).encode("ascii"))
        elif final == "m":
            for code in numbers or [0]:
                if code == 0 or code == 39:
                    self.colour_tag = None
                elif 30 <= code <= 37:
                    self.colour_tag = "fg{}".format(code)
                elif 90 <= code <= 97:
                    self.colour_tag = "fg{}".format(code - 60)

    def _trim(self):
        lines = self.last_row()
        if lines > self.max_lines:
            self.text.delete("1.0", "{}.0".format(lines - self.max_lines + 1))

    def clear(self):
        self.text.delete("1.0", "end")
        self.text.mark_set("term", "1.0")
