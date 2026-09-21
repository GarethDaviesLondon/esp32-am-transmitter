"""The station list panel: play, add, delete, reorder, trim, power-on station.

A panel mixin owns the widgets it builds, the rendering of its part of the
state document, and what its buttons do. It reaches the board through
`self.act` and `self.submit`, which ProgramTab provides, and never touches the
serial port itself.

See `programmer_transmitters.py` for the panel pattern.
"""

import tkinter as tk
from tkinter import ttk, messagebox

from programmer_config import TX_NAMES
from programmer_dialogs import ask_form


class StationsPanel:
    """Part of ProgramTab. See the module docstring."""

    def build_stations(self):
        page = tk.Frame(self.inner)
        self.inner.add(page, text="Stations")
        columns = ("n", "name", "trim", "poweron", "url")
        self.station_tree = ttk.Treeview(page, columns=columns, show="headings", selectmode="browse", height=10)
        for name, title, width in (("n", "#", 40), ("name", "Name", 220), ("trim", "Trim", 60),
                                   ("poweron", "Power-on", 80), ("url", "URL", 460)):
            self.station_tree.heading(name, text=title)
            self.station_tree.column(name, width=width, anchor="w", stretch=name == "url")
        self.station_tree.pack(fill="both", expand=True, padx=4, pady=4)
        self.station_tree.bind("<Double-1>", lambda e: self.station_play(0))

        row = tk.Frame(page)
        row.pack(fill="x", padx=4, pady=2)
        tk.Button(row, text="Play on A", command=lambda: self.station_play(0)).pack(side="left")
        tk.Button(row, text="Play on B", command=lambda: self.station_play(1)).pack(side="left", padx=4)
        tk.Button(row, text="Add...", command=self.station_add).pack(side="left", padx=(16, 4))
        tk.Button(row, text="Delete...", command=self.station_delete).pack(side="left", padx=4)
        tk.Button(row, text="Move up", command=lambda: self.station_move(-1)).pack(side="left", padx=(16, 4))
        tk.Button(row, text="Move down", command=lambda: self.station_move(1)).pack(side="left", padx=4)
        tk.Button(row, text="Trim...", command=self.station_trim).pack(side="left", padx=(16, 4))

        row = tk.Frame(page)
        row.pack(fill="x", padx=4, pady=(2, 6))
        tk.Label(row, text="Power-on station:").pack(side="left")
        for ch, name in enumerate(TX_NAMES):
            tk.Button(row, text="Set for {}".format(name),
                      command=lambda c=ch: self.station_default(c, clear=False)).pack(side="left", padx=4)
            tk.Button(row, text="Clear for {}".format(name),
                      command=lambda c=ch: self.station_default(c, clear=True)).pack(side="left", padx=(0, 12))

    def render_stations(self, doc):
        selected = self.selected_station()
        self.station_tree.delete(*self.station_tree.get_children())
        for i, station in enumerate(doc.get("stations", [])):
            flags = station.get("def", [])
            poweron = " ".join(TX_NAMES[c] for c in range(min(2, len(flags))) if flags[c]) or "-"
            self.station_tree.insert("", "end", iid=str(i), values=(
                i, station.get("name", ""), "{}%".format(station.get("gain", 100)), poweron, station.get("url", "")))
        if selected is not None and self.station_tree.exists(str(selected)):
            self.station_tree.selection_set(str(selected))

    def selected_station(self):
        selection = self.station_tree.selection()
        return int(selection[0]) if selection else None

    def require_station(self):
        index = self.selected_station()
        if index is None:
            messagebox.showinfo("No station", "Select a station in the list first.", parent=self.frame)
        return index

    def station_name(self, index):
        try:
            return self.doc["stations"][index]["name"]
        except (TypeError, KeyError, IndexError):
            return "station {}".format(index)

    def station_play(self, ch):
        index = self.require_station()
        if index is not None:
            self.act("Play {} on {}".format(self.station_name(index), TX_NAMES[ch]), lambda b: b.play(ch, index))

    def station_add(self):
        def validate(values):
            if not values["url"].strip() or "://" not in values["url"]:
                return "A station needs a full stream URL."
            if len(values["name"]) > 39:
                return "Names are at most 39 characters."
            if len(values["url"]) > 159:
                return "URLs are at most 159 characters."
            return None
        result = ask_form(self.frame, "Add a station",
                          [("name", "Name", "", False), ("url", "Stream URL", "http://", False)],
                          note="MP3 over plain http. The station is added to the end of the shared list.",
                          validate=validate, ok_text="Add")
        if result:
            name, url = result["name"].strip(), result["url"].strip()
            self.act("Add {}".format(name or url), lambda b: b.station_add(name, url))

    def station_delete(self):
        index = self.require_station()
        if index is None:
            return
        if messagebox.askyesno("Delete station", "Delete {}?\n\nIf it is a power-on station, that choice is "
                               "cleared too.".format(self.station_name(index)), parent=self.frame):
            self.act("Delete {}".format(self.station_name(index)), lambda b: b.station_delete(index))

    def station_move(self, step):
        index = self.require_station()
        if index is None:
            return
        target = index + step
        count = len(self.doc.get("stations", [])) if self.doc else 0
        if not 0 <= target < count:
            return

        def done(reply):
            self.refresh(explicit=False)
            self.root.after(600, lambda: self.station_tree.exists(str(target))
                            and self.station_tree.selection_set(str(target)))
        self.submit("Move {}".format(self.station_name(index)), lambda b: b.station_move(index, target), done)

    def station_trim(self):
        index = self.require_station()
        if index is None:
            return
        current = self.doc["stations"][index].get("gain", 100) if self.doc else 100

        def validate(values):
            try:
                value = int(values["trim"])
            except ValueError:
                return "The trim is a whole number of percent."
            return None if 10 <= value <= 400 else "The trim is between 10 and 400 percent."
        result = ask_form(self.frame, "Level trim for {}".format(self.station_name(index)),
                          [("trim", "Trim (%)", current, False)],
                          note="100 is no change. The loudness AGC handles most of the difference between "
                               "stations; this is for when it gets one wrong.", validate=validate)
        if result:
            value = int(result["trim"])
            self.act("Trim {}".format(self.station_name(index)), lambda b: b.station_trim(index, value))

    def station_default(self, ch, clear):
        if clear:
            self.act("Clear power-on station for {}".format(TX_NAMES[ch]), lambda b: b.station_default(ch, None))
            return
        index = self.require_station()
        if index is not None:
            self.act("Power-on station for {}".format(TX_NAMES[ch]), lambda b: b.station_default(ch, index))
