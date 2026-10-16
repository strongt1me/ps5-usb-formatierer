"""PS5-USB-Formatierer fuer Windows.

Findet die PS5 im Netz (ps5_discovery) und spricht mit dem usb_format-Payload (HTTP-API,
Port 8090). Laeuft der Payload noch nicht, wird er an den ELF-Loader der PS5 (Port 9021) geschickt.
Am PC formatiert der Windows-Helfer (pc_common) - für die PS5 oder frei ("Am PC Standard", standard_panel).
Sprachen: Deutsch, English, Italiano, Español, Français (i18n).
"""
import http.client
import json
import os
import queue
import re
import socket
import subprocess
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
import urllib.error
import urllib.request
from tkinter import messagebox, ttk

import i18n
import pc_common
import ps5_discovery
from i18n import _, quote
from pc_common import (PcJob, admin_wait_text, ask_string, confirm, disk_name, enable_dpi_awareness, fmt_size, footer,
                       hwnd_of, irreversible_admin_text, keep_on_screen, open_help, pc_dev_text, pc_list, pc_used,
                       pc_volumes_text, resource_path, serial_args, setup_window, show_centered, top_bar)
from standard_panel import StandardPanel

APP_NAME = "PS5USBFormatierer"
API_PORT = 8090
LOADER_PORT = 9021
LABEL_RE = re.compile(r"^[A-Za-z0-9 _-]{0,11}\Z")   # \Z: kein Zeilenumbruch am Ende
IP_RE = re.compile(r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$")
MODES = (("ps5", "USB an der PS5"), ("pc", "USB am PC (für PS5)"), ("std", "Am PC Standard"))


def find_payload():
    for p in (resource_path("usb_format.elf"),
              os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "usb_format", "usb_format.elf")):
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


def load_settings():
    return pc_common.load_settings(APP_NAME)


def save_settings(data):
    pc_common.save_settings(APP_NAME, data)


def dev_title(d):
    return d.get("model") or _("USB-Laufwerk")


def formattable(d):
    # Eingebundene Sticks werden nicht selbst ausgehängt (CE-108255-1): Der Benutzer zieht sie an der PS5 ab und
    # steckt sie wieder ein, der Formatierer übernimmt sie vor der PS5 ("replug")
    return not d["locked"]


def is_replug_step(step, flags=None):
    """Warten auf Abziehen/Einstecken? Neuere Payloads melden es ausdrücklich, ältere nur im (deutschen) Text"""
    if flags and "replug" in flags:
        return bool(flags["replug"])
    return "abziehen" in step or "einstecken" in step or "neu angemeldet" in step


def fat32_cluster_kb(size):
    g = 1024 ** 3
    return 4 if size <= 8 * g else 8 if size <= 16 * g else 16 if size <= 32 * g else 32


class ApiError(Exception):
    pass


# ohne System-Proxy: die PS5 steht im eigenen Netz, ein eingetragener Proxy fände sie nicht
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class Refused(ConnectionError):
    """Port 8090 lehnt ab: dort läuft (noch) kein Formatierer"""


class Api:
    def __init__(self, host):
        self.base = f"http://{host}:{API_PORT}"

    def _call(self, path, body=None, timeout=4):
        data = None
        headers = {}
        if body is not None:
            body = dict(body, lang=i18n.lang())   # Meldungen des Auftrags in dieser Sprache
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        sep = "&" if "?" in path else "?"
        req = urllib.request.Request(f"{self.base}{path}{sep}lang={i18n.lang()}", data=data, headers=headers)
        try:
            with _OPENER.open(req, timeout=timeout) as r:
                result = json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read().decode("utf-8")).get("error")
            except (ValueError, OSError, AttributeError):
                msg = None
            raise ApiError(str(msg) if msg else _("HTTP-Fehler {code}", code=e.code)) from None
        except (urllib.error.URLError, OSError, ValueError, http.client.HTTPException) as e:
            reason = getattr(e, "reason", e)
            err = Refused if isinstance(e, ConnectionRefusedError) or isinstance(reason, ConnectionRefusedError) \
                else ConnectionError
            raise err(str(e) or type(e).__name__) from None
        if not isinstance(result, dict):   # Formatierer liefert immer ein Objekt - alles andere ist kaputt
            raise ConnectionError(_("HTTP-Fehler {code}", code=200))
        return result

    def devices(self, timeout=4):
        return self._call("/api/devices", timeout=timeout)

    def status(self):
        return self._call("/api/status")

    def library(self):
        return self._call("/api/library", timeout=8)

    def delete_games(self, keys, uninstall):
        return self._call("/api/delete", {"keys": ",".join(keys), "uninstall": uninstall})

    def format(self, device, fs, label, size, replug=False, limit=0, folders=True, verify=False, ident=""):
        body = {"device": device, "fs": fs, "label": label, "size": size, "replug": replug,
                "folders": folders, "verify": verify}
        if ident:
            body["ident"] = ident   # Seriennummer: ein getauschter baugleicher Stick wird abgelehnt
        if limit:
            body["limit"] = limit   # gefälschter Stick: nur den echten Bereich nutzen
        return self._call("/api/format", body)

    def cancel(self):
        return self._call("/api/cancel", {})


def send_payload(host, path):
    with open(path, "rb") as f:
        data = f.read()
    with socket.create_connection((host, LOADER_PORT), timeout=5) as s:
        s.sendall(data)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.withdraw()
        self.resizable(False, False)
        self.report_callback_exception = self._on_exception
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.bind("<<StdClose>>", lambda e: self._on_close())
        self.bind("<<StdResized>>", lambda e: self.after_idle(self._keep_on_screen))
        setup_window(self)   # Symbol (auch für Dialoge) und Stil je System

        self.settings = load_settings()
        i18n.set_lang(self.settings.get("lang") or i18n.system_lang())
        self.q = queue.Queue()
        self.api = None
        self.devices = []
        self.busy = False          # Netzwerkaktion oder Formatierung laeuft
        self.formatting = False
        self.watching = False      # Abfrage nach fremden Aufträgen unterwegs
        self.consoles = {}         # Anzeigetext -> Fund aus ps5_discovery
        self.ps5_devices = []      # zuletzt gelesene Liste der PS5 (bleibt beim Umschalten erhalten)
        self.pc_job = None         # laufender Auftrag des PC-Helfers
        self.games = {}            # PS5-Gerätename (da1 ...) -> Spiele darauf (aus /api/library)
        self.lib_loading = False
        self.lib_known = False     # Spieleliste mindestens einmal geladen (sonst Inhalt unbekannt)
        self.ps5_step = ""         # aktueller Schritt eines PS5-Auftrags (für "Abbrechen")
        self.ps5_flags = {}        # dazu: replug/cancellable, falls der Payload sie meldet

        self.std_busy = False      # Auftrag im Bereich "Am PC Standard"
        mode = self.settings.get("mode")
        self.mode_var = tk.StringVar(value=mode if mode in dict(MODES) else "ps5")
        self.verify_var = tk.BooleanVar(value=self.settings.get("verify", False))
        self.folders_var = tk.BooleanVar(value=self.settings.get("folders", True))
        self.host_var = tk.StringVar(value=self.settings.get("host", ""))
        self.dev_var = tk.StringVar()
        self.fs_var = tk.StringVar(value="exFAT")
        self.cluster_var = tk.StringVar()
        saved_label = self.settings.get("label", "")   # die Eingabeprüfung greift nur beim Tippen
        self.label_var = tk.StringVar(value=saved_label if LABEL_RE.match(saved_label) else "")
        self.status_var = tk.StringVar(value=_("Nicht verbunden"))
        self.uninstall_var = tk.BooleanVar(value=True)

        self._build_ui()
        self._update()

        show_centered(self)   # mittig im nutzbaren Bildschirmbereich, ganz sichtbar
        self._poll_id = self.after(50, self._poll)
        self._mode_changed(initial=True)
        self._watch_id = self.after(3000, self._watch)

    # ------------------------------------------------------------------ UI

    def _build_ui(self):
        outer = self.outer = ttk.Frame(self, padding=(14, 8, 14, 12))
        outer.grid(sticky="nsew")
        outer.columnconfigure(0, weight=1)

        top, self.lang_box = top_bar(outer, i18n.lang(), self._set_lang, lambda: open_help(i18n.lang(), self))
        top.grid(row=0, column=0, sticky="ew", pady=(0, 6))

        # Schalter: Stick an der PS5 (über das Netzwerk), am PC für die PS5 oder am PC frei formatieren
        modes = ttk.Frame(outer)
        modes.grid(row=1, column=0, sticky="ew")
        modes.columnconfigure(tuple(range(len(MODES))), weight=1, uniform="mode")
        self.mode_btns = []
        for col, (val, text) in enumerate(MODES):
            b = ttk.Radiobutton(modes, text=_(text), value=val, variable=self.mode_var, style="Toolbutton",
                                command=self._mode_changed)
            b.grid(row=0, column=col, sticky="ew", padx=(0 if col == 0 else 3, 0 if col == len(MODES) - 1 else 3),
                   ipady=3)
            self.mode_btns.append(b)

        # "Am PC Standard": eigener Bereich ohne PS5-Optionen (derselbe wie im eigenständigen USB-Formatierer)
        self.std_panel = StandardPanel(outer, self.settings.setdefault("standard", {}),
                                       lambda: save_settings(self.settings), on_busy=self._std_busy_changed)
        self.std_panel.grid(row=2, column=0, sticky="nsew")

        # PS5 und "USB am PC (für PS5)"
        frm = self.ps5_body = ttk.Frame(outer)
        frm.grid(row=2, column=0, sticky="nsew")
        footer(outer).grid(row=3, column=0, sticky="ew")
        frm.columnconfigure(0, weight=1)
        row = 0

        def caption(text):
            nonlocal row
            lbl = ttk.Label(frm, text=text)
            lbl.grid(row=row, column=0, columnspan=2, sticky="w", pady=(8, 2))
            row += 1
            return lbl

        # wie in der Weboberfläche ganz oben und nur eine Zeile (die Erklärung steht in der Anleitung)
        self.ps5_warn = ttk.Label(
            frm, foreground="#9a5b00", wraplength=600, justify="left",
            text="⚠ " + _("Während der Nutzung keine USB-Datenträger an der PS5 abziehen oder neu anschließen"))
        self.ps5_warn.grid(row=row, column=0, columnspan=2, sticky="w", pady=(8, 0))
        row += 1
        self.ps5_caption = caption(_("PS5:"))
        self.host_entry = ttk.Combobox(frm, textvariable=self.host_var, width=62)
        self.host_entry.grid(row=row, column=0, sticky="ew")
        self.host_entry.bind("<Return>", lambda e: self._connect())
        self.host_entry.bind("<<ComboboxSelected>>", lambda e: self._connect())
        self.search_btn = ttk.Button(frm, text=_("Suchen"), command=self._search)
        self.search_btn.grid(row=row, column=1, sticky="ew", padx=(6, 0))
        row += 1

        caption(_("Laufwerk:"))
        self.dev_combo = ttk.Combobox(frm, textvariable=self.dev_var, state="readonly", style="Dev.TCombobox",
                                      postcommand=self._widen_dev_list)
        self.dev_combo.grid(row=row, column=0, sticky="ew")
        self.dev_combo.bind("<<ComboboxSelected>>", lambda e: (self._update(), self._show_games()))
        self.reload_btn = ttk.Button(frm, text=_("Aktualisieren"), command=self._reload)
        self.reload_btn.grid(row=row, column=1, padx=(6, 0))
        row += 1

        # Spiele auf dem gewählten Datenträger (nur PS5-Modus, wie "Inhalt" in der Weboberfläche)
        self.games_box = ttk.Frame(frm)
        self.games_box.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        self.games_box.columnconfigure(0, weight=1)
        self.games_caption = ttk.Label(self.games_box, text=_("Spiele auf diesem Datenträger:"))
        self.games_caption.grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, 2))
        cols = ("name", "id", "kind", "size")
        self.games_tree = ttk.Treeview(self.games_box, columns=cols, show="headings", height=5, selectmode="extended")
        self.games_tree.bind("<<TreeviewSelect>>", lambda e: self._update())
        for c, text, w, anchor in (("name", _("Titel"), 250, "w"), ("id", _("Titel-ID"), 90, "w"),
                                   ("kind", _("Format"), 70, "w"), ("size", _("Größe"), 80, "e")):
            self.games_tree.heading(c, text=text, anchor=anchor)
            self.games_tree.column(c, width=w, minwidth=40, anchor=anchor, stretch=(c == "name"))
        self.games_tree.grid(row=1, column=0, sticky="ew")
        sb = ttk.Scrollbar(self.games_box, orient="vertical", command=self.games_tree.yview)
        sb.grid(row=1, column=1, sticky="ns")
        self.games_tree.configure(yscrollcommand=sb.set)
        # Löschen: Spiele oben markieren (Strg/Umschalt für mehrere)
        gbar = ttk.Frame(self.games_box)
        gbar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        self.del_btn = ttk.Button(gbar, text=_("Markierte Spiele löschen …"), command=self._delete_games)
        self.del_btn.pack(side="right")
        self.uninstall_chk = ttk.Checkbutton(gbar, variable=self.uninstall_var,
                                             text=_("Kachel auch von der Startseite entfernen"))
        self.uninstall_chk.pack(side="left")
        row += 1

        caption(_("Dateisystem:"))
        self.fs_combo = ttk.Combobox(frm, textvariable=self.fs_var, values=["exFAT", "FAT32"], state="readonly")
        self.fs_combo.grid(row=row, column=0, columnspan=2, sticky="ew")
        self.fs_combo.bind("<<ComboboxSelected>>", lambda e: self._update())
        row += 1

        caption(_("Größe der Zuordnungseinheiten:"))
        self.cluster_combo = ttk.Combobox(frm, textvariable=self.cluster_var, state="disabled")
        self.cluster_combo.grid(row=row, column=0, columnspan=2, sticky="ew")
        row += 1

        caption(_("Volumebezeichnung:"))
        vcmd = (self.register(lambda p: bool(LABEL_RE.match(p))), "%P")
        self.label_entry = ttk.Entry(frm, textvariable=self.label_var, validate="key", validatecommand=vcmd)
        self.label_entry.grid(row=row, column=0, columnspan=2, sticky="ew")
        row += 1
        self.label_hint = ttk.Label(frm, foreground="#666666")
        self.label_hint.grid(row=row, column=0, columnspan=2, sticky="w")
        row += 1

        # dieselben Zusatzoptionen wie in der Weboberfläche (an der PS5 und am PC)
        self.pc_opts = ttk.Frame(frm)
        self.pc_opts.grid(row=row, column=0, columnspan=2, sticky="w", pady=(8, 0))
        self.verify_chk = ttk.Checkbutton(self.pc_opts, variable=self.verify_var,
                                          text=_("Vorher Echtheit und Tempo prüfen (ca. 2–10 Minuten)"))
        self.verify_chk.pack(anchor="w")
        self.folders_chk = ttk.Checkbutton(self.pc_opts, variable=self.folders_var,
                                           text=_("Ordner homebrew, etaHEN/games und pkg anlegen"))
        self.folders_chk.pack(anchor="w", pady=(2, 0))
        row += 1

        self.pbar = ttk.Progressbar(frm, maximum=100)
        self.pbar.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(14, 4))
        row += 1
        ttk.Label(frm, textvariable=self.status_var, wraplength=560, justify="left").grid(
            row=row, column=0, columnspan=2, sticky="w")
        row += 1

        btns = ttk.Frame(frm)
        btns.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        self.tools = ttk.Frame(btns)   # nur am PC
        self.tools.pack(side="left")
        self.speed_btn = ttk.Button(self.tools, text=_("Tempo messen"), command=self._pc_speed)
        self.speed_btn.pack(side="left")
        self.rename_btn = ttk.Button(self.tools, text=_("Umbenennen"), command=self._pc_rename)
        self.rename_btn.pack(side="left", padx=(6, 0))
        ttk.Button(btns, text=_("Schließen"), command=self._on_close).pack(side="right", padx=(6, 0))
        self.start_btn = ttk.Button(btns, text=_("Formatieren …"), command=self._start)
        self.start_btn.pack(side="right")
        self.cancel_btn = ttk.Button(btns, text=_("Abbrechen"), command=self._cancel)

    def _set_lang(self, code):
        """Sprache wechseln: Oberfläche in der neuen Sprache neu aufbauen (nicht während eines Auftrags)"""
        if code == i18n.lang():
            return
        if self.busy or self.formatting or self.std_busy:
            self.lang_box.set(dict(i18n.LANGS)[i18n.lang()])
            messagebox.showinfo(self.title(), _("Die Sprache lässt sich nach dem laufenden Auftrag wechseln."),
                                parent=self)
            return
        self.settings["lang"] = code
        save_settings(self.settings)
        i18n.set_lang(code)
        self.std_panel.may_close()   # Abfrage-Schleife des alten Bereichs beenden
        consoles = list(self.consoles.values())
        picked = self.consoles.get(self.host_var.get().strip())
        self.outer.destroy()
        self._build_ui()
        self.consoles = {self._console_text(c): c for c in consoles}   # Anzeigetexte in der neuen Sprache
        self.host_entry.configure(values=list(self.consoles))
        if picked:
            self.host_var.set(self._console_text(picked))
        self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")) if self.api
                            else _("Nicht verbunden"))
        self._mode_changed(initial=True, relang=True)

    def _confirm(self, title, text, ok_text, ack=None):
        return confirm(self, title, text, ok_text, ack)

    def _keep_on_screen(self):
        keep_on_screen(self)   # die Bereiche sind verschieden hoch: wieder mittig

    def _std_busy_changed(self, busy):
        """Auftrag in "Am PC Standard": solange nicht umschalten"""
        self.std_busy = busy
        self._update()

    def _widen_dev_list(self):
        """Aufgeklappte Liste so breit wie der längste Eintrag (ttk nimmt sonst die Breite des Feldes)"""
        font = tkfont.nametofont("TkTextFont")
        need = max((font.measure(v) for v in self.dev_combo.cget("values")), default=0) + 40
        extra = max(0, need - self.dev_combo.winfo_width())
        ttk.Style(self).configure("Dev.TCombobox", postoffset=(0, 0, extra, 0))

    def _selected_device(self):
        i = self.dev_combo.current()
        return self.devices[i] if 0 <= i < len(self.devices) else None

    def _update(self):
        dev = self._selected_device()
        if self.fs_var.get() == "exFAT":
            self.cluster_var.set(_("64 Kilobytes"))
            self.label_hint.configure(text=_("Bis zu {n} Zeichen: A–Z, 0–9, Leerzeichen, - und _", n=11))
        else:
            self.cluster_var.set(_("{n} Kilobytes (Standard)", n=fat32_cluster_kb(dev["size"])) if dev
                                 else _("Standardgröße"))
            self.label_hint.configure(text=_("Bis zu 11 Zeichen, wird bei FAT32 großgeschrieben"))

        idle = not self.busy
        pc = self.mode_var.get() == "pc"

        def enable(w, on, *extra):
            w.state(list(extra) + (["!disabled"] if on else ["disabled"]))

        for w in (self.host_entry, self.label_entry, self.search_btn, self.verify_chk, self.folders_chk):
            enable(w, idle)
        for w in self.mode_btns:
            enable(w, idle and not self.std_busy)
        enable(self.lang_box, idle and not self.std_busy and not self.formatting, "readonly")
        enable(self.reload_btn, idle and (pc or self.api))
        enable(self.dev_combo, idle and self.devices, "readonly")
        enable(self.fs_combo, idle, "readonly")
        enable(self.start_btn, idle and dev and (pc or formattable(dev)))
        enable(self.speed_btn, idle and dev)
        enable(self.del_btn, idle and not pc and self.api and self.games_tree.selection())
        enable(self.uninstall_chk, idle)
        enable(self.rename_btn, idle and dev and any(v["fs"] in ("exFAT", "FAT32") for v in dev.get("volumes", [])))
        # Abbrechen nur bei Warten auf Abziehen, Echtheitsprüfung und Tempomessung (Formatieren selbst dauert Sekunden)
        if "cancellable" in self.ps5_flags:
            ps5_can = bool(self.ps5_flags["cancellable"])
        else:   # ältere Payloads: am (deutschen) Schritt erkennen
            ps5_can = is_replug_step(self.ps5_step) or bool(re.search(r"Echtheit|[Tt]empo|^Prüfen", self.ps5_step))
        ps5_cancellable = not pc and self.formatting and not self.pc_job and ps5_can
        if (self.pc_job and getattr(self.pc_job, "cancellable", False)) or ps5_cancellable:
            if not self.cancel_btn.winfo_ismapped():
                self.cancel_btn.state(["!disabled"])   # nach einem früheren Abbruch noch gesperrt
                self.cancel_btn.pack(side="right", padx=(0, 6))
        elif self.cancel_btn.winfo_ismapped():
            self.cancel_btn.pack_forget()

    def _mode_changed(self, initial=False, relang=False):
        mode = self.mode_var.get()
        pc = mode == "pc"
        if not initial:
            self.settings["mode"] = mode
            save_settings(self.settings)
        self.after_idle(self._keep_on_screen)   # die Bereiche sind verschieden hoch
        if mode == "std":   # eigener Bereich ohne PS5-Funktionen
            self.ps5_body.grid_remove()
            self.std_panel.grid()
            self.title(_("USB-Datenträger formatieren"))
            self.std_panel.activate()
            self._update()
            return
        self.std_panel.grid_remove()
        self.ps5_body.grid()
        self.title(_("PS5-USB formatieren"))
        for w in (self.ps5_caption, self.host_entry, self.search_btn, self.games_box, self.ps5_warn):
            w.grid_remove() if pc else w.grid()
        if pc:
            self.tools.pack(side="left", before=self.start_btn)
        else:
            self.tools.pack_forget()
        self.pbar["value"] = 0
        if pc:
            self.devices = []
            self.dev_combo.configure(values=[])
            self.dev_var.set("")
            self._pc_reload()
        else:
            self._fill_devices(self.ps5_devices)
            if self.api:
                self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")))
            elif relang:
                self.status_var.set(_("Nicht verbunden"))
            elif initial or not self.consoles:
                self.after(200 if initial else 0, self._search)
            else:
                self.status_var.set(_("Nicht verbunden"))
        self._update()

    def _ps5_dev_text(self, d):
        """Kurz, damit die Auswahlliste nichts abschneidet; Erklärungen stehen in den Rückfragen"""
        fmt = d.get("format") or ""
        fmt = {"leer": _("leer"), "keine Partition": _("ohne Partition"),
               "unbekannt": _("Format unbekannt")}.get(fmt, fmt)
        if d.get("label") and fmt:
            fmt += " " + quote(d["label"])
        where = _("gesperrt") if d["locked"] else _("eingebunden") if d["mounts"] else _("frei")
        parts = [f"{dev_title(d)} ({d['name']})", fmt_size(d["size"])] + ([fmt] if fmt else []) + [where]
        if d.get("fake_real"):
            parts.append(_("GEFÄLSCHT – echt nur {size}", size=fmt_size(d["fake_real"])))
        n = len(self.games.get(d["name"], []))
        if n:
            parts.append(_("1 Spiel") if n == 1 else _("{n} Spiele", n=n))
        return " · ".join(parts)

    def _show_games(self):
        """Spiele des gewählten PS5-Datenträgers in der Liste darunter"""
        self.games_tree.delete(*self.games_tree.get_children())
        dev = self._selected_device()
        if self.mode_var.get() != "ps5" or not dev:
            self.games_caption.configure(text=_("Spiele auf diesem Datenträger:"))
            return
        games = self.games.get(dev["name"], [])
        for g in sorted(games, key=lambda g: g["name"].lower()):
            kind = _("Ordner") if g["kind"] == "Ordner" else g["kind"]   # Art kommt deutsch vom Formatierer
            self.games_tree.insert("", "end", iid=g.get("key") or None, values=(g["name"] or g["id"], g["id"], kind,
                                                      fmt_size(g["size"]) if g["size"] else "–"))
        if games:
            text = _("Spiele auf diesem Datenträger: {n}", n=len(games))
        elif self.lib_loading:
            text = _("Spiele auf diesem Datenträger: werden gesucht …")
        else:
            text = _("Spiele auf diesem Datenträger: keine gefunden")
        self.games_caption.configure(text=text)

    def _relibrary(self):
        self.lib_loading = False
        self._load_library()

    def _load_library(self):
        """Spieleliste der PS5 im Hintergrund holen; läuft die Suche dort noch, später erneut"""
        if not self.api or self.lib_loading:
            return
        api = self.api
        self.lib_loading = True

        def work():
            data = None
            try:
                data = api.library()
            except (ConnectionError, ApiError):
                pass
            finally:   # immer melden - sonst bliebe lib_loading stehen und die Liste käme nie wieder
                self.q.put(("library", data if isinstance(data, dict) else None, api))

        threading.Thread(target=self._guard(work, background=True), daemon=True).start()

    def _fill_pc_devices(self, devices):
        prev = self._selected_device()
        self.devices = devices
        self.dev_combo.configure(values=[pc_dev_text(d) for d in devices])
        # am PC wird nie vorausgewählt: dort hängen oft Platten mit Daten (Spiele-Sicherung, Backups)
        idx = next((i for i, d in enumerate(devices) if prev and d["disk"] == prev.get("disk")
                    and d["size"] == prev.get("size") and d.get("model") == prev.get("model")
                    and d.get("serial", "") == prev.get("serial", "")), -1)
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_var.set("")
        if not devices:
            self.status_var.set(_("Kein USB-Datenträger am PC gefunden. Stick einstecken und „Aktualisieren“ "
                                  "klicken."))
        else:
            self.status_var.set(_("{n} USB-Datenträger am PC gefunden – bitte oben auswählen.", n=len(devices)))

    def _fill_devices(self, devices):
        for d in devices:   # ältere/andere Payload-Versionen: fehlende Felder ergänzen
            for k, default in (("name", "?"), ("model", ""), ("size", 0), ("locked", False), ("mounts", []),
                               ("format", ""), ("label", ""), ("fake_real", 0), ("ident", "")):
                d.setdefault(k, default)
        self.ps5_devices = devices
        if self.mode_var.get() == "pc":
            return
        prev = self._selected_device()
        if prev and "disk" in prev:
            prev = None
        self.devices = devices
        self.dev_combo.configure(values=[self._ps5_dev_text(d) for d in devices])
        idx = -1
        if prev:   # gleicher Name reicht nicht: steckt dahinter ein anderer Datenträger (anderer Stick, die
            # Spiele-SSD), wird nichts übernommen
            idx = next((i for i, d in enumerate(devices) if d["name"] == prev["name"] and d["size"] == prev["size"]
                        and d["model"] == prev["model"] and d.get("ident", "") == prev.get("ident", "")), -1)
        if idx < 0:
            # vorausgewählt wird nur ein freier Stick, nie ein eingebundener (z. B. die Spiele-SSD)
            free = [i for i, d in enumerate(devices) if formattable(d) and not d["mounts"]]
            idx = free[0] if len(free) == 1 else -1
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_var.set("")
        if not devices and self.api:
            self.status_var.set(_("Kein USB-Stick an der PS5 gefunden. Stick einstecken und „Aktualisieren“ "
                                  "klicken."))
        self._show_games()

    # ------------------------------------------------------------------ Aktionen

    def _run(self, work):
        self.busy = True
        self._update()
        threading.Thread(target=self._guard(work), daemon=True).start()

    def _guard(self, work, background=False):
        """background: Spieleliste/Wächter - deren Fehler dürfen laufende Aufträge (busy) nicht freigeben"""
        def run():
            try:
                work()
            except Exception as e:  # unerwartete Fehler im Thread sichtbar machen
                self.q.put(("bgerror" if background else "error", _("Unerwarteter Fehler: {e}", e=e)))
        return run

    @staticmethod
    def _console_text(c):
        extra = " (" + _("Formatierer läuft") + ")" if c["formatter"] else \
            " (" + _("Ruhemodus") + ")" if c["standby"] else ""
        return f"{c['name']} – {c['ip']}{extra}"

    def _search(self):
        if self.busy:
            return

        def work():
            self.q.put(("status", _("Suche PS5 im Netzwerk …")))
            self.q.put(("found", ps5_discovery.discover(API_PORT)))

        self._run(work)

    def _on_found(self, consoles):
        self.consoles = {self._console_text(c): c for c in consoles}
        self.host_entry.configure(values=list(self.consoles))
        saved = self.settings.get("host")
        if len(consoles) == 1:
            pick = consoles[0]
        else:
            pick = next((c for c in consoles if c["ip"] == saved), None)
        if pick:
            self.host_var.set(self._console_text(pick))
            self._connect()
        elif consoles:
            self.host_var.set("")
            self.status_var.set(_("{n} PS5 gefunden – bitte oben auswählen.", n=len(consoles)))
        elif saved:
            self.host_var.set(saved)
            self.status_var.set(_("Keine PS5 gefunden – versuche zuletzt verwendete Adresse {host} …", host=saved))
            self._connect()
        else:
            self.status_var.set(_("Keine PS5 gefunden. Ist sie eingeschaltet und im selben Netzwerk? "
                                  "Die IP-Adresse kann auch oben eingegeben werden (Enter)."))

    def _connect(self):
        if self.busy:
            return
        text = self.host_var.get().strip()
        console = self.consoles.get(text)
        host = console["ip"] if console else text
        m = IP_RE.match(host)
        if not m or any(int(x) > 255 for x in m.groups()):
            messagebox.showerror(self.title(), _("Bitte eine gefundene PS5 auswählen oder ihre IP-Adresse eingeben, "
                                                 "z. B. 192.168.178.40.\n\nSie steht auf der PS5 unter Einstellungen › "
                                                 "Netzwerk › Verbindungsstatus."), parent=self)
            return
        if console and console["standby"] and not console["formatter"]:
            messagebox.showerror(self.title(), _("{name} ist im Ruhemodus. Bitte die PS5 einschalten, den ELF-Loader "
                                                 "starten und dann „Suchen“ klicken.", name=console["name"]),
                                 parent=self)
            return
        self.settings["host"] = host
        save_settings(self.settings)
        api = Api(host)
        # alte Verbindung vergessen: scheitert die neue, darf "Starten" nicht die vorige PS5 formatieren
        self.api = None
        self.games = {}
        self.lib_known = self.lib_loading = False   # Spieleliste gehört zur alten PS5
        self._fill_devices([])

        def work():
            self.q.put(("status", _("Verbinde mit {host} …", host=host)))
            try:
                self.q.put(("connected", api, api.devices(timeout=2)))
                return
            except ApiError as e:
                self.q.put(("error", str(e)))
                return
            except Refused:
                pass
            except ConnectionError:   # keine Antwort in 2 s: läuft er vielleicht schon (WLAN-Aussetzer)? Geduldiger fragen
                try:
                    self.q.put(("connected", api, api.devices(timeout=8)))
                    return
                except ApiError as e:
                    self.q.put(("error", str(e)))
                    return
                except ConnectionError:
                    pass

            payload = find_payload()
            if not payload:
                self.q.put(("error", _("usb_format.elf nicht gefunden. Sie muss neben dem Programm liegen.")))
                return
            self.q.put(("status", _("Formatierer läuft noch nicht – sende Payload an die PS5 …")))
            try:
                send_payload(host, payload)
            except OSError:
                self.q.put(("error", _("Keine Verbindung zu {host}. Läuft auf der PS5 ein ELF-Loader (Port {port}) "
                                       "und ist die IP-Adresse richtig?", host=host, port=LOADER_PORT)))
                return
            for _attempt in range(20):
                time.sleep(0.5)
                try:
                    self.q.put(("connected", api, api.devices(timeout=2)))
                    return
                except (ConnectionError, ApiError):
                    pass
            self.q.put(("error", _("Der Payload wurde gesendet, antwortet aber nicht. "
                                   "Bitte die PS5-Benachrichtigungen prüfen.")))

        self._run(work)

    def _reload(self):
        if self.mode_var.get() == "pc":
            self._pc_reload()
            return
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                self.q.put(("devices", api.devices()))
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Geräteliste konnte nicht geladen werden: {e}", e=e)))

        self._run(work)

    def _start(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return
        if self.mode_var.get() == "pc":
            self._pc_format(dev)
            return
        label = self.label_var.get()
        fs = "exfat" if self.fs_var.get() == "exFAT" else "fat32"
        fs_text = _("exFAT mit 64 KB Zuordnungseinheiten") if fs == "exfat" else "FAT32"
        replug = bool(dev["mounts"])
        replug_text = _("Die PS5 nutzt diesen Datenträger gerade. Der Formatierer meldet ihn per Software kurz ab "
                        "und übernimmt ihn, bevor die PS5 ihn wieder einbindet. Klappt das nicht, bittet er, den "
                        "Stick an der PS5 abzuziehen und wieder einzustecken. Spiele und Apps, die davon laufen, "
                        "vorher beenden.") + "\n\n" if replug else ""
        limit = dev.get("fake_real") or 0
        if limit:
            replug_text = _("GEFÄLSCHTER STICK: Er meldet {size}, hat aber nur {real} echten Speicher. Es wird nur "
                            "dieser Bereich formatiert. Für Spiele an der PS5 besser einen echten Datenträger "
                            "verwenden.", size=fmt_size(dev["size"]), real=fmt_size(limit)) + "\n\n" + replug_text
        games = sorted(g["name"] or g["id"] for g in self.games.get(dev["name"], []))
        ack = None
        if games:
            shown = "\n".join("• " + g for g in games[:8])
            if len(games) > 8:
                shown += "\n" + _("… und {n} weitere", n=len(games) - 8)
            head = _("Darauf liegt 1 Spiel, das gelöscht wird:") if len(games) == 1 else \
                _("Darauf liegen {n} Spiele, die gelöscht werden:", n=len(games))
            replug_text = f"{head}\n{shown}\n\n" + replug_text
            ack = _("Ich weiß, dass diese Spiele unwiderruflich gelöscht werden")
        elif dev["mounts"] and (self.lib_loading or not self.lib_known):
            # Inhalt noch unbekannt: lieber ausdrücklich bestätigen lassen
            replug_text = _("Der Inhalt wird noch durchsucht – möglicherweise liegen Spiele darauf.") + "\n\n" + \
                replug_text
            ack = _("Ich weiß, dass alles darauf unwiderruflich gelöscht wird")
        verify = bool(self.verify_var.get())
        if verify:
            replug_text += _("Vorher werden Echtheit und Tempo geprüft (einige Minuten).") + "\n\n"
            limit = 0   # die Prüfung testet den ganzen Stick erneut
        if not self._confirm(
                _("Alle Daten werden gelöscht"),
                _("{name} ({dev}, {size}) wird als {fs} formatiert.", name=dev_title(dev), dev=dev["name"],
                  size=fmt_size(dev["size"]), fs=fs_text) + "\n\n" + replug_text +
                _("Das lässt sich nicht rückgängig machen."),
                _("Formatieren"), ack):
            return
        if self.busy or not self.api:   # während der Rückfrage kann ein anderer Auftrag begonnen haben
            return
        self.settings.update(label=label, verify=verify, folders=self.folders_var.get())
        save_settings(self.settings)
        folders = bool(self.folders_var.get())

        def after(ok, msg, result):
            """eigenes Formatieren: bei gefälschtem Stick anbieten, nur den echten Bereich zu nutzen"""
            if ok:
                messagebox.showinfo(_("PS5-USB formatieren"), _("Datenträger erfolgreich formatiert.") + "\n\n" + msg,
                                    parent=self)
            elif result.get("fake") and result.get("real", 0) >= 64 * 1024 * 1024:
                real = result["real"]
                if self._confirm(_("Gefälschter Speicher erkannt"),
                                 msg + "\n\n" + _("Nur die echten {size} formatieren? Für Spiele an der PS5 besser "
                                                  "einen echten Datenträger verwenden.", size=fmt_size(real)),
                                 _("Nur echte {size} formatieren", size=fmt_size(real))):
                    self._ps5_format_job(dev, fs, label, folders, False, real, after)
            else:
                messagebox.showerror(_("PS5-USB formatieren"), msg, parent=self)

        self._ps5_format_job(dev, fs, label, folders, verify, limit, after)

    def _ps5_format_job(self, dev, fs, label, folders, verify, limit, after):
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                api.format(dev["name"], fs, label, dev["size"], bool(dev["mounts"]), limit, folders, verify,
                           dev.get("ident", ""))
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Formatieren konnte nicht gestartet werden: {e}", e=e)))
                return
            self._follow(api, foreign=False, after=after)

        self.pbar["value"] = 0
        self._run(work)

    def _delete_games(self):
        dev = self._selected_device()
        keys = list(self.games_tree.selection())
        if self.busy or not dev or not keys or not self.api:
            return
        games = {g.get("key"): g for g in self.games.get(dev["name"], [])}
        names = sorted((games[k]["name"] or games[k]["id"]) for k in keys if k in games)
        if not names:
            return
        uninstall = self.uninstall_var.get()
        shown = "\n".join("• " + n for n in names[:10])
        if len(names) > 10:
            shown += "\n" + _("… und {n} weitere", n=len(names) - 10)
        head = _("Dieses Spiel wird von {name} ({dev}) gelöscht:", name=dev_title(dev), dev=dev["name"]) \
            if len(names) == 1 else _("Diese {n} Spiele werden von {name} ({dev}) gelöscht:", n=len(names),
                                      name=dev_title(dev), dev=dev["name"])
        tiles = _("Die Kacheln werden auch von der PS5-Startseite entfernt.") if uninstall else \
            _("Die Kacheln bleiben auf der Startseite (ohne Spiel).")
        if not messagebox.askokcancel(
                _("Spiele löschen"),
                f"{head}\n\n{shown}\n\n{tiles}\n\n" + _("Laufende Spiele vorher beenden. Das lässt sich nicht "
                                                        "rückgängig machen."),
                icon="warning", default="cancel", parent=self):
            return
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                api.delete_games(keys, uninstall)
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Löschen konnte nicht gestartet werden: {e}", e=e)))
                return
            self._follow(api, foreign=False, done_title=_("Spiele gelöscht."))

        self.pbar["value"] = 0
        self._run(work)

    def _follow(self, api, foreign, done_title=None, after=None):
        """Auftrag auf der PS5 bis zum Ende verfolgen (im Thread). foreign: woanders gestartet (PS5-Browser,
        anderer PC) - dann nur anzeigen, ohne Rückfrage-Dialoge des Starters."""
        done_title = done_title or _("Datenträger erfolgreich formatiert.")
        self.q.put(("formatting", foreign))
        last_ok = time.time()
        while True:
            time.sleep(0.5)
            try:
                st = api.status()
                last_ok = time.time()
            except (ConnectionError, ApiError):
                # WLAN-Aussetzer oder PC kurz im Ruhezustand: der Auftrag läuft auf der PS5 weiter - geduldig
                # weiterfragen und erst nach 3 Minuten aufgeben (mit ehrlicher Meldung)
                offline = time.time() - last_ok
                if offline >= 180:
                    self.q.put(("finished", False, _("Keine Verbindung zur PS5 seit 3 Minuten. Der Auftrag läuft dort "
                                                     "vermutlich weiter – den Stick NICHT abziehen. Später „Suchen“ "
                                                     "klicken, um das Ergebnis zu sehen."), foreign, done_title))
                    return
                if offline >= 5:
                    self.q.put(("status", _("Verbindung zur PS5 unterbrochen – der Auftrag läuft dort weiter, frage "
                                            "weiter nach …")))
                time.sleep(min(2.0, offline / 10))   # sanft weiterfragen
                continue
            if not st.get("busy") and not st.get("done"):
                # Formatierer wurde zwischendurch neu gestartet: Ergebnis unbekannt
                self.q.put(("finished", False, _("Der Formatierer auf der PS5 wurde zwischendurch neu gestartet – das "
                                                 "Ergebnis ist unbekannt. Bitte den Datenträger prüfen bzw. erneut "
                                                 "formatieren."), foreign, done_title))
                return
            flags = {k: st[k] for k in ("replug", "cancellable") if k in st}
            self.q.put(("progress", int(st.get("percent") or 0), st.get("step") or "", flags))
            if not st.get("busy"):
                try:   # erst die Liste, dann das Ergebnis - sonst überschreibt der Listenhinweis die Meldung
                    self.q.put(("devices", api.devices()))
                except (ConnectionError, ApiError):
                    pass
                result = st.get("result") if isinstance(st.get("result"), dict) else {}
                self.q.put(("finished", bool(st.get("ok")), st.get("message") or "", foreign, done_title, result,
                            after))
                return

    def _watch(self):
        """Alle 3 Sekunden nachsehen, ob auf der PS5 ein Auftrag läuft, den jemand anderes gestartet hat"""
        if self.api and not self.busy and not self.watching and self.mode_var.get() == "ps5":
            api = self.api
            self.watching = True

            def check():
                try:
                    if api.status().get("busy"):
                        self.q.put(("foreign", api))
                except (ConnectionError, ApiError):
                    pass
                finally:   # sonst wäre der Wächter nach einem unerwarteten Fehler für immer aus
                    self.watching = False

            threading.Thread(target=self._guard(check, background=True), daemon=True).start()
        self._watch_id = self.after(3000, self._watch)

    # ------------------------------------------------------------------ PC-Modus

    def _pc_reload(self):
        if self.busy:
            return

        def work():
            self.q.put(("status", _("Suche USB-Datenträger am PC …")))
            try:
                self.q.put(("pcdevices", pc_list()))
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as e:
                self.q.put(("error", _("USB-Datenträger konnten nicht gelesen werden: {e}", e=e)))

        self._run(work)

    def _pc_run(self, op, args, cancellable=False, after=None):
        """Auftrag mit Administratorrechten ausführen; after(ok, meldung, info) läuft danach im UI-Thread"""
        hwnd = hwnd_of(self)

        def work():
            try:
                job = PcJob(args, hwnd)
            except (OSError, RuntimeError) as e:
                self.q.put(("error", str(e)))
                return
            job.cancellable = cancellable
            self.pc_job = job
            self.q.put(("formatting", False))
            ok, msg, info = job.follow(lambda pct, step: self.q.put(("progress", pct, step, {})))
            try:
                devices = pc_list()
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError):
                devices = None
            self.q.put(("pcdone", op, ok, msg, info, devices, after))

        self.pbar["value"] = 0
        self.status_var.set(admin_wait_text())
        self._run(work)

    def _pc_format(self, dev, limit=0):
        label = self.label_var.get()
        fs = "exfat" if self.fs_var.get() == "exFAT" else "fat32"
        fs_text = _("exFAT mit 64 KB Zuordnungseinheiten") if fs == "exfat" else "FAT32"
        if not limit:
            vols = pc_volumes_text(dev)
            used = pc_used(dev)
            has_data = used > 64 * 1024 * 1024
            data_warn = "\n\n" + _("Auf dem Datenträger liegen {size} Daten – sie sind danach weg.",
                                   size=fmt_size(used)) if has_data else ""
            head = _("{model} (Datenträger {disk}, {size})", model=dev["model"] or _("USB-Datenträger"),
                     disk=disk_name(dev), size=fmt_size(dev["size"]))
            if not self._confirm(
                    _("Alle Daten werden gelöscht"),
                    f"{head}\n{vols + chr(10) if vols else ''}\n" +
                    _("wird als {fs} für die PS5 formatiert.", fs=fs_text) + data_warn + "\n\n" +
                    irreversible_admin_text(),
                    _("Formatieren"),
                    _("Ich weiß, dass die {size} Daten unwiderruflich gelöscht werden", size=fmt_size(used))
                    if has_data else None):
                return
        if self.busy or self.mode_var.get() != "pc":
            return
        self.settings.update(label=label, verify=self.verify_var.get(), folders=self.folders_var.get())
        save_settings(self.settings)
        args = ["format", str(dev["disk"]), str(dev["size"]), fs, label, *serial_args(dev)]
        if self.verify_var.get() and not limit:
            args.append("verify")
        if self.folders_var.get():
            args.append("folders")
        if limit:
            args.append(f"limit={limit}")

        def after(ok, msg, info):
            if ok:
                messagebox.showinfo(_("USB-Datenträger am PC"), _("Datenträger erfolgreich formatiert.") + "\n\n" + msg,
                                    parent=self)
            elif info.get("fake") and info.get("real", 0) >= 64 * 1024 * 1024:
                real = info["real"] // (1024 * 1024) * 1024 * 1024
                if messagebox.askyesno(
                        _("Gefälschte Größe"), msg + "\n\n" + _(
                            "Nur den echten Bereich ({size}) formatieren? Dann gehen keine Daten verloren, die über "
                            "diese Grenze hinaus geschrieben würden.", size=fmt_size(real)),
                        icon="warning", parent=self):
                    self._pc_format(dev, limit=real)
            else:
                messagebox.showerror(_("USB-Datenträger am PC"), msg, parent=self)

        self._pc_run("format", args, cancellable=bool(self.verify_var.get()) and not limit, after=after)

    def _pc_speed(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Tempo messen"), msg, parent=self)

        self._pc_run("speed", ["speed", str(dev["disk"]), str(dev["size"]), *serial_args(dev)], cancellable=True,
                     after=after)

    def _pc_rename(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return
        current = next((v["label"] for v in dev["volumes"] if v["fs"] in ("exFAT", "FAT32")), "")
        label = ask_string(self, _("Umbenennen"), _("Neue Bezeichnung (bis zu 11 Zeichen: A–Z, 0–9, Leerzeichen, - "
                                                    "und _; leer = Bezeichnung entfernen):"), current)
        if label is None or label == current:
            return
        if not LABEL_RE.match(label):
            messagebox.showerror(_("Umbenennen"), _("Nur A–Z, 0–9, Leerzeichen, - und _ erlaubt (höchstens {n} "
                                                    "Zeichen).", n=11), parent=self)
            return
        target = quote(label) if label else _("keine Bezeichnung (wird entfernt)")
        if not self._confirm(
                _("Umbenennen"), pc_dev_text(dev) + "\n\n" + _("Neue Bezeichnung: {label}", label=target) + "\n\n" +
                _("Die Daten bleiben erhalten. Das Laufwerk wird dafür kurz abgemeldet – geöffnete Dateien darauf "
                  "vorher schließen."),
                _("Umbenennen") if label else _("Entfernen")):
            return
        if self.busy or self.mode_var.get() != "pc":
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Umbenennen"), msg, parent=self)

        self._pc_run("label", ["label", str(dev["disk"]), str(dev["size"]), label, *serial_args(dev)], after=after)

    def _cancel(self):
        """Abbrechen: am PC über die Abbruch-Datei des Helfers, an der PS5 über /api/cancel"""
        self.cancel_btn.state(["disabled"])
        self.status_var.set(_("Wird abgebrochen …"))
        if self.pc_job:
            self.pc_job.cancel()
        elif self.api:
            api = self.api

            def work():
                try:
                    api.cancel()
                except (ConnectionError, ApiError) as e:
                    self.q.put(("bgerror", _("Abbrechen nicht möglich: {e}", e=e)))

            threading.Thread(target=self._guard(work, background=True), daemon=True).start()

    # ------------------------------------------------------------------ Ereignisse aus Threads

    def _poll(self):
        try:
            while True:
                try:
                    msg = self.q.get_nowait()
                except queue.Empty:
                    break
                try:
                    self._handle(msg)
                except Exception as e:   # eine kaputte Nachricht (z. B. ältere PS5-Version) darf die Schleife nicht stoppen
                    self.status_var.set(_("Unerwarteter Fehler: {e}", e=e))
                self._update()
        finally:
            self._poll_id = self.after(50, self._poll)

    def _handle(self, msg):
        kind = msg[0]
        if kind == "status":
            self.status_var.set(msg[1])
        elif kind == "found":
            self.busy = False
            self._on_found(msg[1])
        elif kind == "connected":
            self.api = msg[1]
            self.busy = False
            self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")))
            self._fill_devices(msg[2].get("devices") or [])
            self._load_library()
        elif kind == "devices":
            self.busy = self.formatting
            self._fill_devices(msg[1].get("devices") or [])
            self._load_library()
        elif kind == "formatting":
            self.formatting = True
        elif kind == "foreign":
            # woanders gestartet: hier mitverfolgen (nur bei der PS5, die jetzt verbunden ist)
            if not self.busy and self.mode_var.get() == "ps5" and msg[1] is self.api:
                self.status_var.set(_("Auf der PS5 läuft ein Auftrag …"))
                self.pbar["value"] = 0
                api = msg[1]
                self._run(lambda: self._follow(api, foreign=True))
        elif kind == "progress":
            pct, step = msg[1], msg[2]
            flags = msg[3] if len(msg) > 3 else {}
            # Schritte ohne Prozentangabe (Warten auf das Abmelden, Werkzeuge): Lauf-Animation statt Stillstand
            moving = is_replug_step(step, flags) or (pct == 0 and step not in ("", "Fertig", _("Fertig")))
            if moving and str(self.pbar["mode"]) != "indeterminate":
                self.pbar.configure(mode="indeterminate")
                self.pbar.start(12)
            elif not moving:
                if str(self.pbar["mode"]) != "determinate":
                    self.pbar.stop()
                    self.pbar.configure(mode="determinate")
                self.pbar["value"] = pct
            self.status_var.set(f"{step} …" if moving else f"{step} … {pct} %")
            self.ps5_step = step   # für "Abbrechen" (Warten auf Abziehen, Prüfung, Messung)
            self.ps5_flags = flags
        elif kind == "finished":
            self.formatting = False
            self.busy = False
            self.pbar.stop()
            self.pbar.configure(mode="determinate")
            self.status_var.set(msg[2])
            foreign = len(msg) > 3 and msg[3]
            done_title = msg[4] if len(msg) > 4 else _("Datenträger erfolgreich formatiert.")
            result = msg[5] if len(msg) > 5 else {}
            after = msg[6] if len(msg) > 6 else None
            self.ps5_step = ""
            self.ps5_flags = {}
            if after and not foreign:   # der Auftraggeber entscheidet selbst (z. B. Fälschung: nur echten Bereich)
                self.pbar["value"] = 100 if msg[1] else 0
                self._update()
                after(msg[1], msg[2], result)
            elif msg[1]:
                self.pbar["value"] = 100
                if not foreign:
                    messagebox.showinfo(_("PS5-USB formatieren"), done_title + "\n\n" + msg[2], parent=self)
            else:
                self.pbar["value"] = 0
                if not foreign:
                    messagebox.showerror(_("PS5-USB formatieren"), msg[2], parent=self)
        elif kind == "library" and len(msg) > 2 and msg[2] is not self.api:
            pass   # Antwort einer vorher verbundenen PS5: verwerfen
        elif kind == "library":
            self.lib_loading = False
            data = msg[1]
            if data is not None:
                games = {}
                for t in data.get("titles", []):
                    if t.get("device"):   # nur Datenträger, nicht der interne Ordner /data/homebrew
                        games.setdefault(t["device"], []).append(t)
                self.games = games
                self.lib_known = True
                if data.get("scanning"):
                    self.lib_loading = True   # Anzeige "werden gesucht" bis zum nächsten Abruf
                    self.after(2000, self._relibrary)
            if self.mode_var.get() == "ps5" and self.ps5_devices:
                i = self.dev_combo.current()
                self.dev_combo.configure(values=[self._ps5_dev_text(d) for d in self.ps5_devices])
                if 0 <= i < len(self.ps5_devices):
                    self.dev_combo.current(i)
            self._show_games()
        elif kind == "pcdevices":
            self.busy = False
            if self.mode_var.get() == "pc":
                self._fill_pc_devices(msg[1])
        elif kind == "pcdone":
            _k, op, ok, text, info, devices, after = msg
            self.pc_job = None
            self.formatting = self.busy = False
            self.pbar.stop()
            self.pbar.configure(mode="determinate")
            self.pbar["value"] = 100 if ok else 0
            if devices is not None and self.mode_var.get() == "pc":
                self._fill_pc_devices(devices)
            self.status_var.set(text)
            self._update()
            if after:
                after(ok, text, info)
        elif kind == "error":
            self.busy = self.formatting = False
            self.pc_job = None
            self.pbar.stop()
            self.pbar.configure(mode="determinate")
            self.status_var.set(msg[1])
            messagebox.showerror(_("PS5-USB formatieren"), msg[1], parent=self)
        elif kind == "bgerror":   # Hintergrund (Spieleliste, Wächter): laufende Aufträge nicht berühren
            self.status_var.set(msg[1])

    def _on_close(self):
        if self.formatting and not messagebox.askyesno(
                self.title(), (_("Der Auftrag läuft am PC im Hintergrund weiter.") if self.pc_job else
                               _("Die Formatierung läuft auf der PS5 weiter.")) + " " +
                _("Fenster trotzdem schließen?"),
                icon="warning", parent=self):
            return
        if not self.std_panel.may_close():   # Auftrag in "Am PC Standard" (fragt selbst nach)
            return
        if self.pc_job and getattr(self.pc_job, "cancellable", False):
            self.pc_job.cancel()   # Prüfung/Tempo am PC nicht ohne Fenster weiterlaufen lassen
        self.after_cancel(self._poll_id)
        self.after_cancel(self._watch_id)
        self.destroy()

    def _on_exception(self, exc, val, tb):
        messagebox.showerror(self.title(), _("Unerwarteter Fehler: {e}", e=val), parent=self)


if __name__ == "__main__":
    enable_dpi_awareness()
    App().mainloop()
