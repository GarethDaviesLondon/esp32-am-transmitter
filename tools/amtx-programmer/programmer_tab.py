"""The Program tab: connecting to a board, and the machinery the panels use.

Everything here is what is not a panel: the worker thread that owns the serial
console, connecting and reconnecting (including handing the port to the flasher
and taking it back when it has finished), the log and the status line, and the
refresh that fetches one state document and hands it to each panel to render.

The panels are the four mixins, one file each, and this class is what gives
them `act`, `submit` and the console to talk to.
"""

import queue
import threading
import time

import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext

import amtx_console as ac

from programmer_config import (
    AUTO_REFRESH_SECONDS,
    LEGACY_COMMANDS,
    MONO_FONT,
    RECONNECT_RETRY_SECONDS,
    RECONNECT_TIMEOUT,
    TX_NAMES,
    flasher,
    monitor,
)
from programmer_discover import DiscoverPanel
from programmer_network import NetworkPanel
from programmer_stations import StationsPanel
from programmer_transmitters import TransmittersPanel


class ProgramTab(TransmittersPanel, StationsPanel, DiscoverPanel, NetworkPanel):
    """Buttons and dialogs that drive the board's console, one command at a time.

    Commands run on a worker thread, in order. Replies and console traffic come
    back to the Tk thread through `self.results`, drained on a timer. The tab
    holds the board's port while connected, so it closes any monitor tab on the
    same board first, and lets go while the flasher probes or flashes.
    """

    def __init__(self, app, notebook, sim_board=None):
        self.app = app
        self.root = app.root
        self.sim_board = sim_board
        self.results = queue.Queue()
        self.jobs = queue.Queue()
        self.console = ac.AmtxConsole(on_event=self.on_console_event)
        self.board = ac.AmtxBoard(self.console, on_reply=lambda r: self.results.put(("reply", r)))
        self.key = "simulated board" if sim_board else None
        self.want_connected = False
        self.connecting = False
        self.last_attempt = 0.0
        self.last_refresh = 0.0
        self.connect_cancel = threading.Event()
        self.job_running = False
        self.doc = None
        self.discover_hits = []
        self.wifi_nets = []
        self.rf_dirty = [False, False]
        self.rf_loading = False
        self.closed = False

        self.board_var = tk.StringVar(value="Not connected. Select a board above, then press Connect.")
        self.status_var = tk.StringVar(value="Ready")
        self.auto_refresh_var = tk.BooleanVar(value=False)

        self.frame = tk.Frame(notebook)
        self.build_ui()

        self.worker = threading.Thread(target=self.work, daemon=True)
        self.worker.start()
        self.root.after(80, self.pump)

    # ---- construction ----

    def build_ui(self):
        bar = tk.Frame(self.frame)
        bar.pack(fill="x", padx=6, pady=(6, 2))
        self.connect_button = tk.Button(bar, text="Connect", width=11, bg="#dff0d8", command=self.connect)
        self.connect_button.pack(side="left")
        tk.Button(bar, text="Disconnect", command=self.disconnect).pack(side="left", padx=4)
        tk.Button(bar, text="Refresh", command=lambda: self.refresh(explicit=True)).pack(side="left", padx=4)
        tk.Checkbutton(bar, text="Refresh every {:.0f} s".format(AUTO_REFRESH_SECONDS),
                       variable=self.auto_refresh_var).pack(side="left", padx=4)
        tk.Button(bar, text="Reboot board...", command=self.reboot).pack(side="left", padx=(16, 4))
        tk.Label(bar, textvariable=self.board_var, anchor="w", font=MONO_FONT).pack(side="left", padx=10,
                                                                                   fill="x", expand=True)

        self.status_label = tk.Label(self.frame, textvariable=self.status_var, anchor="w", relief="sunken")
        self.status_label.pack(fill="x", side="bottom", padx=6, pady=(0, 4))

        self.inner = ttk.Notebook(self.frame)
        self.inner.pack(fill="both", expand=True, padx=6, pady=4)
        self.build_transmitters()
        self.build_stations()
        self.build_discover()
        self.build_network()
        self.build_console_log()

    def build_console_log(self):
        page = tk.Frame(self.inner)
        self.inner.add(page, text="Console log")
        self.log_text = scrolledtext.ScrolledText(page, wrap="char", font=MONO_FONT,
                                                  background="#101418", foreground="#d8dee9")
        self.log_text.pack(fill="both", expand=True, padx=4, pady=4)
        for tag, colour in (("sent", "#7fb3ff"), ("ok", "#8fd18f"), ("error", "#ff8f8f"), ("sys", "#8a94a0"),
                            ("info", "#e5c07b")):
            self.log_text.tag_configure(tag, foreground=colour)
        self.log_text.bind("<Key>", lambda e: None if (e.state & 0x4) else "break")
        row = tk.Frame(page)
        row.pack(fill="x", padx=4, pady=(0, 6))
        tk.Label(row, text="Send a command:").pack(side="left")
        self.raw_var = tk.StringVar()
        entry = tk.Entry(row, textvariable=self.raw_var, font=MONO_FONT)
        entry.pack(side="left", fill="x", expand=True, padx=4)
        entry.bind("<Return>", lambda e: self.send_raw())
        tk.Button(row, text="Send", command=self.send_raw).pack(side="left")
        tk.Button(row, text="Clear", command=lambda: self.log_text.delete("1.0", "end")).pack(side="left", padx=4)

    # ---- output ----

    def log(self, text, tag=None):
        self.log_text.insert("end", text + "\n", tag or ())
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > monitor.MAX_LINES_IN_VIEW:
            self.log_text.delete("1.0", "{}.0".format(lines - monitor.MAX_LINES_IN_VIEW))
        self.log_text.see("end")

    def set_status(self, text, bad=False):
        self.status_var.set(text)
        self.status_label.config(fg="#b00020" if bad else "#1b5e20")

    # ---- worker ----

    def work(self):
        while True:
            label, fn, on_done, explicit = self.jobs.get()
            if label is None:
                return
            self.job_running = True
            self.results.put(("busy", label))
            try:
                value = fn(self.board)
                self.results.put(("done", label, on_done, value, None, explicit))
            except ac.NotConnected:
                self.results.put(("done", label, None, None, "not connected: press Connect first", explicit))
            except ValueError as exc:
                self.results.put(("done", label, None, None, str(exc), explicit))
            except Exception as exc:
                self.results.put(("done", label, None, None, "{}: {}".format(type(exc).__name__, exc), explicit))
            finally:
                self.job_running = False

    def submit(self, label, fn, on_done=None, explicit=True):
        self.jobs.put((label, fn, on_done, explicit))

    def act(self, label, fn, refresh=True):
        """Run an action whose reply is the result; refresh the state after it."""
        def done(reply):
            if refresh:
                self.refresh(explicit=False)
        self.submit(label, fn, done)

    def on_console_event(self, kind, text):
        self.results.put(("console", kind, text))

    def pump(self):
        if self.closed:
            return
        try:
            for _ in range(500):
                item = self.results.get_nowait()
                kind = item[0]
                if kind == "console":
                    self.on_console(item[1], item[2])
                elif kind == "reply":
                    self.on_reply(item[1])
                elif kind == "busy":
                    self.set_status("Working: {}...".format(item[1]))
                elif kind == "done":
                    self.on_done(*item[1:])
                elif kind == "connected":
                    self.on_connected(item[1], item[2], item[3])
        except queue.Empty:
            pass
        if self.auto_refresh_var.get() and self.console.connected and self.jobs.empty() \
                and not self.job_running and time.monotonic() - self.last_refresh > AUTO_REFRESH_SECONDS:
            self.refresh(explicit=False)
        if self.sim_board is not None:
            self.maybe_reconnect()
        self.root.after(80, self.pump)

    def on_console(self, kind, text):
        if kind == "log":
            self.log("[SYS] " + text, "sys")
        elif kind == "sent":
            self.log("> " + text, "sent")
        elif kind == "disconnected":
            self.log("--- the board went away (reset or unplugged) ---", "info")
            self.update_board_label()
            if self.want_connected:
                self.set_status("The board disconnected; reconnecting when it comes back...", bad=True)

    def on_reply(self, reply):
        for line in reply.lines:
            self.log("  " + line)
        bad = reply.ok is False or not reply.complete
        self.log("  " + reply.summary(), "error" if bad else "ok")
        self.set_status(reply.summary(), bad=bad)

    def on_done(self, label, on_done, value, problem, explicit):
        if problem:
            self.log("{}: {}".format(label, problem), "error")
            self.set_status("{}: {}".format(label, problem), bad=True)
            if explicit:
                messagebox.showwarning(label, problem, parent=self.frame)
            return
        reply = value[0] if isinstance(value, tuple) else value
        if explicit and isinstance(reply, ac.Reply) and (reply.ok is False or not reply.complete):
            messagebox.showwarning(label, reply.summary(), parent=self.frame)
        if on_done is not None:
            on_done(value)

    # ---- connection ----

    def open_transport(self):
        if self.sim_board is not None:
            return self.sim_board.open()
        port = flasher.find_port(self.key)
        if port is None:
            raise ac.TransportClosed("{} is not on the bus".format(self.key))
        return ac.SerialTransport(port["device"])

    def connect(self):
        if self.sim_board is None:
            key = self.app.selected_key()
            if key is None:
                messagebox.showinfo("No board", "Select a board in the list at the top first.", parent=self.frame)
                return
            if self.app.busy:
                messagebox.showinfo("Busy", "Wait for the flasher to finish first.", parent=self.frame)
                return
            if self.console.connected and key != self.key:
                self.console.detach()
            self.key = key
            self.app.hand_port_to_program(key)
        self.want_connected = True
        self.start_connect()

    def start_connect(self):
        if self.connecting or self.console.connected:
            return
        self.connecting = True
        self.last_attempt = time.monotonic()
        self.set_status("Connecting to {}...".format(self.key))
        cancel = self.connect_cancel = threading.Event()

        def run():
            ok = ac.attach_when_ready(self.console, self.open_transport, timeout=RECONNECT_TIMEOUT, cancel=cancel)
            if cancel.is_set() and ok:
                self.console.detach()
                ok = False
            self.results.put(("connected", ok, self.key, cancel.is_set()))

        threading.Thread(target=run, daemon=True).start()

    def on_connected(self, ok, key, cancelled):
        self.connecting = False
        self.update_board_label()
        if cancelled:
            return          # the flasher took the port; on_ports tries again when it is done
        if not ok:
            # Stop retrying: a board that gave no prompt for this long is not
            # running amtx, or something else holds its port.
            self.want_connected = False
            self.update_board_label()
            self.set_status("No amtx> prompt from {} within {:.0f} s. Is the amtx firmware running, and is the "
                            "port free? Press Connect to try again.".format(key, RECONNECT_TIMEOUT), bad=True)
            self.log("--- no prompt from {} ---".format(key), "error")
            return
        if not self.want_connected:
            self.console.detach()
            return
        self.log("--- connected to {} ---".format(key), "info")
        self.set_status("Connected to {}".format(key))
        self.refresh(explicit=False)

    def disconnect(self):
        self.want_connected = False
        self.console.detach()
        self.update_board_label()
        self.set_status("Disconnected")
        self.log("--- disconnected ---", "info")

    def release_for_flasher(self, key):
        """The flasher needs the port. Let go, but come back afterwards."""
        if key != self.key:
            return
        if self.connecting:
            self.connect_cancel.set()
        if self.console.connected:
            self.console.detach()
            self.log("--- port released for the flasher; reconnecting afterwards ---", "info")
            self.update_board_label()

    def yield_port(self, key):
        """A monitor tab wants this board: step aside and stay away."""
        if key == self.key and (self.console.connected or self.want_connected):
            self.want_connected = False
            self.console.detach()
            self.log("--- port handed to a monitor tab; press Connect to take it back ---", "info")
            self.update_board_label()

    def wants(self, key):
        return self.want_connected and key == self.key

    def maybe_reconnect(self, ports=None):
        if not self.want_connected or self.console.connected or self.connecting or self.app.busy:
            return
        if time.monotonic() - self.last_attempt < RECONNECT_RETRY_SECONDS:
            return
        if self.sim_board is None and flasher.find_port(self.key, ports) is None:
            return
        self.start_connect()

    def on_ports(self, ports):
        self.maybe_reconnect(ports)

    def update_board_label(self):
        if self.console.connected:
            parts = ["Connected: {}".format(self.key)]
            if self.doc:
                sys_doc, net = self.doc.get("sys", {}), self.doc.get("net", {})
                parts.append("{}.local".format(sys_doc.get("host", "?")))
                parts.append("{} {}".format(net.get("state", "?"), net.get("ip", "")))
                parts.append("firmware {}".format(sys_doc.get("version", "?")))
            self.board_var.set("   ".join(parts))
        elif self.want_connected:
            self.board_var.set("Waiting for {} to come back...".format(self.key))
        else:
            self.board_var.set("Not connected.")

    def shutdown(self):
        self.closed = True
        self.want_connected = False
        self.jobs.put((None, None, None, False))
        self.console.detach()

    # ---- state ----

    def refresh(self, explicit=True):
        self.last_refresh = time.monotonic()
        self.submit("Refresh", lambda b: b.state(), self.on_state, explicit)

    def on_state(self, value):
        reply, doc = value
        if doc is None:
            return
        self.doc = doc
        self.update_board_label()
        self.render_transmitters(doc)
        self.render_stations(doc)
        self.render_network(doc)
