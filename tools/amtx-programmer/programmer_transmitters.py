"""The two transmitter panels: the RF form, Apply, and the test tone.

A panel mixin owns the widgets it builds, the rendering of its part of the
state document, and what its buttons do. It reaches the board through
`self.act` and `self.submit`, which ProgramTab provides, and never touches the
serial port itself.

This is also the file that knows the RF form is generated from `RF_FIELDS`
rather than laid out by hand, so adding a knob to the firmware is one row in
that table plus a key in design 07's `rf` command.
"""

import tkinter as tk
from tkinter import ttk, messagebox

import amtx_console as ac

from programmer_config import MONO_FONT, RF_FIELDS, STATE_RF_FIELD, TX_NAMES
from programmer_dialogs import ask_form


class TransmittersPanel:
    """Part of ProgramTab. See the module docstring."""

    def build_transmitters(self):
        page = tk.Frame(self.inner)
        self.inner.add(page, text="Transmitters")
        self.now_vars = []
        self.rf_vars = []
        self.tone_vars = []
        for ch, name in enumerate(TX_NAMES):
            box = tk.LabelFrame(page, text="Transmitter {}".format(name))
            box.pack(side="left", fill="both", expand=True, padx=4, pady=4)

            now = {"title": tk.StringVar(value="-"), "detail": tk.StringVar(value=""),
                   "url": tk.StringVar(value="")}
            tk.Label(box, textvariable=now["title"], anchor="w", font=("Segoe UI", 11, "bold")).pack(fill="x", padx=6)
            tk.Label(box, textvariable=now["detail"], anchor="w").pack(fill="x", padx=6)
            tk.Label(box, textvariable=now["url"], anchor="w", fg="#555555", wraplength=460,
                     justify="left").pack(fill="x", padx=6)
            row = tk.Frame(box)
            row.pack(fill="x", padx=6, pady=4)
            tk.Button(row, text="Stop", command=lambda c=ch: self.act(
                "Stop {}".format(TX_NAMES[c]), lambda b: b.stop(c))).pack(side="left")
            tk.Button(row, text="Play a URL...", command=lambda c=ch: self.play_url(c)).pack(side="left", padx=4)
            self.now_vars.append(now)

            form = tk.Frame(box)
            form.pack(fill="x", padx=6, pady=(4, 0))
            fields = {}
            for r, (key, label, kind, spec) in enumerate(RF_FIELDS):
                tk.Label(form, text=label).grid(row=r, column=0, sticky="w", pady=1)
                if kind == "check":
                    var = tk.BooleanVar(value=False)
                    widget = tk.Checkbutton(form, variable=var, text="on")
                elif kind == "fade":
                    var = tk.StringVar(value="off")
                    widget = ttk.Combobox(form, textvariable=var, values=ac.FADE_NAMES, width=10, state="readonly")
                else:
                    low, high, step = spec
                    var = tk.StringVar(value="")
                    widget = tk.Spinbox(form, from_=low, to=high, increment=step, textvariable=var, width=10)
                widget.grid(row=r, column=1, sticky="w", padx=6, pady=1)
                var.trace_add("write", lambda *_, c=ch: self.mark_rf_dirty(c))
                fields[key] = var
            self.rf_vars.append(fields)

            row = tk.Frame(box)
            row.pack(fill="x", padx=6, pady=6)
            tk.Button(row, text="Apply and save", bg="#dff0d8", command=lambda c=ch: self.apply_rf(c)).pack(side="left")
            tk.Button(row, text="Reload from board", command=lambda c=ch: self.reload_rf(c)).pack(side="left", padx=4)

            row = tk.Frame(box)
            row.pack(fill="x", padx=6, pady=(0, 6))
            tk.Label(row, text="Test tone (Hz):").pack(side="left")
            tone = tk.StringVar(value="1000")
            tk.Spinbox(row, from_=50, to=5000, increment=50, textvariable=tone, width=7).pack(side="left", padx=4)
            tk.Button(row, text="Tone on", command=lambda c=ch: self.tone(c, True)).pack(side="left", padx=2)
            tk.Button(row, text="Tone off", command=lambda c=ch: self.tone(c, False)).pack(side="left", padx=2)
            self.tone_vars.append(tone)

    def render_transmitters(self, doc):
        for ch, chain in enumerate(doc.get("chains", [])[:2]):
            play, rf = chain.get("play", {}), chain.get("rf", {})
            now = self.now_vars[ch]
            now["title"].set(play.get("name") or "Nothing playing")
            now["detail"].set("{}   carrier {}   {:.1f} kHz   depth {}%   level {}%".format(
                play.get("state", "?"), "ON" if rf.get("on") else "off", rf.get("hz", 0) / 1000.0,
                rf.get("depth", "?"), rf.get("levelnow", rf.get("level", "?"))))
            now["url"].set(play.get("url", ""))
            if not self.rf_dirty[ch]:
                values = {key: rf.get(field) for key, field in STATE_RF_FIELD.items()}
                values["fade"] = ac.FADE_NAMES[values["fade"]] if isinstance(values["fade"], int) \
                    and 0 <= values["fade"] < 4 else "off"
                self.fill_rf(ch, values)

    def fill_rf(self, ch, values):
        self.rf_loading = True
        try:
            for key, _, kind, _ in RF_FIELDS:
                value = values.get(key)
                if value is None:
                    continue
                var = self.rf_vars[ch][key]
                if kind == "check":
                    var.set(str(value).lower() in ("1", "true", "on"))
                elif kind == "fade":
                    var.set(ac.FADE_NAMES[int(value)] if str(value).isdigit() and int(value) < 4 else str(value))
                else:
                    var.set(str(value))
        finally:
            self.rf_loading = False
        self.rf_dirty[ch] = False

    def mark_rf_dirty(self, ch):
        if not self.rf_loading:
            self.rf_dirty[ch] = True

    def play_url(self, ch):
        result = ask_form(self.frame, "Play a URL on transmitter {}".format(TX_NAMES[ch]),
                          [("url", "Stream URL", "http://", False)],
                          note="MP3 over plain http. It plays once and is not saved to the station list.",
                          validate=lambda v: None if "://" in v["url"] and len(v["url"]) > 8 else "Enter a full URL.")
        if result:
            url = result["url"].strip()
            self.act("Play URL on {}".format(TX_NAMES[ch]), lambda b: b.play(ch, url))

    def rf_values_from_form(self, ch):
        values = {}
        for key, label, kind, spec in RF_FIELDS:
            var = self.rf_vars[ch][key]
            if kind == "check":
                values[key] = "on" if var.get() else "off"
            elif kind == "fade":
                values[key] = var.get()
            else:
                text = var.get().strip()
                try:
                    number = int(text)
                except ValueError:
                    raise ValueError("{} must be a whole number".format(label))
                low, high, _ = spec
                if not low <= number <= high:
                    raise ValueError("{} must be between {} and {}".format(label, low, high))
                values[key] = number
        return values

    def apply_rf(self, ch):
        try:
            values = self.rf_values_from_form(ch)
        except ValueError as exc:
            messagebox.showwarning("Transmitter {}".format(TX_NAMES[ch]), str(exc), parent=self.frame)
            return

        def done(reply):
            if reply.ok:
                self.rf_dirty[ch] = False
            self.refresh(explicit=False)
        self.submit("Apply transmitter {}".format(TX_NAMES[ch]), lambda b: b.rf_set(ch, values), done)

    def reload_rf(self, ch):
        def done(value):
            reply, values = value
            if reply.ok and values:
                self.fill_rf(ch, values)
        self.submit("Reload transmitter {}".format(TX_NAMES[ch]), lambda b: b.rf_get(ch), done)

    def tone(self, ch, on):
        hz = self.tone_vars[ch].get().strip()
        if on:
            try:
                hz_value = int(hz)
            except ValueError:
                messagebox.showwarning("Test tone", "The tone frequency must be a whole number.", parent=self.frame)
                return
            self.act("Tone on {}".format(TX_NAMES[ch]), lambda b: b.tone(ch, True, hz_value))
        else:
            self.act("Tone off {}".format(TX_NAMES[ch]), lambda b: b.tone(ch, False))
