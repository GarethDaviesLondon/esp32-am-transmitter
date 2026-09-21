"""The window itself: the port list, the tabs, and the baud-detection dialog.

Tk plumbing only. Anything here that had to think about serial ports or text
has been moved to the module that owns that job, which is what keeps this file
about layout and event handling.
"""

import queue
import threading

import tkinter as tk
from tkinter import ttk, messagebox

from monitor_baud import detect_baud, summarise_results
from monitor_config import COMMON_BAUDS, DEFAULT_BAUD, DEFAULT_LISTEN_SECONDS
from monitor_ports import list_serial_ports
from monitor_tab import MonitorTab



class DetectDialog(tk.Toplevel):
    """Runs detect_baud() on one port in a thread and shows the ranked result."""

    def __init__(self, app, port_info, listen_seconds, reset, all_bauds):
        super().__init__(app.root)
        self.app = app
        self.port_info = port_info
        self.title("Detect baud rate on {}".format(port_info["device"]))
        self.geometry("880x420")
        self.transient(app.root)

        self.events = queue.Queue()
        self.cancel = threading.Event()
        self.results = []

        header = "{}  ({})".format(port_info["device"], port_info["adapter"])
        if port_info["native_usb"]:
            header += "   -   native USB CDC: the baud rate is cosmetic on this port"
        tk.Label(self, text=header, anchor="w").pack(fill="x", padx=10, pady=(10, 4))

        columns = ("baud", "bytes", "printable", "score", "sample")
        self.tree = ttk.Treeview(self, columns=columns, show="headings", height=12)
        for name, width in zip(columns, (80, 70, 80, 70, 460)):
            self.tree.heading(name, text=name.title())
            self.tree.column(name, width=width, anchor="w")
        self.tree.pack(fill="both", expand=True, padx=10, pady=4)
        self.tree.bind("<Double-1>", lambda e: self.open_selected())

        self.progress_var = tk.StringVar(value="Starting...")
        tk.Label(self, textvariable=self.progress_var, anchor="w").pack(fill="x", padx=10)

        buttons = tk.Frame(self)
        buttons.pack(fill="x", padx=10, pady=8)
        tk.Button(buttons, text="Open monitor at this rate", command=self.open_selected).pack(side="left")
        tk.Button(buttons, text="Stop", command=self.cancel.set).pack(side="left", padx=6)
        tk.Button(buttons, text="Close", command=self.destroy).pack(side="right")

        self.thread = threading.Thread(
            target=self.run_detection,
            args=(listen_seconds, reset, not all_bauds),
            daemon=True,
        )
        self.thread.start()
        self.after(120, self.drain)

    def run_detection(self, listen_seconds, reset, stop_when_confident):
        try:
            results = detect_baud(
                self.port_info["device"],
                listen_seconds=listen_seconds,
                reset=reset,
                stop_when_confident=stop_when_confident,
                progress=lambda msg: self.events.put(("progress", msg)),
                cancel=self.cancel,
            )
        except Exception as exc:
            self.events.put(("progress", "Detection failed: {}".format(exc)))
            results = []
        self.events.put(("done", results))

    def drain(self):
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "progress":
                    self.progress_var.set(payload)
                elif kind == "done":
                    self.show_results(payload)
                    return
        except queue.Empty:
            pass
        self.after(120, self.drain)

    def show_results(self, results):
        self.results = results
        for row in results:
            note = row["error"] or row["sample"] or "(silence)"
            self.tree.insert("", "end", values=(
                row["baud"], row["bytes"], "{:.0%}".format(row["printable_ratio"]),
                "{:.2f}".format(row["score"]), note,
            ))
        summary = summarise_results(results)
        if results and not all(r["error"] for r in results):
            summary += " Double-click a row to open a monitor at that rate."
        self.progress_var.set(summary)

    def open_selected(self):
        selection = self.tree.selection()
        if not selection:
            if not self.results:
                return
            baud = self.results[0]["baud"]
        else:
            baud = int(self.tree.item(selection[0], "values")[0])
        self.cancel.set()
        self.app.open_monitor(self.port_info, baud)
        self.destroy()

    def destroy(self):
        self.cancel.set()
        return super().destroy()



class SerialMonitorApp:
    """Board list on top, one monitor tab per open port underneath."""

    def __init__(self, root):
        self.root = root
        self.root.title("ESP Serial Monitor")
        self.root.geometry("1120x800")
        self.root.minsize(900, 600)

        self.rx_queue = queue.Queue()
        self.tabs = {}
        self.ports = []

        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.esp_only_var = tk.BooleanVar(value=False)
        self.reset_var = tk.BooleanVar(value=True)
        self.all_bauds_var = tk.BooleanVar(value=False)
        self.listen_var = tk.StringVar(value=str(DEFAULT_LISTEN_SECONDS))
        self.status_var = tk.StringVar(value="Ready")

        self.build_ui()
        self.refresh_ports()
        self.root.after(60, self.drain_queue)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def build_ui(self):
        boards = tk.LabelFrame(self.root, text="Boards")
        boards.pack(fill="x", padx=10, pady=(10, 4))

        columns = ("port", "adapter", "description", "state")
        self.tree = ttk.Treeview(boards, columns=columns, show="headings", height=6,
                                 selectmode="extended")
        for name, width in zip(columns, (110, 220, 480, 90)):
            self.tree.heading(name, text=name.title())
            self.tree.column(name, width=width, anchor="w")
        self.tree.pack(fill="x", expand=False, padx=8, pady=(8, 4))
        self.tree.bind("<Double-1>", lambda e: self.open_selected())

        controls = tk.Frame(boards)
        controls.pack(fill="x", padx=8, pady=(0, 8))

        tk.Button(controls, text="Refresh", command=self.refresh_ports).pack(side="left")
        tk.Checkbutton(controls, text="Likely ESP boards only", variable=self.esp_only_var,
                       command=self.refresh_ports).pack(side="left", padx=6)

        tk.Label(controls, text="Baud:").pack(side="left", padx=(16, 2))
        ttk.Combobox(controls, textvariable=self.baud_var, width=9,
                     values=[str(b) for b in COMMON_BAUDS]).pack(side="left")
        tk.Button(controls, text="Open monitor", command=self.open_selected,
                  bg="#dff0d8").pack(side="left", padx=6)

        tk.Button(controls, text="Detect baud...", command=self.detect_selected).pack(side="left", padx=(16, 2))
        tk.Label(controls, text="listen (s):").pack(side="left", padx=(6, 2))
        tk.Entry(controls, textvariable=self.listen_var, width=5).pack(side="left")
        tk.Checkbutton(controls, text="Reset board while probing",
                       variable=self.reset_var).pack(side="left", padx=6)
        tk.Checkbutton(controls, text="Try every rate", variable=self.all_bauds_var).pack(side="left", padx=6)

        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=10, pady=(4, 4))

        tk.Label(self.root, textvariable=self.status_var, anchor="w",
                 relief="sunken").pack(fill="x", side="bottom")

    # ---- board list ----

    def refresh_ports(self):
        self.ports = list_serial_ports(esp_only=self.esp_only_var.get())
        self.tree.delete(*self.tree.get_children())
        for port in self.ports:
            self.tree.insert("", "end", iid=port["device"], values=(
                port["device"],
                port["adapter"],
                port["description"],
                self.state_of(port["device"]),
            ))
        esp_count = sum(1 for p in self.ports if p["likely_esp"])
        self.status_var.set("{} port(s) found, {} look like ESP boards.".format(len(self.ports), esp_count))

    def state_of(self, device):
        tab = self.tabs.get(device)
        if tab is None:
            return ""
        return "open" if tab.session.is_open else "closed"

    def refresh_port_states(self):
        for device in self.tree.get_children():
            values = list(self.tree.item(device, "values"))
            values[3] = self.state_of(device)
            self.tree.item(device, values=values)

    def selected_ports(self):
        selected = [p for p in self.ports if p["device"] in self.tree.selection()]
        if not selected and len(self.ports) == 1:
            selected = self.ports[:]
        return selected

    # ---- actions ----

    def open_selected(self):
        ports = self.selected_ports()
        if not ports:
            messagebox.showinfo("No port selected", "Select one or more ports in the list first.")
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showerror("Bad baud rate", "The baud rate must be a whole number.")
            return
        for port in ports:
            self.open_monitor(port, baud)

    def open_monitor(self, port_info, baud):
        device = port_info["device"]
        existing = self.tabs.get(device)
        if existing is not None:
            self.notebook.select(existing.frame)
            if not existing.session.is_open:
                existing.baud_var.set(str(baud))
                existing.toggle_port()
            return existing

        tab = MonitorTab(self, self.notebook, port_info, baud)
        self.notebook.add(tab.frame, text=device)
        self.tabs[device] = tab
        self.notebook.select(tab.frame)
        tab.open_port()
        return tab

    def remove_tab(self, device):
        tab = self.tabs.pop(device, None)
        if tab is None:
            return
        self.notebook.forget(tab.frame)
        self.refresh_port_states()

    def detect_selected(self):
        ports = self.selected_ports()
        if not ports:
            messagebox.showinfo("No port selected", "Select a port in the list first.")
            return
        port = ports[0]
        if len(ports) > 1:
            messagebox.showinfo("One at a time",
                                "Detection opens the port exclusively; probing {} only.".format(port["device"]))

        tab = self.tabs.get(port["device"])
        if tab is not None and tab.session.is_open:
            if not messagebox.askyesno(
                    "Port in use",
                    "{} is open in a monitor tab. Close it and probe the port?".format(port["device"])):
                return
            tab.close_port()

        try:
            listen = float(self.listen_var.get())
        except ValueError:
            listen = DEFAULT_LISTEN_SECONDS
        DetectDialog(self, port, listen, self.reset_var.get(), self.all_bauds_var.get())

    # ---- event pump ----

    def drain_queue(self):
        try:
            for _ in range(400):
                kind, key, payload = self.rx_queue.get_nowait()
                tab = self.tabs.get(key)
                if tab is not None:
                    tab.handle_event(kind, payload)
        except queue.Empty:
            pass
        self.root.after(60, self.drain_queue)

    def on_close(self):
        for tab in list(self.tabs.values()):
            tab.close_port()
            tab.stop_logging()
        self.root.destroy()
