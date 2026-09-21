"""One board: the tab, its two panes, and everything typed into them.

The biggest piece of the UI, and the one esp-flasher subclasses to add
reconnecting after a flash, so it is on its own rather than buried in the main
window.
"""

import codecs
from datetime import datetime

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

from monitor_config import COMMON_BAUDS, DEFAULT_BAUD, MAX_LINES_IN_VIEW, MONO_FONT
from monitor_session import SerialSession
from monitor_split import ConsoleSplitter
from monitor_terminal import TerminalView
from monitor_text import SPECIAL_KEYS, expand_escapes, hex_rows, timestamp



LEVEL_TAGS = {"E": "sys_e", "W": "sys_w", "I": "sys_i", "D": "sys_d", "V": "sys_d", "": "sys"}

ENTER_KEYS = [("CR", b"\r"), ("LF", b"\n"), ("CRLF", b"\r\n")]
DEFAULT_ENTER_KEY = "CR"      # ESP-IDF's console reads CR as Enter (NEWLIB_STDIN_LINE_ENDING_CR)


class MonitorTab:
    """One notebook tab: a port, its session, and two views of what it says.

    The top pane holds system messages, each line prefixed `[SYS]`: the log
    output, ROM banners and crash dumps, plus the monitor's own notices. The
    bottom pane is a terminal for the board's console: click in it and type,
    PuTTY style, and every key goes to the board. Untick "Split system messages"
    to send everything to the terminal instead.
    """

    def __init__(self, app, notebook, port_info, baud):
        self.app = app
        self.notebook = notebook
        self.port_info = port_info
        self.device = port_info["device"]
        self.key = self.device

        self.session = SerialSession(self.device, baud, app.rx_queue, key=self.key)
        self.decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self.splitter = ConsoleSplitter()
        self.log_handle = None
        self.history = []
        self.history_pos = None
        self.hex_buffer = bytearray()
        self.hex_offset = 0
        self.paused = False
        self.tab_closed = False

        self.baud_var = tk.StringVar(value=str(baud))
        self.dtr_var = tk.BooleanVar(value=False)
        self.rts_var = tk.BooleanVar(value=False)
        self.timestamp_var = tk.BooleanVar(value=False)
        self.hex_var = tk.BooleanVar(value=False)
        self.split_var = tk.BooleanVar(value=True)
        self.autoscroll_var = tk.BooleanVar(value=True)
        self.echo_var = tk.BooleanVar(value=False)
        self.escapes_var = tk.BooleanVar(value=False)
        self.ending_var = tk.StringVar(value=DEFAULT_ENTER_KEY)
        self.log_path_var = tk.StringVar(value="")
        self.entry_var = tk.StringVar()
        self.status_var = tk.StringVar(value="closed")

        self.frame = tk.Frame(notebook)
        self.build_ui()
        self.frame.after(100, self.tick)

    # ---- construction ----

    def build_ui(self):
        bar = tk.Frame(self.frame)
        bar.pack(fill="x", padx=6, pady=(6, 2))

        tk.Label(bar, text="Baud:").pack(side="left")
        self.baud_box = ttk.Combobox(bar, textvariable=self.baud_var, width=9,
                                     values=[str(b) for b in COMMON_BAUDS])
        self.baud_box.pack(side="left", padx=(2, 4))
        tk.Button(bar, text="Apply", command=self.apply_baud).pack(side="left", padx=2)

        tk.Checkbutton(bar, text="DTR", variable=self.dtr_var,
                       command=lambda: self.apply_control_line("dtr")).pack(side="left", padx=(10, 2))
        tk.Checkbutton(bar, text="RTS", variable=self.rts_var,
                       command=lambda: self.apply_control_line("rts")).pack(side="left", padx=2)

        tk.Button(bar, text="Reset board", command=self.reset_board).pack(side="left", padx=(10, 2))
        tk.Button(bar, text="Send break", command=self.send_break).pack(side="left", padx=2)
        self.connect_button = tk.Button(bar, text="Close port", command=self.toggle_port)
        self.connect_button.pack(side="left", padx=(10, 2))
        tk.Button(bar, text="Close tab", command=self.close_tab).pack(side="left", padx=2)

        opts = tk.Frame(self.frame)
        opts.pack(fill="x", padx=6, pady=2)

        tk.Checkbutton(opts, text="Split system messages", variable=self.split_var,
                       command=self.on_split_toggle).pack(side="left", padx=(0, 6))
        tk.Checkbutton(opts, text="Timestamps", variable=self.timestamp_var).pack(side="left", padx=6)
        tk.Checkbutton(opts, text="Hex view", variable=self.hex_var,
                       command=self.on_hex_toggle).pack(side="left", padx=6)
        tk.Checkbutton(opts, text="Autoscroll", variable=self.autoscroll_var).pack(side="left", padx=6)
        tk.Button(opts, text="Pause", command=self.toggle_pause).pack(side="left", padx=6)
        tk.Button(opts, text="Clear", command=self.clear_view).pack(side="left", padx=2)
        tk.Button(opts, text="Save view...", command=self.save_view).pack(side="left", padx=2)

        log_row = tk.Frame(self.frame)
        log_row.pack(fill="x", padx=6, pady=2)
        tk.Label(log_row, text="Log file:").pack(side="left")
        tk.Entry(log_row, textvariable=self.log_path_var).pack(side="left", fill="x", expand=True, padx=4)
        tk.Button(log_row, text="Browse...", command=self.browse_log).pack(side="left", padx=2)
        self.log_button = tk.Button(log_row, text="Start logging", command=self.toggle_logging)
        self.log_button.pack(side="left", padx=2)

        tk.Label(self.frame, textvariable=self.status_var, anchor="w",
                 relief="sunken").pack(fill="x", side="bottom")

        panes = tk.PanedWindow(self.frame, orient="vertical", sashrelief="raised", sashwidth=6)
        panes.pack(fill="both", expand=True, padx=6, pady=4)

        system_frame = tk.Frame(panes)
        tk.Label(system_frame, text="System messages  [SYS]", anchor="w").pack(fill="x")
        self.text = scrolledtext.ScrolledText(system_frame, wrap="char", font=MONO_FONT, height=12,
                                              background="#101418", foreground="#d8dee9",
                                              insertbackground="#d8dee9")
        self.text.pack(fill="both", expand=True)
        for tag, colour in (("tx", "#7fb3ff"), ("info", "#9aa5b1"), ("error", "#ff8f8f"),
                            ("sys", "#d8dee9"), ("sys_e", "#ff8f8f"), ("sys_w", "#e5c07b"),
                            ("sys_i", "#a9d8a9"), ("sys_d", "#8a94a0")):
            self.text.tag_configure(tag, foreground=colour)
        self.text.bind("<Key>", self.on_system_key)
        panes.add(system_frame, stretch="always", minsize=80)

        console_frame = tk.Frame(panes)
        header = tk.Frame(console_frame)
        header.pack(fill="x")
        tk.Label(header, text="Console: click here and type; keys go straight to the board",
                 anchor="w").pack(side="left")
        ttk.Combobox(header, textvariable=self.ending_var, width=6, state="readonly",
                     values=[name for name, _ in ENTER_KEYS]).pack(side="right")
        tk.Label(header, text="Enter sends:").pack(side="right", padx=4)

        self.console = scrolledtext.ScrolledText(console_frame, wrap="char", font=MONO_FONT, height=14,
                                                 background="#0b0f12", foreground="#d8dee9",
                                                 insertbackground="#f0f0f0", insertwidth=2)
        self.console.pack(fill="both", expand=True)
        self.console.tag_configure("tx", foreground="#7fb3ff")
        self.console.bind("<Key>", self.on_console_key)
        self.console.bind("<<Paste>>", lambda e: self.paste_to_board())
        self.console.bind("<Button-2>", lambda e: "break")
        self.console.bind("<ButtonRelease-1>", lambda e: self.console.after_idle(self.restore_cursor))
        self.terminal = TerminalView(self.console, self.reply_to_board, self.on_console_line,
                                     max_lines=MAX_LINES_IN_VIEW)
        panes.add(console_frame, stretch="always", minsize=80)

        send_row = tk.Frame(console_frame)
        send_row.pack(fill="x", pady=(4, 0))
        tk.Label(send_row, text="Send line:").pack(side="left")
        self.entry = tk.Entry(send_row, textvariable=self.entry_var, font=MONO_FONT)
        self.entry.pack(side="left", fill="x", expand=True, padx=4)
        self.entry.bind("<Return>", lambda e: self.send_line())
        self.entry.bind("<Up>", self.history_back)
        self.entry.bind("<Down>", self.history_forward)
        tk.Checkbutton(send_row, text="Escapes", variable=self.escapes_var).pack(side="left", padx=2)
        tk.Checkbutton(send_row, text="Local echo", variable=self.echo_var).pack(side="left", padx=2)
        tk.Button(send_row, text="Send", command=self.send_line).pack(side="left", padx=2)

    # ---- port lifecycle ----

    def open_port(self):
        try:
            self.session.open()
        except Exception as exc:
            self.emit("Could not open {}: {}\n".format(self.device, exc), "error")
            self.update_status()
            return False
        self.dtr_var.set(False)
        self.rts_var.set(False)
        self.emit("--- {} open at {} baud ---\n".format(self.device, self.session.baud), "info")
        self.connect_button.config(text="Close port")
        self.update_status()
        self.app.refresh_port_states()
        return True

    def toggle_port(self):
        if self.session.is_open:
            self.close_port()
        else:
            self.session = SerialSession(self.device, self.current_baud(), self.app.rx_queue,
                                         key=self.key)
            self.open_port()

    def close_port(self):
        self.session.close()
        self.connect_button.config(text="Reopen port")
        self.update_status()
        self.app.refresh_port_states()

    def close_tab(self):
        self.tab_closed = True
        self.close_port()
        self.stop_logging()
        self.app.remove_tab(self.key)

    def current_baud(self):
        try:
            return int(self.baud_var.get())
        except ValueError:
            return DEFAULT_BAUD

    def apply_baud(self):
        baud = self.current_baud()
        try:
            self.session.set_baud(baud)
        except Exception as exc:
            self.emit("Could not set baud: {}\n".format(exc), "error")
            return
        self.emit("--- baud set to {} ---\n".format(baud), "info")
        self.update_status()

    def apply_control_line(self, name):
        value = self.dtr_var.get() if name == "dtr" else self.rts_var.get()
        try:
            self.session.set_control_line(name, value)
        except Exception as exc:
            self.emit("Could not set {}: {}\n".format(name.upper(), exc), "error")

    def reset_board(self):
        try:
            self.session.reset_board()
        except Exception as exc:
            self.emit("Reset failed: {}\n".format(exc), "error")
            return
        self.dtr_var.set(False)
        self.rts_var.set(False)
        self.emit("--- reset pulse sent ---\n", "info")

    def send_break(self):
        try:
            self.session.send_break()
        except Exception as exc:
            self.emit("Break failed: {}\n".format(exc), "error")
            return
        self.emit("--- break sent ---\n", "info")

    # ---- incoming data ----

    def handle_event(self, kind, payload):
        if kind == "data":
            if not self.paused:
                self.append_bytes(payload)
            self.update_status()
        elif kind == "error":
            self.emit("{}\n".format(payload), "error")
        elif kind == "closed":
            self.connect_button.config(text="Reopen port")
            self.emit("--- {} closed ---\n".format(self.device), "info")
            self.update_status()
            self.app.refresh_port_states()

    def append_bytes(self, data):
        if self.hex_var.get():
            self.hex_buffer.extend(data)
            rows, consumed = hex_rows(bytes(self.hex_buffer), self.hex_offset)
            if consumed:
                del self.hex_buffer[:consumed]
                self.hex_offset += consumed
                self.emit("\n".join(rows) + "\n", None)
            return

        text = self.decoder.decode(data)
        if not text:
            return
        if not self.split_var.get():
            self.terminal.feed(text)
            return
        self.route(self.splitter.feed(text))

    def tick(self):
        """Release text the splitter has held back, a few times a second."""
        if self.tab_closed or not self.frame.winfo_exists():
            return
        if self.split_var.get() and not self.hex_var.get():
            self.route(self.splitter.flush_stale())
        self.frame.after(100, self.tick)

    def route(self, pieces):
        for piece in pieces:
            if piece[0] == "cli":
                self.terminal.feed(piece[1])
            else:
                _, line, level = piece
                self.emit_system(line, level)

    def emit_system(self, line, level):
        stamp = "[{}] ".format(timestamp()) if self.timestamp_var.get() else ""
        self.emit("{}[SYS] {}\n".format(stamp, line), LEVEL_TAGS.get(level, "sys"))

    def emit(self, text, tag):
        """Append to the system pane (and the log file). The monitor's own notices come here too."""
        self.text.insert("end", text, tag or ())
        self.trim_view()
        if self.autoscroll_var.get():
            self.text.see("end")
        self.write_log(text)

    def on_console_line(self, line):
        self.write_log("[CLI] {}\n".format(line.rstrip()))

    def write_log(self, text):
        if self.log_handle is None:
            return
        try:
            self.log_handle.write(text)
            self.log_handle.flush()
        except Exception:
            self.stop_logging()

    def trim_view(self):
        lines = int(self.text.index("end-1c").split(".")[0])
        if lines > MAX_LINES_IN_VIEW:
            self.text.delete("1.0", "{}.0".format(lines - MAX_LINES_IN_VIEW + 1))

    # ---- outgoing data ----

    def enter_key(self):
        for name, value in ENTER_KEYS:
            if name == self.ending_var.get():
                return value
        return b"\r"

    def send_line(self):
        text = self.entry_var.get()
        body = expand_escapes(text) if self.escapes_var.get() else text.encode("utf-8", errors="replace")
        if not self.transmit(body + self.enter_key()):
            return
        if self.echo_var.get():
            self.console.insert("term", text + "\n", "tx")
        if text and (not self.history or self.history[-1] != text):
            self.history.append(text)
        self.history_pos = None
        self.entry_var.set("")

    def transmit(self, payload):
        try:
            self.session.write(payload)
        except Exception as exc:
            self.emit("Send failed: {}\n".format(exc), "error")
            return False
        self.update_status()
        return True

    def reply_to_board(self, payload):
        """The terminal answering a status query; silent if the port is closed."""
        if self.session.is_open:
            self.transmit(payload)

    def on_console_key(self, event):
        """Keystrokes in the console pane go to the board, PuTTY style.

        Ctrl+C copies when text is selected and sends 0x03 otherwise; Ctrl+V
        pastes the clipboard to the board. Nothing is typed into the pane
        locally: what appears is the board's echo.
        """
        ctrl = bool(event.state & 0x0004)
        keysym = event.keysym

        if ctrl and keysym.lower() == "c" and self.console.tag_ranges("sel"):
            self.console.event_generate("<<Copy>>")
            return "break"
        if ctrl and keysym.lower() == "v":
            return self.paste_to_board()
        if keysym in ("Shift_L", "Shift_R", "Control_L", "Control_R", "Alt_L", "Alt_R", "Caps_Lock"):
            return "break"

        payload = None
        if keysym in ("Return", "KP_Enter"):
            payload = self.enter_key()
        elif keysym in SPECIAL_KEYS:
            payload = SPECIAL_KEYS[keysym]
        elif ctrl and len(keysym) == 1 and keysym.isalpha():
            payload = bytes([ord(keysym.upper()) - 64])
        elif event.char and event.char.isprintable():
            payload = event.char.encode("utf-8", errors="replace")

        if payload:
            self.transmit(payload)
        return "break"

    def paste_to_board(self):
        try:
            text = self.console.clipboard_get()
        except tk.TclError:
            return "break"
        text = text.replace("\r\n", "\n").replace("\n", self.enter_key().decode("ascii"))
        self.transmit(text.encode("utf-8", errors="replace"))
        return "break"

    def restore_cursor(self):
        self.console.mark_set("insert", "term")

    def on_system_key(self, event):
        """The system pane is read-only; copy and navigation keys still work."""
        ctrl = bool(event.state & 0x0004)
        if ctrl and event.keysym.lower() in ("c", "a", "insert"):
            return None
        if event.keysym in ("Up", "Down", "Left", "Right", "Prior", "Next", "Home", "End"):
            return None
        return "break"

    def history_back(self, _event):
        if not self.history:
            return "break"
        self.history_pos = len(self.history) - 1 if self.history_pos is None else max(0, self.history_pos - 1)
        self.entry_var.set(self.history[self.history_pos])
        self.entry.icursor("end")
        return "break"

    def history_forward(self, _event):
        if self.history_pos is None:
            return "break"
        self.history_pos += 1
        if self.history_pos >= len(self.history):
            self.history_pos = None
            self.entry_var.set("")
        else:
            self.entry_var.set(self.history[self.history_pos])
        self.entry.icursor("end")
        return "break"

    # ---- view options ----

    def on_hex_toggle(self):
        if not self.hex_var.get() and self.hex_buffer:
            rows, _ = hex_rows(bytes(self.hex_buffer) + b"\x00" * (16 - len(self.hex_buffer)), self.hex_offset)
            self.emit("\n".join(rows) + "  (partial row flushed)\n", "info")
            self.hex_offset += len(self.hex_buffer)
            self.hex_buffer.clear()
        self.emit("--- {} view{} ---\n".format(
            "hex" if self.hex_var.get() else "text",
            ": all bytes shown here, the console pane is idle" if self.hex_var.get() else ""), "info")

    def on_split_toggle(self):
        if not self.split_var.get():
            self.route(self.splitter.flush_stale(now=float("inf")))
        self.emit("--- system messages {} ---\n".format(
            "split into this pane" if self.split_var.get() else "left in the console"), "info")

    def toggle_pause(self):
        self.paused = not self.paused
        self.emit("--- display {} (the port is still being read) ---\n".format(
            "paused" if self.paused else "resumed"), "info")

    def clear_view(self):
        self.text.delete("1.0", "end")
        self.terminal.clear()

    def save_view(self):
        path = filedialog.asksaveasfilename(
            title="Save monitor output",
            defaultextension=".log",
            initialfile="{}.log".format(self.device.replace("/", "_").replace(":", "")),
            filetypes=[("Log files", "*.log"), ("Text files", "*.txt"), ("All files", "*.*")],
        )
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("=== System messages ===\n")
                handle.write(self.text.get("1.0", "end-1c"))
                handle.write("\n=== Console ===\n")
                handle.write(self.console.get("1.0", "end-1c"))
        except Exception as exc:
            messagebox.showerror("Save failed", str(exc))
            return
        self.emit("--- view saved to {} ---\n".format(path), "info")

    def browse_log(self):
        path = filedialog.asksaveasfilename(
            title="Log file", defaultextension=".log",
            initialfile="{}.log".format(self.device.replace("/", "_").replace(":", "")),
            filetypes=[("Log files", "*.log"), ("All files", "*.*")],
        )
        if path:
            self.log_path_var.set(path)

    def toggle_logging(self):
        if self.log_handle is not None:
            self.stop_logging()
            return
        path = self.log_path_var.get().strip()
        if not path:
            self.browse_log()
            path = self.log_path_var.get().strip()
        if not path:
            return
        try:
            self.log_handle = open(path, "a", encoding="utf-8")
        except Exception as exc:
            messagebox.showerror("Cannot open log file", str(exc))
            return
        self.log_handle.write("\n--- log opened {} on {} ---\n".format(
            datetime.now().isoformat(timespec="seconds"), self.device))
        self.log_button.config(text="Stop logging")
        self.emit("--- logging to {} ([SYS] system lines, [CLI] console lines) ---\n".format(path), "info")

    def stop_logging(self):
        if self.log_handle is None:
            return
        try:
            self.log_handle.close()
        except Exception:
            pass
        self.log_handle = None
        self.log_button.config(text="Start logging")

    def update_status(self):
        self.status_var.set("{}  |  {} baud  |  {}  |  rx {} bytes  tx {} bytes{}".format(
            self.device,
            self.session.baud,
            "open" if self.session.is_open else "closed",
            self.session.rx_bytes,
            self.session.tx_bytes,
            "  |  display paused" if self.paused else "",
        ))
