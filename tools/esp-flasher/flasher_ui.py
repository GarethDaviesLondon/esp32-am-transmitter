"""The window: the board list, the build list, the log, and the monitor tab.

Tk plumbing and the decisions a person makes with it. Everything it needs to
know about ports, images, probing and esptool comes from the modules beside it,
so this file stays about layout, threads and what a button does.

`FlasherMonitorTab` subclasses the monitor's tab to reconnect through a reset,
which is what makes "flash, then watch it boot" one action instead of two.
"""

import contextlib
import queue
import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

from flasher_bus import BusWatcher, find_port, port_key
from flasher_config import (
    Cancelled,
    DEFAULT_APP_OFFSET,
    DEFAULT_FLASH_BAUD,
    DEFAULT_PARTITION_TABLE_OFFSET,
    FLASH_BAUDS,
    LOOP_WINDOW_SECONDS,
    MONO_FONT,
    PORT_POLL_SECONDS,
    REPO_ROOT,
    esptool,
    monitor,
)
from flasher_esptool import (
    TextSink,
    first_line,
    flash_board,
    select_images,
    wait_for_port,
)
from flasher_firmware import Firmware, discover_builds
from flasher_images import human_size
from flasher_probe import assess, probe_board


# =========================

class FlasherMonitorTab(monitor.MonitorTab):
    """A monitor tab that follows the board through resets.

    The S3's native USB port disappears on every reset. The stock tab stops
    there; this one notices its reader died, releases the handle, and lets the
    app reopen it when the board comes back, unless you closed it yourself.
    """

    def __init__(self, app, notebook, port_info, baud):
        super().__init__(app, notebook, port_info, baud)
        self.board_key = port_key(port_info)
        self.user_closed = False
        self.last_attempt = 0.0

    def close_port(self):
        self.user_closed = True
        super().close_port()

    def release_port(self):
        """Close for a flash or probe, without counting as the operator closing it."""
        self.session.close()
        self.connect_button.config(text="Reopen port")
        self.update_status()

    def toggle_port(self):
        if not self.session.is_open:
            self.user_closed = False
        super().toggle_port()

    def reopen_quietly(self, device):
        self.device = device
        self.session = monitor.SerialSession(device, self.current_baud(), self.app.rx_queue, key=self.key)
        try:
            self.session.open()
        except Exception:
            return False
        self.emit("--- {} back on the bus, reconnected ---\n".format(device), "info")
        self.connect_button.config(text="Close port")
        self.update_status()
        return True

    def handle_event(self, kind, payload):
        if kind == "closed":
            session = self.session
            if session.thread is not None and session.thread.is_alive():
                return  # a late event from the session before this one
            if session.serial is not None and session.serial.is_open:
                session.close()  # the reader died on its own: board reset or unplugged
        super().handle_event(kind, payload)


# =========================

class FlasherApp:
    """Board list, firmware picker and flash controls on top; log and monitors below."""

    def __init__(self, root):
        self.root = root
        self.root.title("ESP Flasher")
        self.root.geometry("1180x900")
        self.root.minsize(960, 700)

        self.rx_queue = queue.Queue()     # monitor tabs
        self.events = queue.Queue()       # worker threads
        self.tabs = {}
        self.watcher = BusWatcher()
        self.ports = []
        self.firmware = None
        self.build_paths = []
        self.probes = {}                  # board key -> probe result
        self.busy = False
        self.cancel = threading.Event()
        self.proc_holder = {"proc": None}
        self.pending_flash = None
        self.log_cr = False

        self.show_all_var = tk.BooleanVar(value=False)
        self.reconnect_var = tk.BooleanVar(value=True)
        self.build_var = tk.StringVar()
        self.offset_var = tk.StringVar(value="0x{:x}".format(DEFAULT_APP_OFFSET))
        self.mode_var = tk.StringVar(value="auto")
        self.erase_var = tk.BooleanVar(value=False)
        self.baud_var = tk.StringVar(value=str(DEFAULT_FLASH_BAUD))
        self.monitor_after_var = tk.BooleanVar(value=True)
        self.board_info_var = tk.StringVar(value="Select a board and press Probe board.")
        self.firmware_info_var = tk.StringVar(value="No firmware selected.")
        self.plan_var = tk.StringVar(value="")
        self.status_var = tk.StringVar(value="Ready")

        self.build_ui()
        self.refresh_builds(select_first=True)
        self.log("esptool {} (Python {})\n".format(esptool.__version__, sys.version.split()[0]), "info")

        self.port_thread = threading.Thread(target=self.port_poller, daemon=True)
        self.port_thread.start()
        self.root.after(60, self.drain)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---- construction ----

    def build_ui(self):
        top = tk.Frame(self.root)
        top.pack(fill="x", padx=10, pady=(10, 4))

        boards = tk.LabelFrame(top, text="1. Board")
        boards.pack(fill="x")
        columns = ("port", "adapter", "serial", "state")
        self.tree = ttk.Treeview(boards, columns=columns, show="headings", height=4, selectmode="browse")
        for name, width in zip(columns, (90, 260, 200, 300)):
            self.tree.heading(name, text=name.title())
            self.tree.column(name, width=width, anchor="w")
        self.tree.pack(fill="x", padx=8, pady=(8, 4))
        self.tree.bind("<<TreeviewSelect>>", lambda e: self.update_board_info())
        self.tree.bind("<Double-1>", lambda e: self.open_monitor_selected())

        row = tk.Frame(boards)
        row.pack(fill="x", padx=8, pady=(0, 4))
        self.probe_button = tk.Button(row, text="Probe board", command=self.probe_selected)
        self.probe_button.pack(side="left")
        tk.Button(row, text="Open monitor", command=self.open_monitor_selected).pack(side="left", padx=6)
        tk.Checkbutton(row, text="Reconnect monitors when the board comes back",
                       variable=self.reconnect_var).pack(side="left", padx=6)
        tk.Checkbutton(row, text="Show all serial ports", variable=self.show_all_var).pack(side="left", padx=6)
        tk.Label(boards, textvariable=self.board_info_var, anchor="w", justify="left",
                 font=MONO_FONT).pack(fill="x", padx=8, pady=(0, 8))

        fw = tk.LabelFrame(top, text="2. Firmware")
        fw.pack(fill="x", pady=(6, 0))
        row = tk.Frame(fw)
        row.pack(fill="x", padx=8, pady=(8, 4))
        tk.Label(row, text="Build:").pack(side="left")
        self.build_box = ttk.Combobox(row, textvariable=self.build_var, state="readonly")
        self.build_box.pack(side="left", fill="x", expand=True, padx=4)
        self.build_box.bind("<<ComboboxSelected>>", lambda e: self.load_build_choice())
        tk.Button(row, text="Rescan", command=self.refresh_builds).pack(side="left", padx=2)
        tk.Button(row, text="Build folder...", command=self.browse_build).pack(side="left", padx=2)
        tk.Button(row, text=".bin file...", command=self.browse_bin).pack(side="left", padx=2)
        tk.Label(row, text="at").pack(side="left", padx=(6, 2))
        tk.Entry(row, textvariable=self.offset_var, width=9).pack(side="left")
        tk.Label(fw, textvariable=self.firmware_info_var, anchor="w", justify="left",
                 font=MONO_FONT).pack(fill="x", padx=8, pady=(0, 8))

        fl = tk.LabelFrame(top, text="3. Flash")
        fl.pack(fill="x", pady=(6, 0))
        row = tk.Frame(fl)
        row.pack(fill="x", padx=8, pady=(8, 4))
        for text, value in (("Auto (recommended)", "auto"), ("Bootloader + partitions + app", "full"),
                            ("App only", "app")):
            tk.Radiobutton(row, text=text, value=value, variable=self.mode_var,
                           command=self.update_plan).pack(side="left", padx=(0, 8))
        tk.Checkbutton(row, text="Erase whole flash first (wipes saved Wi-Fi and settings)",
                       variable=self.erase_var).pack(side="left", padx=8)

        row = tk.Frame(fl)
        row.pack(fill="x", padx=8, pady=(0, 4))
        tk.Label(row, text="Baud:").pack(side="left")
        ttk.Combobox(row, textvariable=self.baud_var, width=9,
                     values=[str(b) for b in FLASH_BAUDS]).pack(side="left", padx=(2, 10))
        tk.Checkbutton(row, text="Open a monitor after flashing",
                       variable=self.monitor_after_var).pack(side="left")
        self.flash_button = tk.Button(row, text="Flash", width=14, bg="#dff0d8", command=self.flash_selected)
        self.flash_button.pack(side="left", padx=(20, 4))
        self.stop_button = tk.Button(row, text="Stop", command=self.stop, state="disabled")
        self.stop_button.pack(side="left", padx=4)
        tk.Label(fl, textvariable=self.plan_var, anchor="w", justify="left").pack(fill="x", padx=8, pady=(0, 8))

        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=10, pady=4)
        log_frame = tk.Frame(self.notebook)
        self.log_text = scrolledtext.ScrolledText(log_frame, wrap="char", font=MONO_FONT,
                                                  background="#101418", foreground="#d8dee9")
        self.log_text.pack(fill="both", expand=True, padx=6, pady=6)
        self.log_text.tag_configure("info", foreground="#9aa5b1")
        self.log_text.tag_configure("good", foreground="#8fd18f")
        self.log_text.tag_configure("error", foreground="#ff8f8f")
        self.log_text.tag_configure("bus", foreground="#d8b86a")
        self.log_text.bind("<Key>", lambda e: None if (e.state & 0x4 and e.keysym.lower() in ("c", "a")) else "break")
        self.notebook.add(log_frame, text="Flasher log")

        tk.Label(self.root, textvariable=self.status_var, anchor="w", relief="sunken").pack(fill="x", side="bottom")

    # ---- log ----

    def log(self, text, tag=None):
        """Append to the flasher log. A bare CR rewrites the line, so progress bars stay one line."""
        widget = self.log_text
        for part in re.split(r"(\r\n|\n|\r)", text):
            if part in ("\n", "\r\n"):
                widget.insert("end", "\n")
                self.log_cr = False
            elif part == "\r":
                self.log_cr = True
            elif part:
                if self.log_cr:
                    widget.delete("end-1c linestart", "end-1c")
                    self.log_cr = False
                widget.insert("end", part, tag or ())
        lines = int(widget.index("end-1c").split(".")[0])
        if lines > monitor.MAX_LINES_IN_VIEW:
            widget.delete("1.0", "{}.0".format(lines - monitor.MAX_LINES_IN_VIEW))
        widget.see("end")

    def worker_log(self, text, tag=None):
        self.events.put(("log", text, tag))

    # ---- bus ----

    def port_poller(self):
        while True:
            try:
                self.events.put(("ports", monitor.list_serial_ports()))
            except Exception:
                pass
            time.sleep(PORT_POLL_SECONDS)

    def on_ports(self, ports):
        visible = ports if self.show_all_var.get() else [p for p in ports if p["likely_esp"]]
        stamp = datetime.now().strftime("%H:%M:%S")
        for kind, key, info in self.watcher.poll(visible):
            if kind == "looping":
                period = self.watcher.loop_period(key)
                self.log("[{}] {} keeps dropping off the bus (every {:.1f}s): it is boot-looping. Blank or "
                         "corrupt flash is the usual cause. Probe board will catch it in download mode.\n"
                         .format(stamp, info["device"], period or 0), "bus")
            elif kind == "settled":
                self.log("[{}] {} has stayed on the bus.\n".format(stamp, info["device"]), "bus")
            elif not self.watcher.boards[key]["looping_reported"]:
                self.log("[{}] {} {} ({})\n".format(stamp, info["device"], kind, info["adapter"]), "bus")
        self.ports = visible
        self.refresh_tree()
        self.reconnect_monitors()

    def refresh_tree(self):
        present = {port_key(p) for p in self.ports}
        for key, board in self.watcher.boards.items():
            if key not in present and not self.watcher.is_looping(key) and board["present"] is False \
                    and time.monotonic() - board["last_seen"] > LOOP_WINDOW_SECONDS:
                if self.tree.exists(key):
                    self.tree.delete(key)
                continue
            if not board["info"]["likely_esp"] and not self.show_all_var.get():
                if self.tree.exists(key):
                    self.tree.delete(key)
                continue
            info = board["info"]
            values = (info["device"], info["adapter"], info["serial_number"], self.watcher.state(key))
            if self.tree.exists(key):
                if tuple(self.tree.item(key, "values")) != values:
                    self.tree.item(key, values=values)
            else:
                self.tree.insert("", "end", iid=key, values=values)
        children = self.tree.get_children()
        if not self.tree.selection() and len(children) == 1:
            self.tree.selection_set(children[0])
        busy = "  |  working..." if self.busy else ""
        self.status_var.set("{} board(s) on the bus{}".format(len(self.ports), busy))

    def selected_key(self):
        selection = self.tree.selection()
        return selection[0] if selection else None

    def reconnect_monitors(self):
        if self.busy or not self.reconnect_var.get():
            return
        now = time.monotonic()
        for tab in self.tabs.values():
            if tab.session.is_open or tab.user_closed or now - tab.last_attempt < 0.3:
                continue
            port = find_port(tab.board_key, self.ports)
            if port is not None:
                tab.last_attempt = now
                tab.reopen_quietly(port["device"])

    # ---- firmware ----

    def refresh_builds(self, select_first=False):
        self.build_paths = discover_builds()
        labels = []
        for path in self.build_paths:
            try:
                labels.append(Firmware.from_build(path).label())
            except Exception as exc:
                labels.append("{}   [unusable: {}]".format(path, first_line(exc)))
        self.build_box.config(values=labels)
        if select_first and labels:
            self.build_box.current(0)
            self.load_build_choice()

    def load_build_choice(self):
        index = self.build_box.current()
        if index < 0:
            return
        self.set_firmware(lambda: Firmware.from_build(self.build_paths[index]))

    def browse_build(self):
        path = filedialog.askdirectory(title="ESP-IDF build directory", initialdir=str(REPO_ROOT))
        if path:
            self.set_firmware(lambda: Firmware.from_build(path))
            if self.firmware is not None:
                self.build_var.set(self.firmware.label())

    def browse_bin(self):
        path = filedialog.askopenfilename(title="Firmware image", initialdir=str(REPO_ROOT),
                                          filetypes=[("Binary images", "*.bin"), ("All files", "*.*")])
        if not path:
            return
        try:
            offset = int(self.offset_var.get(), 0)
        except ValueError:
            messagebox.showerror("Bad offset", "The offset must be a number, such as 0x10000.")
            return
        self.set_firmware(lambda: Firmware.from_bin(path, offset))
        if self.firmware is not None:
            self.build_var.set(self.firmware.label())

    def set_firmware(self, loader):
        try:
            self.firmware = loader()
        except Exception as exc:
            self.firmware = None
            self.firmware_info_var.set("Could not load: {}".format(exc))
            self.update_plan()
            return
        self.firmware_info_var.set("\n".join(self.firmware.summary_lines()))
        self.update_board_info()

    # ---- board info and plan ----

    def update_board_info(self):
        key = self.selected_key()
        probe = self.probes.get(key)
        if key is None:
            self.board_info_var.set("No board selected.")
        elif probe is None:
            self.board_info_var.set("Not probed yet. Probe board reads what is on its flash (read-only).")
        else:
            report = assess(probe, self.firmware)
            self.board_info_var.set("Probed at {}:\n".format(probe["time"]) + "\n".join(report["lines"]))
        self.update_plan()

    def current_plan(self, key):
        """(mode, reasons, blockers) for the selected board and firmware."""
        if self.firmware is None:
            return None, [], ["select a firmware"]
        probe = self.probes.get(key)
        report = assess(probe, self.firmware) if probe else None
        blockers = list(report["blockers"]) if report else []
        choice = self.mode_var.get()
        if choice == "auto":
            if report is None:
                return None, ["the board will be probed first to choose"], blockers
            return report["mode"], report["reasons"], blockers
        reasons = ["chosen by hand"]
        if choice == "app" and not self.firmware.image("app"):
            blockers.append("this firmware has no separate app image")
        if choice == "app" and report and report["mode"] == "full":
            reasons.append("warning: the probe recommends a full flash ({})".format("; ".join(report["reasons"])))
        return choice, reasons, blockers

    def update_plan(self):
        mode, reasons, blockers = self.current_plan(self.selected_key())
        lines = []
        if mode:
            names = ", ".join("{} @ 0x{:x}".format(i["role"], i["offset"]) for i in select_images(self.firmware, mode))
            lines.append("Will write: {}{}".format(names, "  (after erasing all flash)" if self.erase_var.get() else ""))
        if reasons:
            lines.append("Because: " + "; ".join(reasons))
        if blockers:
            lines.append("Cannot flash: " + "; ".join(blockers))
        self.plan_var.set("\n".join(lines))

    # ---- actions ----

    def set_busy(self, busy):
        self.busy = busy
        state = "disabled" if busy else "normal"
        self.flash_button.config(state=state)
        self.probe_button.config(state=state)
        self.stop_button.config(state="normal" if busy else "disabled")
        if busy:
            self.cancel.clear()

    def release_monitors(self, key):
        for tab in self.tabs.values():
            if tab.board_key == key and tab.session.is_open:
                tab.release_port()
                tab.emit("--- port released for the flasher ---\n", "info")

    def require_board(self):
        key = self.selected_key()
        if key is None:
            messagebox.showinfo("No board", "Select a board in the list first. If none is listed, plug one in; "
                                "a board that boot-loops still shows up for a couple of seconds at a time.")
        return key

    def probe_selected(self, then_flash=False):
        key = self.require_board()
        if key is None or self.busy:
            return
        self.set_busy(True)
        self.release_monitors(key)
        self.notebook.select(0)
        self.log("\n=== Probing {} ===\n".format(key), "info")
        pt_offset = self.firmware.partition_table_offset if self.firmware else DEFAULT_PARTITION_TABLE_OFFSET
        reset_after = False if then_flash else None
        self.pending_flash = then_flash

        def work():
            sink = TextSink(self.worker_log)
            try:
                with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
                    result = probe_board(key, self.worker_log, pt_offset, reset_after, self.cancel)
                self.events.put(("probe", key, result))
            except Cancelled:
                self.events.put(("probe_failed", key, "stopped"))
            except Exception as exc:
                self.events.put(("probe_failed", key, str(exc)))

        threading.Thread(target=work, daemon=True).start()

    def on_probe(self, key, result):
        self.probes[key] = result
        self.set_busy(False)
        report = assess(result, self.firmware)
        self.log("\n".join(report["lines"]) + "\n", "good")
        if self.tree.exists(key):
            self.tree.selection_set(key)
        self.update_board_info()
        if self.pending_flash:
            self.pending_flash = False
            self.confirm_and_flash(key)

    def on_probe_failed(self, key, message):
        self.set_busy(False)
        self.pending_flash = False
        self.log("Probe failed: {}\n".format(message), "error")
        if message != "stopped":
            messagebox.showwarning("Could not reach the board", message)

    def flash_selected(self):
        key = self.require_board()
        if key is None or self.busy:
            return
        if self.firmware is None:
            messagebox.showinfo("No firmware", "Pick a build or a .bin file first.")
            return
        if self.mode_var.get() == "auto" and key not in self.probes:
            self.probe_selected(then_flash=True)
            return
        self.confirm_and_flash(key)

    def confirm_and_flash(self, key):
        mode, reasons, blockers = self.current_plan(key)
        if blockers:
            messagebox.showerror("Cannot flash", "\n".join(blockers))
            return
        images = select_images(self.firmware, mode)
        port = find_port(key, self.ports) or self.watcher.boards.get(key, {}).get("info") or {"device": key}
        text = ["Board: {} ({})".format(port["device"], key), "Firmware: {}".format(self.firmware.source), ""]
        text += ["  0x{:06x}  {}  ({})".format(i["offset"], Path(i["path"]).name, human_size(len(i["data"])))
                 for i in images]
        if self.erase_var.get():
            text += ["", "The whole flash is erased first: saved Wi-Fi networks and settings are lost."]
        text += ["", "Why: " + "; ".join(reasons)]
        if not messagebox.askokcancel("Flash the board?", "\n".join(text)):
            return

        try:
            baud = int(self.baud_var.get())
        except ValueError:
            baud = DEFAULT_FLASH_BAUD
        firmware, erase, monitor_after = self.firmware, self.erase_var.get(), self.monitor_after_var.get()
        self.set_busy(True)
        self.release_monitors(key)
        self.notebook.select(0)
        self.log("\n=== Flashing {} ({}) ===\n".format(port["device"], mode), "info")

        def work():
            try:
                ok = flash_board(key, firmware, mode, erase, baud, self.worker_log, self.cancel, self.proc_holder)
                reappeared = None
                if ok and monitor_after:
                    self.worker_log("Waiting for the board to come back on the bus...\n")
                    reappeared = wait_for_port(key, cancel=self.cancel)
                self.events.put(("flashed", key, ok, reappeared))
            except Cancelled:
                self.events.put(("flashed", key, False, None))
            except Exception as exc:
                self.worker_log("Flash failed: {}\n".format(exc), "error")
                self.events.put(("flashed", key, False, None))

        threading.Thread(target=work, daemon=True).start()

    def on_flashed(self, key, ok, reappeared):
        self.set_busy(False)
        self.probes.pop(key, None)       # what we read is out of date now
        self.update_board_info()
        if ok:
            self.log("Flash complete.\n", "good")
            if reappeared is not None:
                self.open_monitor(reappeared)
            elif self.monitor_after_var.get():
                self.log("The board has not come back on the bus. Press RESET, or unplug and replug it.\n", "error")
        else:
            self.log("Flash did not complete.\n", "error")

    def stop(self):
        self.cancel.set()
        proc = self.proc_holder.get("proc")
        if proc is not None:
            try:
                proc.terminate()
            except Exception:
                pass
        self.log("Stopping...\n", "error")

    # ---- monitors ----

    def open_monitor_selected(self):
        key = self.require_board()
        if key is None:
            return
        port = find_port(key, self.ports)
        if port is None:
            port = self.watcher.boards[key]["info"]
        self.open_monitor(port)

    def open_monitor(self, port_info):
        key = port_key(port_info)
        for tab in self.tabs.values():
            if tab.board_key == key:
                self.notebook.select(tab.frame)
                if not tab.session.is_open:
                    tab.user_closed = False
                    tab.last_attempt = time.monotonic()
                    tab.reopen_quietly(port_info["device"])
                return tab
        tab = FlasherMonitorTab(self, self.notebook, port_info, monitor.DEFAULT_BAUD)
        self.notebook.add(tab.frame, text="Monitor {}".format(port_info["device"]))
        self.tabs[tab.key] = tab
        self.notebook.select(tab.frame)
        if not tab.open_port():
            tab.user_closed = False   # a boot-looping board: let reconnect catch it
        return tab

    # hooks the monitor tabs call back into
    def refresh_port_states(self):
        pass

    def remove_tab(self, key):
        tab = self.tabs.pop(key, None)
        if tab is not None:
            self.notebook.forget(tab.frame)

    # ---- event pump ----

    def drain(self):
        latest_ports = None
        try:
            for _ in range(400):
                event = self.events.get_nowait()
                kind = event[0]
                if kind == "ports":
                    latest_ports = event[1]
                    self.on_ports(latest_ports)
                elif kind == "log":
                    self.log(event[1], event[2])
                elif kind == "probe":
                    self.on_probe(event[1], event[2])
                elif kind == "probe_failed":
                    self.on_probe_failed(event[1], event[2])
                elif kind == "flashed":
                    self.on_flashed(event[1], event[2], event[3])
        except queue.Empty:
            pass
        try:
            for _ in range(400):
                kind, key, payload = self.rx_queue.get_nowait()
                tab = self.tabs.get(key)
                if tab is not None:
                    tab.handle_event(kind, payload)
        except queue.Empty:
            pass
        self.root.after(50, self.drain)

    def on_close(self):
        self.stop()
        for tab in list(self.tabs.values()):
            tab.close_port()
            tab.stop_logging()
        self.root.destroy()
