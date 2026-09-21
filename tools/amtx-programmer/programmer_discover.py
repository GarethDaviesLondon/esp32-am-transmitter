"""The Radio-Browser panel: search the directory, then play or save a result.

A panel mixin owns the widgets it builds, the rendering of its part of the
state document, and what its buttons do. It reaches the board through
`self.act` and `self.submit`, which ProgramTab provides, and never touches the
serial port itself.

A search is a blocking HTTPS round trip on the board, which is why everything
here goes through the tab's worker rather than the Tk thread.
"""

import tkinter as tk
from tkinter import ttk, messagebox

import amtx_console as ac

from programmer_config import TX_NAMES


class DiscoverPanel:
    """Part of ProgramTab. See the module docstring."""

    def build_discover(self):
        page = tk.Frame(self.inner)
        self.inner.add(page, text="Discover")
        row = tk.Frame(page)
        row.pack(fill="x", padx=4, pady=6)
        tk.Label(row, text="Search Radio-Browser by").pack(side="left")
        self.discover_by = tk.StringVar(value="name")
        ttk.Combobox(row, textvariable=self.discover_by, values=ac.DISCOVER_BY, width=9,
                     state="readonly").pack(side="left", padx=4)
        self.discover_query = tk.StringVar()
        entry = tk.Entry(row, textvariable=self.discover_query, width=40)
        entry.pack(side="left", padx=4)
        entry.bind("<Return>", lambda e: self.discover())
        tk.Button(row, text="Search", command=self.discover).pack(side="left", padx=4)
        tk.Label(row, text="MP3 over http only; a search takes a few seconds.", fg="#555555").pack(side="left", padx=8)

        columns = ("n", "name", "country", "kbps", "url")
        self.discover_tree = ttk.Treeview(page, columns=columns, show="headings", selectmode="browse", height=10)
        for name, title, width in (("n", "#", 40), ("name", "Name", 240), ("country", "Country", 140),
                                   ("kbps", "kbit/s", 60), ("url", "URL", 380)):
            self.discover_tree.heading(name, text=title)
            self.discover_tree.column(name, width=width, anchor="w", stretch=name == "url")
        self.discover_tree.pack(fill="both", expand=True, padx=4, pady=4)

        row = tk.Frame(page)
        row.pack(fill="x", padx=4, pady=(2, 6))
        tk.Button(row, text="Play on A", command=lambda: self.discover_play(0)).pack(side="left")
        tk.Button(row, text="Play on B", command=lambda: self.discover_play(1)).pack(side="left", padx=4)
        tk.Button(row, text="Save to stations", command=self.discover_save).pack(side="left", padx=(16, 4))

    def discover(self):
        query, by = self.discover_query.get().strip(), self.discover_by.get()
        if not query:
            messagebox.showinfo("Discover", "Type something to search for.", parent=self.frame)
            return
        self.discover_tree.delete(*self.discover_tree.get_children())

        def done(value):
            reply, hits = value
            self.discover_hits = hits
            for hit in hits:
                self.discover_tree.insert("", "end", iid=str(hit["n"]), values=(
                    hit["n"], hit["name"], hit["country"], hit["kbps"], hit["url"]))
        self.submit("Search for {}".format(query), lambda b: b.discover(by, query), done)

    def selected_hit(self):
        selection = self.discover_tree.selection()
        if not selection:
            messagebox.showinfo("No result", "Select a search result first.", parent=self.frame)
            return None
        return int(selection[0])

    def discover_play(self, ch):
        n = self.selected_hit()
        if n is not None:
            self.act("Play result {} on {}".format(n, TX_NAMES[ch]), lambda b: b.discover_play(ch, n))

    def discover_save(self):
        n = self.selected_hit()
        if n is not None:
            self.act("Save result {}".format(n), lambda b: b.discover_save(n))
