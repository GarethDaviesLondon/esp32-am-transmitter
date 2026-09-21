"""The network and access panel: Wi-Fi, hostname, the web interface, reboot.

A panel mixin owns the widgets it builds, the rendering of its part of the
state document, and what its buttons do. It reaches the board through
`self.act` and `self.submit`, which ProgramTab provides, and never touches the
serial port itself.

The two actions that are hard to undo are here, and both ask first. Turning the
web interface off leaves the serial console as the only way back in, and
forgetting every saved network leaves the board waiting to be told a new one.
"""

import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext

import amtx_console as ac

from programmer_config import LEGACY_COMMANDS, MONO_FONT
from programmer_dialogs import ask_form


class NetworkPanel:
    """Part of ProgramTab. See the module docstring."""

    def build_network(self):
        page = tk.Frame(self.inner)
        self.inner.add(page, text="Network and access")

        wifi = tk.LabelFrame(page, text="Wi-Fi")
        wifi.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        row = tk.Frame(wifi)
        row.pack(fill="x", padx=6, pady=4)
        tk.Button(row, text="Scan", command=self.wifi_scan).pack(side="left")
        tk.Button(row, text="Saved networks", command=self.wifi_list).pack(side="left", padx=4)
        tk.Button(row, text="Status", command=self.wifi_status).pack(side="left", padx=4)
        row2 = tk.Frame(wifi)
        row2.pack(fill="x", padx=6, pady=(0, 4))
        tk.Button(row2, text="Join...", command=lambda: self.wifi_join(save_only=False)).pack(side="left")
        tk.Button(row2, text="Save without joining...",
                  command=lambda: self.wifi_join(save_only=True)).pack(side="left", padx=4)
        tk.Button(row2, text="Forget all...", command=self.wifi_forget).pack(side="left", padx=4)

        self.wifi_tree = ttk.Treeview(wifi, columns=("ssid", "rssi", "security"), show="headings",
                                      selectmode="browse", height=7)
        for name, title, width in (("ssid", "Network", 220), ("rssi", "Signal", 70), ("security", "Security", 80)):
            self.wifi_tree.heading(name, text=title)
            self.wifi_tree.column(name, width=width, anchor="w")
        self.wifi_tree.pack(fill="both", expand=True, padx=6, pady=4)
        self.wifi_tree.bind("<Double-1>", lambda e: self.wifi_join(save_only=False))
        self.wifi_text = scrolledtext.ScrolledText(wifi, height=6, font=MONO_FONT, wrap="word")
        self.wifi_text.pack(fill="both", expand=False, padx=6, pady=(0, 6))
        self.wifi_text.bind("<Key>", lambda e: None if (e.state & 0x4) else "break")

        side = tk.Frame(page)
        side.pack(side="left", fill="both", expand=True, padx=4, pady=4)

        net = tk.LabelFrame(side, text="On the network")
        net.pack(fill="x", pady=(0, 6))
        self.net_var = tk.StringVar(value="-")
        tk.Label(net, textvariable=self.net_var, anchor="w", justify="left", font=MONO_FONT).pack(fill="x", padx=6, pady=4)

        host = tk.LabelFrame(side, text="Hostname")
        host.pack(fill="x", pady=6)
        self.host_var = tk.StringVar(value="-")
        tk.Label(host, textvariable=self.host_var, anchor="w", font=MONO_FONT).pack(fill="x", padx=6, pady=(4, 0))
        tk.Label(host, text="Give each board its own name when there is more than one on the network.",
                 anchor="w", fg="#555555", wraplength=420, justify="left").pack(fill="x", padx=6)
        tk.Button(host, text="Change...", command=self.hostname_change).pack(anchor="w", padx=6, pady=4)

        web = tk.LabelFrame(side, text="Web interface")
        web.pack(fill="x", pady=6)
        self.web_var = tk.StringVar(value="-")
        tk.Label(web, textvariable=self.web_var, anchor="w", font=MONO_FONT).pack(fill="x", padx=6, pady=(4, 0))
        tk.Label(web, text="Off means no web server and no setup portal: the serial console becomes the "
                           "only way in. It has no password, so turn it off on any network you do not trust.",
                 anchor="w", fg="#555555", wraplength=420, justify="left").pack(fill="x", padx=6)
        row = tk.Frame(web)
        row.pack(fill="x", padx=6, pady=4)
        tk.Button(row, text="Turn on", command=lambda: self.web_set(True)).pack(side="left")
        tk.Button(row, text="Turn off...", command=lambda: self.web_set(False)).pack(side="left", padx=4)

        logs = tk.LabelFrame(side, text="Board log output")
        logs.pack(fill="x", pady=6)
        row = tk.Frame(logs)
        row.pack(fill="x", padx=6, pady=4)
        tk.Button(row, text="Log on", command=lambda: self.act("Log on", lambda b: b.log(True), refresh=False)
                  ).pack(side="left")
        tk.Button(row, text="Log off", command=lambda: self.act("Log off", lambda b: b.log(False), refresh=False)
                  ).pack(side="left", padx=4)
        tk.Label(logs, text="Only affects what scrolls past; the Program tab separates log lines either way.",
                 anchor="w", fg="#555555", wraplength=420, justify="left").pack(fill="x", padx=6, pady=(0, 4))

    def render_network(self, doc):
        net, sys_doc = doc.get("net", {}), doc.get("sys", {})
        self.net_var.set("state   {}\nnetwork {}\naddress {}\nsignal  {} dBm   drops {}".format(
            net.get("state", "?"), net.get("ssid", ""), net.get("ip", ""), net.get("rssi", "?"), net.get("drops", "?")))
        host = sys_doc.get("host")
        self.host_var.set("{}   (http://{}.local)".format(host, host) if host else "not reported by this firmware")
        web = sys_doc.get("web")
        self.web_var.set("on" if web is True else ("off" if web is False else "not reported by this firmware"))

    def show_wifi_lines(self, reply):
        self.wifi_text.delete("1.0", "end")
        self.wifi_text.insert("end", "\n".join(reply.lines) or "(no output)")

    def wifi_scan(self):
        def done(value):
            reply, nets = value
            self.wifi_nets = nets
            self.wifi_tree.delete(*self.wifi_tree.get_children())
            for i, net in enumerate(nets):
                self.wifi_tree.insert("", "end", iid=str(i), values=(
                    net["ssid"], "{} dBm".format(net["rssi"]), "secured" if net["secure"] else "open"))
            self.show_wifi_lines(reply)
        self.set_status("Scanning; the stream drops for a moment while the radio looks.")
        self.submit("Wi-Fi scan", lambda b: b.wifi_scan(), done)

    def wifi_list(self):
        self.submit("Saved networks", lambda b: b.wifi_list(), lambda v: self.show_wifi_lines(v[0]))

    def wifi_status(self):
        self.submit("Wi-Fi status", lambda b: b.wifi_status(), self.show_wifi_lines)

    def wifi_join(self, save_only):
        ssid = ""
        selection = self.wifi_tree.selection()
        if selection and int(selection[0]) < len(self.wifi_nets):
            ssid = self.wifi_nets[int(selection[0])]["ssid"]
        title = "Save a Wi-Fi network" if save_only else "Join a Wi-Fi network"
        note = ("Saved for the next boot, without joining now." if save_only else
                "The board tries for up to 15 seconds, and saves the network only if the join works. "
                "Names and passwords are case-sensitive.")
        result = ask_form(self.frame, title, [("ssid", "Network", ssid, False), ("password", "Password", "", True)],
                          note=note, validate=lambda v: None if v["ssid"].strip() else "Enter the network name.",
                          ok_text="Save" if save_only else "Join")
        if not result:
            return
        name, password = result["ssid"].strip(), result["password"]
        if save_only:
            self.submit("Save {}".format(name), lambda b: b.wifi_save(name, password), self.show_wifi_lines)
        else:
            self.set_status("Joining {}; this can take 15 seconds...".format(name))

            def done(reply):
                self.show_wifi_lines(reply)
                self.refresh(explicit=False)
            self.submit("Join {}".format(name), lambda b: b.wifi_join(name, password), done)

    def wifi_forget(self):
        if messagebox.askyesno("Forget all networks",
                               "Delete every saved Wi-Fi network from the board?\n\nIt stays on its current "
                               "network until it reboots. After that it has nowhere to join.", parent=self.frame):
            self.submit("Forget all networks", lambda b: b.wifi_forget(), self.show_wifi_lines)

    def hostname_change(self):
        current = (self.doc or {}).get("sys", {}).get("host", "")
        result = ask_form(self.frame, "Change the hostname", [("name", "Hostname", current, False)],
                          note="1 to 31 letters, digits and hyphens, not starting or ending with a hyphen. "
                               "The board answers to <name>.local at once; the router's device list catches "
                               "up after a reboot.",
                          validate=lambda v: ac.validate_hostname(v["name"])[1], ok_text="Change")
        if result:
            name = ac.validate_hostname(result["name"])[0]
            self.act("Hostname {}".format(name), lambda b: b.hostname_set(name))

    def web_set(self, on):
        if not on and not messagebox.askyesno(
                "Turn the web interface off",
                "Turn the web interface off?\n\nThe board stops serving web pages at once, and will not bring "
                "up the AMTX-Setup portal either. After this, the serial console is the only way to control "
                "it, including turning the web interface back on.", icon="warning", parent=self.frame):
            return
        self.act("Web interface {}".format("on" if on else "off"), lambda b: b.web_set(on))

    def reboot(self):
        if not messagebox.askyesno("Reboot", "Restart the board? It goes quiet for a few seconds, and this tab "
                                   "reconnects when it comes back.", parent=self.frame):
            return
        self.submit("Reboot", lambda b: b.reboot(), lambda reply: self.update_board_label())

    def send_raw(self):
        text = self.raw_var.get().strip()
        if not text:
            return
        legacy = text.split()[0] in LEGACY_COMMANDS
        expect_reboot = text.split()[0] == "reboot"
        slow = text.split()[0] in ("discover", "wifi")
        self.raw_var.set("")
        self.submit(text, lambda b: b.console.command(
            text, timeout=ac.SLOW_TIMEOUT if slow else ac.DEFAULT_TIMEOUT, legacy=legacy,
            quiet=1.5 if legacy and not slow else None, expect_reboot=expect_reboot),
            lambda reply: self.on_reply(reply), explicit=False)
