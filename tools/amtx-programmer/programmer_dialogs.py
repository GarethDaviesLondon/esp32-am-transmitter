"""One general-purpose form dialog, used for every "ask me some fields" case.

Adding a station, joining a network, setting a trim and changing the hostname
are the same dialog with different fields, so they are one dialog driven by a
field list rather than four nearly-identical windows.
"""

import tkinter as tk
from tkinter import ttk


class FormDialog(tk.Toplevel):
    """A small modal form. `fields` is [(key, label, default, secret)]; result is a dict or None."""

    def __init__(self, parent, title, fields, note=None, validate=None, ok_text="OK"):
        super().__init__(parent)
        self.title(title)
        self.transient(parent)
        self.resizable(False, False)
        self.result = None
        self.validate = validate
        self.vars = {}
        body = tk.Frame(self)
        body.pack(fill="both", expand=True, padx=14, pady=12)
        if note:
            tk.Label(body, text=note, justify="left", wraplength=420, anchor="w").grid(
                row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))
        first = None
        for row, (key, label, default, secret) in enumerate(fields, start=1):
            tk.Label(body, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=3)
            var = tk.StringVar(value="" if default is None else str(default))
            entry = tk.Entry(body, textvariable=var, width=46, show="*" if secret else "")
            entry.grid(row=row, column=1, sticky="we", pady=3)
            if secret:
                shown = tk.BooleanVar(value=False)
                tk.Checkbutton(body, text="Show", variable=shown,
                               command=lambda e=entry, s=shown: e.config(show="" if s.get() else "*")
                               ).grid(row=row, column=2, padx=(6, 0))
            self.vars[key] = var
            first = first or entry
        self.problem_var = tk.StringVar(value="")
        tk.Label(body, textvariable=self.problem_var, fg="#b00020", anchor="w", justify="left",
                 wraplength=420).grid(row=len(fields) + 1, column=0, columnspan=3, sticky="w")
        buttons = tk.Frame(self)
        buttons.pack(fill="x", padx=14, pady=(0, 12))
        tk.Button(buttons, text="Cancel", width=10, command=self.destroy).pack(side="right")
        tk.Button(buttons, text=ok_text, width=10, command=self.accept).pack(side="right", padx=6)
        self.bind("<Return>", lambda e: self.accept())
        self.bind("<Escape>", lambda e: self.destroy())
        if first is not None:
            first.focus_set()
        self.grab_set()
        parent.wait_window(self)

    def accept(self):
        values = {key: var.get() for key, var in self.vars.items()}
        if self.validate is not None:
            problem = self.validate(values)
            if problem:
                self.problem_var.set(problem)
                return
        self.result = values
        self.destroy()


def ask_form(parent, title, fields, note=None, validate=None, ok_text="OK"):
    return FormDialog(parent, title, fields, note, validate, ok_text).result
