"""„Am PC – frei“: USB-Datenträger am PC frei formatieren – exFAT, FAT32, NTFS, FAT (FAT16), ext2/3/4 (Linux) oder
HFS+ (Mac), jede Zuordnungseinheit, MBR oder GPT, Schnell- oder vollständige Formatierung; im Expertenmodus mehrere
Partitionen mit Typ und Startkennung. Ohne PS5-Optionen.
Dritter Modus des PS5-USB-Formatierers und einziger Inhalt des eigenständigen USB-Formatierers."""
import os
import queue
import subprocess
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog, ttk

import fsinfo
from fsinfo import (FILESYSTEMS, KB, LABEL_CHARS, MB, SCHEMES, available_filesystems, cluster_choices, mbr_limit,
                    partition_bytes, unit_text, uses_gpt)
import ui_msgbox as messagebox
import ui_theme
from i18n import _, quote
from partition_editor import PartitionEditor
from pc_common import (IMAGE_EXT, IS_WIN, PcJob, PlainJob, admin_wait_text, ask_string, confirm, data_kept_admin_text,
                       disk_name, fmt_size, hwnd_of, image_file_name, image_info, irreversible_admin_text, on_device,
                       history_load, last_job, last_job_text, pc_dev_text, pc_list, pc_used, pc_volumes_text,
                       record_job, serial_args, show_scan)

from ui_widgets import ChoiceChips, DriveList, Tooltip

__all__ = ["StandardPanel", "SCHEMES", "FILESYSTEMS", "KB", "MB"]

# "Wofür soll der Datenträger dienen?" - Kachel -> Dateisystem (bzw. eigene Aufteilung im Expertenmodus)
PURPOSES = [("exFAT", "Dateien tauschen"), ("FAT32", "Ältere Geräte"), ("NTFS", "Windows"),
            ("ext4 (Linux)", "Linux"), ("HFS+ (Mac OS Extended)", "Mac"), ("expert", "Eigene Aufteilung")]


class StandardPanel(ttk.Frame):
    def __init__(self, master, settings, save, on_busy=None, title=None):
        super().__init__(master)
        self.settings = settings        # eigenes Wörterbuch für diesen Bereich
        self.save = save
        self.on_busy = on_busy or (lambda busy: None)
        self.title_text = title or _("USB-Datenträger formatieren")
        self.q = queue.Queue()
        self.devices = []
        self.busy = False
        self.job = None                 # laufender Auftrag des PC-Helfers
        self._bar_reset = None          # Zeitgeber: Balken nach dem Einlesen leeren
        self.cluster_values = []        # Bytes zu den Einträgen der Auswahlliste

        fs = settings.get("fs") if settings.get("fs") in FILESYSTEMS else "exFAT"
        self.fs_var = tk.StringVar(value=fs)
        self.cluster_var = tk.StringVar()
        self.scheme_var = tk.StringVar()
        self.label_var = tk.StringVar(value=settings.get("label", ""))
        self.quick_var = tk.BooleanVar(value=settings.get("quick", True))
        self.verify_var = tk.BooleanVar(value=settings.get("verify", False))
        self.status_var = tk.StringVar(value="")
        self.scheme_key = settings.get("scheme") if settings.get("scheme") in dict(SCHEMES) else "auto"
        self.cluster_pick = {}          # Dateisystem -> zuletzt gewählte Größe (für dieses Laufwerk)
        self.expert_var = tk.BooleanVar(value=bool(settings.get("expert", False)))
        self._build()
        self._expert_changed(initial=True)
        self._refresh_options()
        self._poll_id = self.after(50, self._poll)
        # wird der Bereich zerstört (Sprachwechsel, Fenster zu), endet auch die Abfrage-Schleife
        self.bind("<Destroy>", lambda e: self.after_cancel(self._poll_id) if e.widget is self else None)

    # ------------------------------------------------------------------ Oberfläche

    def _build(self):
        """Links Laufwerke und Werkzeuge, rechts "Formatieren", darunter der Fortschritt - alles auf Karten"""
        self.configure(style="Page.TFrame")
        gap, px = ui_theme.px(12), ui_theme.px
        self.columnconfigure(0, weight=5, uniform="col")
        self.columnconfigure(1, weight=6, uniform="col")
        self.rowconfigure(0, weight=1)
        left = ttk.Frame(self, style="Page.TFrame")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, gap))
        left.columnconfigure(0, weight=1)
        left.rowconfigure(0, weight=1)

        # ---- Laufwerke
        drives = ui_theme.card(left)
        drives.grid(row=0, column=0, sticky="nsew")
        head = ttk.Frame(drives)
        head.pack(fill="x", pady=(0, px(8)))
        ttk.Label(head, text=_("Laufwerke").upper(), style="Section.TLabel").pack(side="left")
        ico = ui_theme.icon(self, "refresh", "muted", 15)
        self.reload_btn = ttk.Button(head, text=" " + _("Aktualisieren"), image=ico, compound="left",
                                     style="Ghost.TButton", command=self.reload)
        self.reload_btn.image = ico
        self.reload_btn.pack(side="right")
        Tooltip(self.reload_btn, lambda: self._why(self.reload_btn, need_dev=False) or _("Laufwerke neu einlesen (F5)"))
        self.dev_combo = DriveList(drives, empty_text=_("Kein USB-Datenträger am PC gefunden. Stick einstecken und "
                                                        "„Aktualisieren“ klicken."))
        self.dev_combo.pack(fill="both", expand=True)
        self.dev_combo.bind("<<ComboboxSelected>>", lambda e: self._device_changed())

        # ---- Werkzeuge
        tools = ui_theme.card(left)
        tools.grid(row=1, column=0, sticky="ew", pady=(gap, 0))
        tools.columnconfigure((0, 1, 2), weight=1, uniform="tool")
        ttk.Label(tools, text=_("Werkzeuge").upper(), style="Section.TLabel").grid(row=0, column=0, columnspan=3,
                                                                                   sticky="w", pady=(0, px(8)))
        self.speed_btn = ttk.Button(tools, text=_("Tempo messen"), command=self._speed)
        self.speed_btn.grid(row=1, column=0, sticky="ew")
        self.rename_btn = ttk.Button(tools, text=_("Umbenennen"), command=self._rename)
        self.rename_btn.grid(row=1, column=1, sticky="ew", padx=(px(6), 0))
        ttk.Label(tools, text=_("Image-Sicherung").upper(), style="Section.TLabel").grid(
            row=2, column=0, columnspan=3, sticky="w", pady=(px(12), px(8)))
        self.backup_btn = ttk.Button(tools, text=_("Sichern …"), command=self._backup)
        self.backup_btn.grid(row=3, column=0, sticky="ew")
        self.restore_btn = ttk.Button(tools, text=_("Wiederherstellen …"), command=self._restore)
        self.restore_btn.grid(row=3, column=1, sticky="ew", padx=(px(6), 0))
        self.verify_btn = ttk.Button(tools, text=_("Prüfen …"), command=self._verify)
        self.verify_btn.grid(row=3, column=2, sticky="ew", padx=(px(6), 0))
        for btn, text in ((self.speed_btn, _("Lese- und Schreibtempo messen (die Daten bleiben erhalten)")),
                          (self.rename_btn, _("Bezeichnung des Laufwerks ändern")),
                          (self.backup_btn, _("Gewähltes Laufwerk als Image (.img.zst) sichern")),
                          (self.restore_btn, _("Ein Image auf das gewählte Laufwerk zurückschreiben")),
                          (self.verify_btn, _("Eine Image-Datei ganz lesen und ihre Prüfsummen vergleichen – ohne "
                                              "Datenträger und ohne Administratorrechte"))):
            Tooltip(btn, lambda b=btn, t=text: self._why(b) or t)

        # ---- Formatieren
        right = self.form = ui_theme.card(self)
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(1, weight=1)
        row = 0
        wrap = px(400)

        def hint(parent, r, col=0, span=2):
            lbl = ttk.Label(parent, style="Hint.TLabel", wraplength=wrap, justify="left")
            lbl.grid(row=r, column=col, columnspan=span, sticky="w", pady=(px(3), 0))
            lbl.grid_remove()
            return lbl

        ttk.Label(right, text=_("Formatieren"), style="H2.TLabel").grid(row=row, column=0, columnspan=2, sticky="w")
        row += 1
        ttk.Label(right, text=_("Wofür soll der Datenträger dienen?"), style="Muted.TLabel").grid(
            row=row, column=0, columnspan=2, sticky="w", pady=(px(10), px(6)))
        row += 1
        self.purpose = ChoiceChips(right, [(k, _(t)) for k, t in PURPOSES], self._purpose_picked)
        self.purpose.grid(row=row, column=0, columnspan=2, sticky="ew")
        row += 1
        ttk.Separator(right).grid(row=row, column=0, columnspan=2, sticky="ew", pady=px(12))
        row += 1

        # Beschriftungen links gleich breit - auch über die getrennten Bereiche (einfach/Experte, Schema) hinweg
        labels = (_("Dateisystem"), _("Zuordnungseinheit"), _("Bezeichnung"), _("Partitionsschema"))
        label_w = max(tkfont.nametofont("TkDefaultFont").measure(x) for x in labels) + px(14)

        def field(parent, r, text):
            ttk.Label(parent, text=text, style="Muted.TLabel").grid(row=r, column=0, sticky="w",
                                                                     padx=(0, px(12)), pady=(px(4), 0))

        # einfacher Modus: eine Partition über den ganzen Datenträger
        sb = self.simple_box = ttk.Frame(right)
        sb.grid(row=row, column=0, columnspan=2, sticky="ew")
        sb.columnconfigure(1, weight=1)
        sb.columnconfigure(0, minsize=label_w)
        row += 1
        field(sb, 0, _("Dateisystem"))
        self.fs_combo = ttk.Combobox(sb, textvariable=self.fs_var, state="readonly")
        self.fs_combo.grid(row=0, column=1, sticky="ew", pady=(px(4), 0))
        self.fs_combo.bind("<<ComboboxSelected>>", lambda e: self._fs_picked())
        self.fs_hint = hint(sb, 1, col=1, span=1)
        field(sb, 2, _("Zuordnungseinheit"))
        self.cluster_combo = ttk.Combobox(sb, textvariable=self.cluster_var, state="readonly")
        self.cluster_combo.grid(row=2, column=1, sticky="ew", pady=(px(8), 0))
        self.cluster_combo.bind("<<ComboboxSelected>>", lambda e: self._cluster_changed())
        field(sb, 3, _("Bezeichnung"))
        vcmd = (self.register(lambda p: len(p) <= self._label_max() and bool(LABEL_CHARS.match(p))), "%P")
        self.label_entry = ttk.Entry(sb, textvariable=self.label_var, validate="key", validatecommand=vcmd)
        self.label_entry.grid(row=3, column=1, sticky="ew", pady=(px(8), 0))
        self.label_hint = hint(sb, 4, col=1, span=1)

        # Expertenmodus: Partitionstabelle
        self.editor = PartitionEditor(right, on_change=lambda: self._update() if hasattr(self, "start_btn") else None)
        self.editor.grid(row=row, column=0, columnspan=2, sticky="ew")
        self.editor.grid_remove()
        row += 1

        sc = ttk.Frame(right)
        sc.grid(row=row, column=0, columnspan=2, sticky="ew")
        sc.columnconfigure(1, weight=1)
        sc.columnconfigure(0, minsize=label_w)
        row += 1
        field(sc, 0, _("Partitionsschema"))
        self.scheme_combo = ttk.Combobox(sc, textvariable=self.scheme_var, state="readonly",
                                         values=[_(t) for _k, t in SCHEMES])
        self.scheme_combo.grid(row=0, column=1, sticky="ew", pady=(px(8), 0))
        self.scheme_combo.bind("<<ComboboxSelected>>", lambda e: self._scheme_changed())
        self.scheme_hint = hint(sc, 1, col=1, span=1)

        opts = ttk.Frame(right)
        opts.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(px(12), 0))
        row += 1
        self.quick_chk = ttk.Checkbutton(opts, variable=self.quick_var, text=_("Schnellformatierung"),
                                         style="Switch.TCheckbutton", command=self._update)
        self.quick_chk.pack(anchor="w")
        self.verify_chk = ttk.Checkbutton(opts, variable=self.verify_var, style="Switch.TCheckbutton",
                                          text=_("Vorher Echtheit und Tempo prüfen (ca. 2–10 Minuten)"))
        self.verify_chk.pack(anchor="w")
        self.opt_hint = ttk.Label(opts, style="Hint.TLabel", wraplength=wrap, justify="left")

        right.rowconfigure(row, weight=1)
        row += 1
        self.start_btn = ttk.Button(right, text=_("Formatieren …"), style="Danger.TButton", command=self._start)
        self.start_btn.grid(row=row, column=0, columnspan=2, sticky="e", pady=(px(14), 0), ipadx=px(12))
        Tooltip(self.start_btn, lambda: self._why(self.start_btn))

        # ---- Fortschritt
        st = ui_theme.card(self, padding=(16, 10))
        st.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(gap, 0))
        st.columnconfigure(0, weight=1)
        self.pbar = ttk.Progressbar(st, maximum=100)
        self.pbar.grid(row=0, column=0, sticky="ew")
        self.cancel_box = ttk.Frame(st)
        self.cancel_box.grid(row=0, column=1, rowspan=2, sticky="e", padx=(px(12), 0))
        self.cancel_btn = ttk.Button(self.cancel_box, text=_("Abbrechen"), command=self._cancel)
        ttk.Label(st, textvariable=self.status_var, style="Muted.TLabel", wraplength=px(820), justify="left").grid(
            row=1, column=0, sticky="w", pady=(px(6), 0))

    def _selected(self):
        i = self.dev_combo.current()
        return self.devices[i] if 0 <= i < len(self.devices) else None

    def _fs(self):
        return FILESYSTEMS[self.fs_var.get()][0]

    def _label_max(self):
        return FILESYSTEMS.get(self.fs_var.get(), ("", 11))[1]

    def _cluster(self):
        i = self.cluster_combo.current()
        return self.cluster_values[i] if 0 <= i < len(self.cluster_values) else 0

    @staticmethod
    def _hint(lbl, text, warn=False):
        lbl.configure(text=text, style="Warn.TLabel" if warn else "Hint.TLabel")
        lbl.grid() if text else lbl.grid_remove()

    def _why(self, btn, need_dev=True):
        """Kurzinfo eines gesperrten Knopfs: warum - leer, wenn er gerade geht"""
        if not btn.instate(["disabled"]):
            return ""
        if self.busy:
            return _("Es läuft gerade ein Auftrag.")
        dev = self._selected()
        if need_dev and not dev:
            return _("Erst ein Laufwerk wählen.")
        if btn is self.rename_btn:
            return _("Umbenennen geht nur bei genau einem Laufwerk mit exFAT, FAT32, NTFS oder FAT.")
        if btn is self.start_btn:
            if self.expert_var.get():
                return self.editor.error or _("Die Aufteilung ist noch nicht vollständig.")
            if not self.cluster_values:
                return _("Dieses Dateisystem passt nicht zur Größe des Laufwerks.")
        return ""

    def _purpose_picked(self, key):
        """Kachel "Wofür?": Dateisystem setzen bzw. den Expertenmodus (eigene Aufteilung)"""
        if key == "expert":
            if not self.expert_var.get():
                self.expert_var.set(True)
                self._expert_changed()
            return
        if self.expert_var.get():
            self.expert_var.set(False)
            self._expert_changed()
        self.fs_var.set(key)
        self._refresh_options()

    def _fs_picked(self):
        self._refresh_options()

    def _sync_purpose(self, names):
        """Kacheln passend zu Auswahl und Laufwerk: gewählt, was gerade gilt; gesperrt, was nicht passt"""
        if self.expert_var.get():
            self.purpose.select("expert")
        else:
            self.purpose.select(self.fs_var.get() if self.fs_var.get() in dict(PURPOSES) else None)
        for key, _t in PURPOSES:
            self.purpose.enable(key, not self.busy and (key == "expert" or key in names))

    def _device_changed(self):
        self.cluster_pick = {}   # Größen hängen vom Laufwerk ab
        if not self.busy:        # Ergebnis des vorigen Auftrags gehört zum vorigen Laufwerk
            self.pbar["value"] = 0
            self.status_var.set("")
        self._refresh_options()

    def _expert_changed(self, initial=False):
        exp = bool(self.expert_var.get())
        if exp:
            self.simple_box.grid_remove()
            self.editor.grid()
        else:
            self.editor.grid_remove()
            self.simple_box.grid()
        if not initial:
            self.settings["expert"] = exp
            self.save()
            self._refresh_options()
            self.winfo_toplevel().event_generate("<<StdResized>>")

    def _cluster_changed(self):
        self.cluster_pick[self._fs()] = self._cluster()

    def _scheme_changed(self):
        self.scheme_key = SCHEMES[max(0, self.scheme_combo.current())][0]
        self._refresh_options()

    def _refresh_options(self):
        """Dateisysteme, Zuordnungseinheiten und Hinweise passend zu Laufwerk, Dateisystem und Partitionsschema"""
        dev = self._selected()
        names = available_filesystems(dev, self.scheme_key)
        self.fs_combo.configure(values=names)
        if self.fs_var.get() not in names:
            old = self.fs_var.get()
            self.fs_var.set("exFAT")
            if dev:
                self.status_var.set(_("{fs} passt nicht zu diesem Datenträger – exFAT gewählt.", fs=old))
        name = self.fs_var.get()
        fs, label_max, _text = FILESYSTEMS[name]
        self._hint(self.fs_hint, fsinfo.fs_hint(name))

        # Zuordnungseinheit
        if dev:
            part = partition_bytes(dev["size"], self.scheme_key, dev.get("ss", 512))
            vals, std = cluster_choices(fs, part, dev.get("ss", 512))
            self.cluster_values = vals
            self.cluster_combo.configure(values=[unit_text(v) + (" (" + _("Standard") + ")" if v == std else "")
                                                 for v in vals])
            want = self.cluster_pick.get(fs, std)
            if vals:
                self.cluster_combo.current(vals.index(want) if want in vals else vals.index(std))
            else:   # nichts passt (Datenträger zu klein): "Formatieren" bleibt gesperrt
                self.cluster_var.set("–")
        else:
            self.cluster_values = []
            self.cluster_combo.configure(values=[])
            self.cluster_var.set(_("Standard (nach Wahl des Laufwerks)"))

        # Partitionsschema (und die Aufteilung im Expertenmodus)
        self.scheme_combo.current([k for k, _t in SCHEMES].index(self.scheme_key))
        self.editor.set_context(dev, self.scheme_key)
        limit = mbr_limit(dev.get("ss", 512)) if dev else 0
        big = dev and dev["size"] - MB > limit
        if big and self.scheme_key == "mbr":
            self._hint(self.scheme_hint, _("MBR fasst hier höchstens {size} – der Rest des Datenträgers bliebe "
                                           "ungenutzt.", size=fmt_size(limit)), warn=True)
        elif big and self.scheme_key == "auto":
            self._hint(self.scheme_hint, _("Größer, als MBR fassen kann ({size}): Es wird GPT.", size=fmt_size(limit)))
        elif dev and self.scheme_key != "gpt" and limit > 0xFFFFFFFF * 512 and dev["size"] - MB > 0xFFFFFFFF * 512:
            self._hint(self.scheme_hint, _("Dieser Datenträger hat 4K-Sektoren: MBR fasst damit bis 16 TB – manche "
                                           "älteren Geräte kommen damit aber nicht zurecht."))
        else:
            self._hint(self.scheme_hint, "")

        # Bezeichnung
        label = self.label_var.get()
        text = _("Bis zu {n} Zeichen: A–Z, 0–9, Leerzeichen, - und _", n=label_max)
        if fs in ("fat32", "fat"):
            text += ", " + _("wird großgeschrieben")
        elif fs in ("ext2", "ext3", "ext4", "hfsplus"):
            if IS_WIN:
                text += " – " + _("Windows zeigt dafür keinen Laufwerksbuchstaben")
        if len(label) > label_max:
            self.label_var.set(label[:label_max])
            text += " – " + _("auf {n} Zeichen gekürzt", n=label_max)
        self._hint(self.label_hint, text)
        self._sync_purpose(names)
        self._update()

    def _update(self):
        idle = not self.busy
        dev = self._selected()

        def enable(w, on, *extra):
            w.state(list(extra) + (["!disabled"] if on else ["disabled"]))

        expert = bool(self.expert_var.get())
        for key, _t in PURPOSES:
            self.purpose.enable(key, idle and (key == "expert" or key in self.fs_combo.cget("values")))
        self.editor.set_enabled(idle)
        enable(self.dev_combo, idle and self.devices, "readonly")
        enable(self.reload_btn, idle)
        enable(self.fs_combo, idle, "readonly")
        enable(self.cluster_combo, idle and dev and self.cluster_values, "readonly")
        enable(self.scheme_combo, idle, "readonly")
        enable(self.label_entry, idle)
        enable(self.quick_chk, idle)
        enable(self.verify_chk, idle and self.quick_var.get())   # vollständig prüft ohnehin jede Stelle
        enable(self.start_btn, idle and dev and (self.editor.valid() if expert else bool(self.cluster_values)))
        enable(self.speed_btn, idle and dev)
        enable(self.backup_btn, idle and dev)
        enable(self.restore_btn, idle and dev)
        enable(self.verify_btn, idle)   # braucht keinen Datenträger
        # nur bei genau einem Laufwerk auf dem Datenträger: der Helfer nimmt nicht unbedingt das angezeigte
        vols = dev.get("volumes", []) if dev else []
        enable(self.rename_btn, idle and dev and len(vols) == 1 and vols[0]["fs"] in ("exFAT", "FAT32", "NTFS", "FAT"))
        self._opt_hint("" if self.quick_var.get() else
                   _("Vollständig: Jede Stelle wird beschrieben und geprüft – findet defekte Bereiche und gefälschte "
                     "Größen, dauert aber je nach Größe bis zu mehreren Stunden."))
        if self.job and self.job.cancellable:
            if not self.cancel_btn.winfo_ismapped():
                self.cancel_btn.state(["!disabled"])   # nach einem früheren Abbruch noch gesperrt
                self.cancel_btn.pack()
        elif self.cancel_btn.winfo_ismapped():
            self.cancel_btn.pack_forget()

    def _opt_hint(self, text):
        self.opt_hint.configure(text=text)
        self.opt_hint.pack(anchor="w", pady=(ui_theme.px(2), 0)) if text else self.opt_hint.pack_forget()

    def _set_busy(self, busy):
        self.busy = busy
        self._update()
        self.on_busy(busy)

    # ------------------------------------------------------------------ Laufwerke

    def activate(self):
        """Bereich wird angezeigt: Liste frisch holen"""
        if not self.busy:
            self.reload()

    def reload(self):
        if self.busy:
            return

        def work():
            self.q.put(("scan", 0, _("Suche USB-Datenträger am PC …")))
            try:
                self.q.put(("devices", pc_list(lambda pct, text: self.q.put(("scan", pct, text)))))
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as e:
                self.q.put(("error", _("USB-Datenträger konnten nicht gelesen werden: {e}", e=e)))

        self._run(work)

    def _fill_devices(self, devices, announce=True):
        prev = self._selected()
        items = [self._drive_item(d) for d in devices]
        self.devices = devices
        self.dev_combo.set_items(items)
        self.dev_combo.set_empty_text(_("Kein USB-Datenträger am PC gefunden. Stick einstecken und „Aktualisieren“ klicken."))
        # nie vorauswählen: am PC hängen oft Platten mit Daten (Sicherungen) - nur die eigene Auswahl bleibt
        idx = next((i for i, d in enumerate(devices) if prev and d["disk"] == prev.get("disk")
                    and d["size"] == prev.get("size") and d.get("model") == prev.get("model")
                    and d.get("serial", "") == prev.get("serial", "")), -1)
        if idx >= 0 and not prev.get("serial") and sum(
                1 for d in devices if d["size"] == prev.get("size") and d.get("model") == prev.get("model") and not d.get("serial")) > 1:
            idx = -1   # mehrere gleiche Sticks ohne Seriennummer: welcher es war, lässt sich nicht sagen - neu wählen lassen
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_combo.current(-1)
            self.cluster_pick = {}
        if announce:
            if not devices:
                self.status_var.set(_("Kein USB-Datenträger am PC gefunden. Stick einstecken und „Aktualisieren“ "
                                      "klicken."))
            else:
                self.status_var.set(_("{n} USB-Datenträger gefunden – bitte oben auswählen.", n=len(devices)))
        self._refresh_options()

    @staticmethod
    def _drive_item(d):
        """Karte eines Laufwerks: Modell, Größe, Nummer und Laufwerke, Belegung"""
        vols = d.get("volumes", [])
        sub = [_("Datenträger {n}", n=disk_name(d))]
        pills = []
        for v in vols[:3]:
            name = " ".join(x for x in (v.get("letters", ""), quote(v["label"]) if v.get("label") else "") if x)
            if name:
                sub.append(name)
            if v.get("fs"):
                pills.append((v["fs"], "Accent"))
        if not vols:
            pills.append((_("ohne Laufwerk"), ""))
        used = pc_used(d)
        last = last_job(history_load(), d.get("model"), d.get("size"), d.get("serial", ""))
        return {"text": pc_dev_text(d), "title": d.get("model") or _("USB-Datenträger"), "size": fmt_size(d["size"]),
                "sub": " · ".join(sub), "pills": pills, "used": min(1.0, used / d["size"]) if used and d["size"] else None,
                "last": last_job_text(last) if last else ""}

    # ------------------------------------------------------------------ Aufträge

    def _how(self):
        quick, verify = bool(self.quick_var.get()), bool(self.verify_var.get()) and bool(self.quick_var.get())
        how = _("Schnellformatierung") if quick else \
            _("vollständig – jede Stelle wird beschrieben und geprüft (dauert lange)")
        return quick, verify, how + (", " + _("vorher Echtheit und Tempo prüfen") if verify else "")

    def _scheme_text(self, dev):
        auto = "GPT" if uses_gpt(dev["size"], "auto", dev.get("ss", 512)) else "MBR"
        return {"auto": _("automatisch ({scheme})", scheme=auto), "mbr": "MBR", "gpt": "GPT"}[self.scheme_key]

    def _ask(self, dev, what, ok_text=None):
        """Rückfrage vor dem Löschen; what: Beschreibung des neuen Inhalts"""
        vols = pc_volumes_text(dev)
        used = pc_used(dev)
        has_data = used > 64 * MB
        data_warn = _("Auf dem Datenträger liegen {size} Daten – sie sind danach weg.", size=fmt_size(used)) + \
            "\n\n" if has_data else ""
        head = _("{model} (Datenträger {disk}, {size})", model=dev["model"] or _("USB-Datenträger"), disk=disk_name(dev),
                 size=fmt_size(dev["size"]))
        return confirm(
            self, _("Alle Daten werden gelöscht"),
            f"{head}\n{vols + chr(10) if vols else ''}\n{what}\n\n{data_warn}"
            + irreversible_admin_text(),
            ok_text or _("Formatieren"),
            _("Ich weiß, dass die {size} Daten unwiderruflich gelöscht werden", size=fmt_size(used)) if has_data
            else None)

    def _after(self, retry):
        """Ergebnis anzeigen; bei gefälschter Größe anbieten, nur den echten Bereich zu nutzen (retry(limit))"""
        def after(ok, msg, info):
            if ok:
                messagebox.showinfo(self.title_text, _("Datenträger erfolgreich formatiert.") + "\n\n" + msg,
                                    parent=self)
            elif info.get("fake") and info.get("real", 0) >= 64 * MB:
                real = info["real"] // MB * MB
                if messagebox.askyesno(
                        _("Gefälschte Größe"), msg + "\n\n" + _(
                            "Nur den echten Bereich ({size}) formatieren? Dann gehen keine Daten verloren, die über "
                            "diese Grenze hinaus geschrieben würden.", size=fmt_size(real)),
                        icon="warning", parent=self):
                    retry(real)
            else:
                messagebox.showerror(self.title_text, msg, parent=self)
        return after

    def _start(self):
        dev = self._selected()
        if self.busy or not dev:
            return
        if self.expert_var.get():
            self._start_expert(dev)
            return
        if not self.cluster_values:
            return
        quick, verify, how = self._how()
        opts = {"name": self.fs_var.get(), "fs": self._fs(), "cluster": self._cluster(), "scheme": self.scheme_key,
                "label": self.label_var.get(), "quick": quick, "verify": verify}
        if not LABEL_CHARS.match(opts["label"]) or len(opts["label"]) > self._label_max():
            messagebox.showerror(self.title_text, _("Die Bezeichnung enthält unerlaubte Zeichen."), parent=self)
            return
        lines = [_("wird formatiert:"),
                 "   " + _("Dateisystem: {fs}", fs=opts["name"]),
                 "   " + _("Zuordnungseinheit: {size}", size=unit_text(opts["cluster"])),
                 "   " + _("Partitionsschema: {scheme}", scheme=self._scheme_text(dev)),
                 "   " + _("Bezeichnung: {label}", label=quote(opts["label"]) if opts["label"] else _("keine")),
                 "   " + _("Art: {how}", how=how)]
        if not self._ask(dev, "\n".join(lines)):
            return
        self.settings.update(fs=opts["name"], scheme=opts["scheme"], label=opts["label"], quick=quick,
                             verify=bool(self.verify_var.get()))
        self.save()
        self._format(dev, opts)

    def _format(self, dev, opts, limit=0):
        if self.busy:
            return
        cluster = opts["cluster"]
        if limit:   # nur der echte Bereich: Größe muss auch dafür gültig sein, sonst die Standardgröße
            ss = dev.get("ss", 512)
            vals, std = cluster_choices(opts["fs"], partition_bytes(limit, opts["scheme"], ss), ss)
            if not vals:
                messagebox.showerror(self.title_text, _("{fs} passt nicht auf den echten Bereich ({size}).",
                                                        fs=opts["name"], size=fmt_size(limit)), parent=self)
                return
            cluster = cluster if cluster in vals else std
        args = ["format", str(dev["disk"]), str(dev["size"]), opts["fs"], opts["label"], *serial_args(dev),
                f"cluster={cluster}", f"scheme={opts['scheme']}", "plain"]
        if limit:
            args.append(f"limit={limit}")
        elif not opts["quick"]:
            args.append("full")
        elif opts["verify"]:
            args.append("verify")
        self._job(args, cancellable=not limit and (not opts["quick"] or opts["verify"]),
                  after=self._after(lambda real: self._format(dev, opts, limit=real)))

    def _start_expert(self, dev):
        ed = self.editor
        if not ed.valid():
            messagebox.showerror(self.title_text, ed.error or _("Die Aufteilung ist noch nicht vollständig."),
                                 parent=self)
            return
        quick, verify, how = self._how()
        what = _("wird neu aufgeteilt (Partitionsschema {scheme}):", scheme=self._scheme_text(dev)) + "\n" + \
            ed.describe() + "\n   " + _("Art: {how}", how=how)
        if not self._ask(dev, what):
            return
        self.settings.update(scheme=self.scheme_key, quick=quick, verify=bool(self.verify_var.get()))
        self.save()
        self._layout(dev, list(ed.args()), [dict(p) for p in ed.parts], quick, verify)

    def _layout(self, dev, parts_args, parts, quick, verify, limit=0):
        if self.busy:
            return
        if limit:   # nur der echte Bereich: passt die Aufteilung überhaupt hinein?
            sizes, free, err = fsinfo.plan(parts, limit, self.scheme_key, dev.get("ss", 512))
            # Einheiten neu für die kleineren Partitionen: die Standardgrößen galten für die gemeldete (falsche) Größe
            new_args = []
            for i, p in enumerate(parts):
                if err:
                    break
                cluster = 0
                if p["fs"] != fsinfo.NO_FS:
                    vals, std = fsinfo.cluster_choices(fsinfo.helper_fs(p["fs"]), sizes[i], dev.get("ss", 512))
                    cluster = p["cluster"] or std
                    if not vals:
                        err = _("Partition {n}: {fs} passt nicht zu dieser Größe – bitte Dateisystem oder Größe "
                                "ändern.", n=i + 1, fs=fsinfo.fs_display(p["fs"]))
                    elif cluster not in vals:
                        err = _("Partition {n}: Die Zuordnungseinheit {size} passt nicht zu dieser Größe – bitte "
                                "ändern.", n=i + 1, size=fsinfo.unit_text(cluster))
                new_args.append(fsinfo.part_arg(p, cluster))
            if err:
                messagebox.showerror(self.title_text, _("Die Aufteilung passt nicht auf den echten Bereich ({size}): "
                                                        "{error}", size=fmt_size(limit), error=err), parent=self)
                return
            parts_args = new_args
        args = ["layout", str(dev["disk"]), str(dev["size"]), *serial_args(dev), f"scheme={self.scheme_key}", "plain"]
        if limit:
            args.append(f"limit={limit}")
        elif not quick:
            args.append("full")
        elif verify:
            args.append("verify")
        self._job(args + parts_args, cancellable=not limit and (not quick or verify),
                  after=self._after(lambda real: self._layout(dev, parts_args, parts, quick, verify, real)))

    def _speed(self):
        dev = self._selected()
        if self.busy or not dev:
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Tempo messen"), msg, parent=self)

        self._job(["speed", str(dev["disk"]), str(dev["size"]), "plain", *serial_args(dev)], cancellable=True,
                  after=after)

    def _rename(self):
        dev = self._selected()
        if self.busy or not dev:
            return
        vols = dev["volumes"]
        vol = vols[0] if len(vols) == 1 and vols[0]["fs"] in ("exFAT", "FAT32", "NTFS", "FAT") else None
        if not vol:
            return
        most = 32 if vol["fs"] == "NTFS" else 11
        label = ask_string(self, _("Umbenennen"), _(
            "Neue Bezeichnung für {drive} ({fs}) – bis zu {n} Zeichen: A–Z, 0–9, Leerzeichen, - und _; leer = "
            "Bezeichnung entfernen:", drive=vol["letters"] or _("das Laufwerk"), fs=vol["fs"], n=most), vol["label"])
        if label is None or label == vol["label"]:
            return
        if not LABEL_CHARS.match(label) or len(label) > most:
            messagebox.showerror(_("Umbenennen"), _("Nur A–Z, 0–9, Leerzeichen, - und _ erlaubt (höchstens {n} "
                                                    "Zeichen).", n=most), parent=self)
            return
        # NTFS/FAT16 benennen unter Linux ntfslabel/fatlabel um - sie hielten "-..." für eine Option
        if not IS_WIN and vol["fs"] in ("NTFS", "FAT") and label.startswith("-"):
            messagebox.showerror(_("Umbenennen"), _("Unter Linux darf die Bezeichnung nicht mit „-“ beginnen."),
                                 parent=self)
            return
        target = quote(label) if label else _("keine Bezeichnung (wird entfernt)")
        if not confirm(self, _("Umbenennen"), pc_dev_text(dev) + "\n\n" + _("Neue Bezeichnung: {label}", label=target)
                       + "\n\n" + data_kept_admin_text(),
                       _("Umbenennen") if label else _("Entfernen")):
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Umbenennen"), msg, parent=self)

        self._job(["label", str(dev["disk"]), str(dev["size"]), label, *serial_args(dev)], after=after)

    # ------------------------------------------------------------------ Image sichern / wiederherstellen

    def _image_dir(self):
        d = self.settings.get("image_dir", "")
        return d if d and os.path.isdir(d) else os.path.expanduser("~")

    def _backup(self):
        dev = self._selected()
        if self.busy or not dev:
            return
        title = _("Image sichern")
        path = filedialog.asksaveasfilename(
            parent=self, title=title, initialdir=self._image_dir(), initialfile=image_file_name(dev),
            defaultextension=IMAGE_EXT, filetypes=[(_("Image-Sicherung"), "*" + IMAGE_EXT)])
        if not path:
            return
        if not path.lower().endswith(IMAGE_EXT):   # Linux-Dialoge hängen die Endung nicht immer an
            path += IMAGE_EXT
            if os.path.exists(path) and not messagebox.askyesno(   # nach diesem Namen hat der Dialog nicht gefragt
                    title, _("{name} gibt es schon. Ersetzen?", name=os.path.basename(path)), icon="warning", parent=self):
                return
        if on_device(path, dev):
            messagebox.showerror(title, _("Die Sicherung kann nicht auf dem Datenträger liegen, der gesichert wird – "
                                          "bitte ein anderes Laufwerk wählen."), parent=self)
            return
        self.settings["image_dir"] = os.path.dirname(path)
        self.save()
        used = pc_used(dev)
        text = pc_dev_text(dev) + "\n\n" + _("wird gesichert nach:") + "\n   " + path + "\n\n" + _(
            "Gesichert werden nur die belegten Bereiche (freier Platz von exFAT und FAT32 wird übersprungen), gepackt "
            "mit zstd; daneben entsteht eine .sha256-Datei. Während der Sicherung ist der Datenträger gesperrt.")
        if used:
            text += "\n" + _("Belegt laut System: {size}.", size=fmt_size(used))
        if not confirm(self, title, text + "\n\n" + data_kept_admin_text(), _("Sichern")):
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(title, msg, parent=self)

        self._job(["backup", str(dev["disk"]), str(dev["size"]), path, *serial_args(dev)], cancellable=True,
                  after=after)

    def _restore(self):
        dev = self._selected()
        if self.busy or not dev:
            return
        title = _("Image wiederherstellen")
        path = filedialog.askopenfilename(
            parent=self, title=title, initialdir=self._image_dir(),
            filetypes=[(_("Image-Sicherung"), "*" + IMAGE_EXT + " *.zst"), (_("Alle Dateien"), "*")])
        if not path:
            return
        try:
            info = image_info(path)
        except OSError as e:
            messagebox.showerror(title, _("Die Datei lässt sich nicht lesen: {e}", e=e), parent=self)
            return
        if info is None:
            messagebox.showerror(title, _("Das ist keine Image-Sicherung (.img.zst)."), parent=self)
            return
        if on_device(path, dev):
            messagebox.showerror(title, _("Die Image-Datei liegt auf dem Datenträger, der überschrieben werden soll – "
                                          "bitte erst auf ein anderes Laufwerk kopieren."), parent=self)
            return
        size = info.get("size", 0)
        if size > dev["size"]:
            messagebox.showerror(title, _("Das Image ({image}) ist größer als dieser Datenträger ({size}).",
                                          image=fmt_size(size), size=fmt_size(dev["size"])), parent=self)
            return
        self.settings["image_dir"] = os.path.dirname(path)
        self.save()
        lines = [_("wird mit dem Image überschrieben:"), "   " + _("Datei: {name}", name=os.path.basename(path))]
        if info.get("model") or info.get("label"):
            src = info.get("model", "")
            if info.get("label"):
                src += (" " if src else "") + quote(info["label"])
            lines.append("   " + _("Gesichert von: {src}", src=src))
        if info.get("created"):
            lines.append("   " + _("Stand: {date}", date=info["created"]))
        lines.append("   " + (_("Größe: {size}", size=fmt_size(size)) if size else _("Größe: unbekannt")))
        if size and size < dev["size"] - MB:
            lines.append("   " + _("Das Image ist kleiner – der Rest des Datenträgers bleibt ungenutzt."))
        if "created" not in info:
            lines.append("   " + _("Fremdes Image: Es wird vollständig geschrieben."))
        if not self._ask(dev, "\n".join(lines), _("Wiederherstellen")):
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(title, msg, parent=self)

        self._job(["restore", str(dev["disk"]), str(dev["size"]), path, *serial_args(dev)], cancellable=True,
                  after=after)

    def _verify(self):
        """Image-Datei nur prüfen: ganz entpacken, Prüfsumme im Image und die .sha256 daneben - ohne Datenträger und
        ohne Administratorrechte"""
        if self.busy:
            return
        title = _("Image prüfen")
        path = filedialog.askopenfilename(
            parent=self, title=title, initialdir=self._image_dir(),
            filetypes=[(_("Image-Sicherung"), "*" + IMAGE_EXT + " *.zst"), (_("Alle Dateien"), "*")])
        if not path:
            return
        try:
            info = image_info(path)
        except OSError as e:
            messagebox.showerror(title, _("Die Datei lässt sich nicht lesen: {e}", e=e), parent=self)
            return
        if info is None:
            messagebox.showerror(title, _("Das ist keine Image-Sicherung (.img.zst)."), parent=self)
            return
        self.settings["image_dir"] = os.path.dirname(path)
        self.save()

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(title, msg, parent=self)

        self._job(["verify", path], cancellable=True, after=after, plain=True)

    def _job(self, args, cancellable=False, after=None, plain=False):
        """Auftrag mit Administratorrechten ausführen (plain: ohne - kein Datenträger beteiligt, danach auch nicht neu
        einlesen); after(ok, meldung, info) läuft danach im UI-Thread"""
        if self.busy:
            return
        hwnd = hwnd_of(self)
        dev = next((d for d in self.devices if len(args) > 1 and str(d["disk"]) == str(args[1])), None)
        self.job_starting = True   # bis der Helfer läuft (UAC/pkexec-Abfrage): Schließen fragt nach

        def work():
            started = time.time()
            try:
                job = (PlainJob if plain else PcJob)(args, hwnd)
            except (OSError, RuntimeError) as e:
                self.job_starting = False
                self.q.put(("error", str(e)))
                return
            job.cancellable = cancellable
            self.job = job
            self.job_starting = False
            self.q.put(("started",))
            ok, msg, info = job.follow(lambda pct, step: self.q.put(("progress", pct, step)))
            msg = (msg or "") + record_job(args, ok, msg, None if plain else dev, started, getattr(job, "cancel_requested", False), info)
            self.q.put(("jobended",))   # der Helfer ist fertig - "Abbrechen" weg, auch während des Neueinlesens
            if plain:
                self.q.put(("done", ok, msg, info, None, after))
                return
            try:
                # danach neu einlesen - mit Fortschritt, schlafende Platten laufen dabei womöglich erst an
                devices = pc_list(lambda pct, text: self.q.put(("scan", pct, text)))
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError):
                devices = None
            self.q.put(("done", ok, msg, info, devices, after))

        self.pbar.stop()
        self.pbar.configure(mode="determinate")
        self.pbar["value"] = 0
        self.status_var.set(_("Startet …") if plain else admin_wait_text())
        self._run(work)

    def _cancel(self):
        if self.job:
            self.cancel_btn.state(["disabled"])
            self.status_var.set(_("Wird abgebrochen …"))
            self.job.cancel()

    def _run(self, work):
        self._set_busy(True)

        def guarded():
            try:
                work()
            except Exception as e:   # unerwartete Fehler im Thread sichtbar machen
                self.q.put(("error", _("Unerwarteter Fehler: {e}", e=e)))

        threading.Thread(target=guarded, daemon=True).start()

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
                except Exception as e:
                    self.status_var.set(_("Unerwarteter Fehler: {e}", e=e))
        finally:
            self._poll_id = self.after(50, self._poll)

    def _handle(self, msg):
        kind = msg[0]
        if kind == "status":
            self.status_var.set(msg[1])
        elif kind == "scan":
            show_scan(self.pbar, self.status_var, msg[1], msg[2])
            if not self.devices:   # während der Suche nicht schon "keiner gefunden"
                self.dev_combo.set_empty_text(_("Suche USB-Datenträger am PC …"))
        elif kind == "devices":
            self._set_busy(False)
            self._fill_devices(msg[1])
            self.pbar["value"] = 100   # kurz voll, dann leer: fertig eingelesen
            if self._bar_reset:
                self.after_cancel(self._bar_reset)
            self._bar_reset = self.after(800, self._clear_bar)
        elif kind == "jobended":
            self.job = None
            self._update()
        elif kind == "started":
            self._update()   # Abbrechen-Knopf zeigen
        elif kind == "progress":
            pct, step = msg[1], msg[2]
            # Schritte ohne Prozentangabe (Warten auf Windows): Lauf-Animation statt Stillstand
            moving = pct == 0 and step not in ("", "Fertig", _("Fertig"))
            if moving and str(self.pbar["mode"]) != "indeterminate":
                self.pbar.configure(mode="indeterminate")
                self.pbar.start(12)
            elif not moving:
                if str(self.pbar["mode"]) != "determinate":
                    self.pbar.stop()
                    self.pbar.configure(mode="determinate")
                self.pbar["value"] = pct
            self.status_var.set(f"{step} …" if moving else f"{step} … {pct} %")
        elif kind == "done":
            _k, ok, text, info, devices, after = msg
            self.job = None
            self.pbar.stop()
            self.pbar.configure(mode="determinate")
            self.pbar["value"] = 100 if ok else 0
            self._set_busy(False)
            if devices is not None:
                self._fill_devices(devices, announce=False)
            self.status_var.set(text)
            if after:
                after(ok, text, info)
        elif kind == "error":
            self.job = None
            self.pbar.stop()
            self.pbar.configure(mode="determinate")
            self._set_busy(False)
            self.status_var.set(msg[1])
            messagebox.showerror(self.title_text, msg[1], parent=self)

    # ------------------------------------------------------------------ Schließen

    def may_close(self):
        """Vor dem Schließen des Fensters: laufenden Auftrag ansprechen. True = schließen."""
        if (self.job or getattr(self, "job_starting", False)) and not messagebox.askyesno(
                self.title_text, _("Der Auftrag läuft am PC im Hintergrund weiter. Fenster trotzdem schließen?"),
                icon="warning", parent=self):
            return False
        if self.job and self.job.cancellable:
            self.job.cancel()   # Prüfung/Tempo nicht ohne Fenster weiterlaufen lassen
        self.after_cancel(self._poll_id)
        if self._bar_reset:   # Zeitgeber nicht nach dem Schließen feuern lassen
            self.after_cancel(self._bar_reset)
            self._bar_reset = None
        return True

    def _clear_bar(self):
        self._bar_reset = None
        if not self.busy:
            self.pbar.configure(value=0)
