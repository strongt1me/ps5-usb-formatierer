"""„Am PC Standard“: USB-Datenträger am PC frei formatieren – exFAT, FAT32, NTFS, FAT (FAT16), ext2/3/4 (Linux) oder
HFS+ (Mac), jede Zuordnungseinheit, MBR oder GPT, Schnell- oder vollständige Formatierung; im Expertenmodus mehrere
Partitionen mit Typ und Startkennung. Ohne PS5-Optionen.
Dritter Modus des PS5-USB-Formatierers und einziger Inhalt des eigenständigen USB-Formatierers."""
import queue
import subprocess
import threading
import tkinter as tk
import tkinter.font as tkfont
from tkinter import messagebox, ttk

import fsinfo
from fsinfo import (FILESYSTEMS, KB, LABEL_CHARS, MB, SCHEMES, available_filesystems, cluster_choices, mbr_limit,
                    partition_bytes, unit_text, uses_gpt)
from i18n import _, quote
from partition_editor import PartitionEditor
from pc_common import (PcJob, ask_string, confirm, fmt_size, hwnd_of, pc_dev_text, pc_list, pc_used,
                       pc_volumes_text)

__all__ = ["StandardPanel", "SCHEMES", "FILESYSTEMS", "KB", "MB"]


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
        self.job = None                 # laufender Auftrag des Windows-Helfers
        self.cluster_values = []        # Bytes zu den Einträgen der Auswahlliste

        fs = settings.get("fs") if settings.get("fs") in FILESYSTEMS else "exFAT"
        self.dev_var = tk.StringVar()
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
        self.columnconfigure(0, weight=1)
        row = 0

        def caption(text, top=8):
            nonlocal row
            lbl = ttk.Label(self, text=text)
            lbl.grid(row=row, column=0, columnspan=2, sticky="w", pady=(top, 2))
            row += 1
            return lbl

        def hint():   # leer: ausgeblendet (sonst bliebe eine Lücke)
            nonlocal row
            lbl = ttk.Label(self, foreground="#666666", wraplength=560, justify="left")
            lbl.grid(row=row, column=0, columnspan=2, sticky="w")
            lbl.grid_remove()
            row += 1
            return lbl

        caption(_("Laufwerk:"))
        self.dev_combo = ttk.Combobox(self, textvariable=self.dev_var, state="readonly", style="Std.TCombobox",
                                      width=62, postcommand=self._widen_dev_list)
        self.dev_combo.grid(row=row, column=0, sticky="ew")
        self.dev_combo.bind("<<ComboboxSelected>>", lambda e: self._device_changed())
        self.reload_btn = ttk.Button(self, text=_("Aktualisieren"), command=self.reload)
        self.reload_btn.grid(row=row, column=1, padx=(6, 0))
        row += 1

        self.expert_chk = ttk.Checkbutton(self, variable=self.expert_var, command=self._expert_changed,
                                          text=_("Expertenmodus – Partitionen selbst festlegen (Größen, Typen, "
                                                 "startfähig)"))
        self.expert_chk.grid(row=row, column=0, columnspan=2, sticky="w", pady=(8, 0))
        row += 1

        # einfacher Modus: eine Partition über den ganzen Datenträger
        sb = self.simple_box = ttk.Frame(self)
        sb.grid(row=row, column=0, columnspan=2, sticky="ew")
        sb.columnconfigure(0, weight=1)
        row += 1
        r = 0

        def cap(text):
            nonlocal r
            ttk.Label(sb, text=text).grid(row=r, column=0, sticky="w", pady=(8, 2))
            r += 1

        def sub_hint():
            nonlocal r
            lbl = ttk.Label(sb, foreground="#666666", wraplength=560, justify="left")
            lbl.grid(row=r, column=0, sticky="w")
            lbl.grid_remove()
            r += 1
            return lbl

        cap(_("Dateisystem:"))
        self.fs_combo = ttk.Combobox(sb, textvariable=self.fs_var, state="readonly")
        self.fs_combo.grid(row=r, column=0, sticky="ew")
        self.fs_combo.bind("<<ComboboxSelected>>", lambda e: self._refresh_options())
        r += 1
        self.fs_hint = sub_hint()

        cap(_("Größe der Zuordnungseinheiten:"))
        self.cluster_combo = ttk.Combobox(sb, textvariable=self.cluster_var, state="readonly")
        self.cluster_combo.grid(row=r, column=0, sticky="ew")
        self.cluster_combo.bind("<<ComboboxSelected>>", lambda e: self._cluster_changed())
        r += 1

        cap(_("Volumebezeichnung:"))
        vcmd = (self.register(lambda p: len(p) <= self._label_max() and bool(LABEL_CHARS.match(p))), "%P")
        self.label_entry = ttk.Entry(sb, textvariable=self.label_var, validate="key", validatecommand=vcmd)
        self.label_entry.grid(row=r, column=0, sticky="ew")
        r += 1
        self.label_hint = sub_hint()

        # Expertenmodus: Partitionstabelle
        self.editor = PartitionEditor(self, on_change=lambda: self._update() if hasattr(self, "start_btn") else None)
        self.editor.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        self.editor.grid_remove()
        row += 1

        caption(_("Partitionsschema:"))
        self.scheme_combo = ttk.Combobox(self, textvariable=self.scheme_var, state="readonly",
                                         values=[_(t) for _k, t in SCHEMES])
        self.scheme_combo.grid(row=row, column=0, columnspan=2, sticky="ew")
        self.scheme_combo.bind("<<ComboboxSelected>>", lambda e: self._scheme_changed())
        row += 1
        self.scheme_hint = hint()

        caption(_("Formatierungsoptionen:"), top=10)
        opts = ttk.Frame(self)
        opts.grid(row=row, column=0, columnspan=2, sticky="w")
        row += 1
        self.quick_chk = ttk.Checkbutton(opts, variable=self.quick_var, text=_("Schnellformatierung"),
                                         command=self._update)
        self.quick_chk.pack(anchor="w")
        self.verify_chk = ttk.Checkbutton(opts, variable=self.verify_var,
                                          text=_("Vorher Echtheit und Tempo prüfen (ca. 2–10 Minuten)"))
        self.verify_chk.pack(anchor="w", pady=(2, 0))
        self.opt_hint = hint()

        self.pbar = ttk.Progressbar(self, maximum=100)
        self.pbar.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(14, 4))
        row += 1
        ttk.Label(self, textvariable=self.status_var, wraplength=560, justify="left").grid(
            row=row, column=0, columnspan=2, sticky="w")
        row += 1

        btns = ttk.Frame(self)
        btns.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        self.speed_btn = ttk.Button(btns, text=_("Tempo messen"), command=self._speed)
        self.speed_btn.pack(side="left")
        self.rename_btn = ttk.Button(btns, text=_("Umbenennen"), command=self._rename)
        self.rename_btn.pack(side="left", padx=(6, 0))
        self.close_btn = ttk.Button(btns, text=_("Schließen"), command=lambda: self.winfo_toplevel().event_generate(
            "<<StdClose>>"))
        self.close_btn.pack(side="right", padx=(6, 0))
        self.start_btn = ttk.Button(btns, text=_("Formatieren …"), command=self._start)
        self.start_btn.pack(side="right")
        self.cancel_btn = ttk.Button(btns, text=_("Abbrechen"), command=self._cancel)

    def _widen_dev_list(self):
        """Aufgeklappte Liste so breit wie der längste Eintrag (ttk nimmt sonst die Breite des Feldes)"""
        font = tkfont.nametofont("TkTextFont")
        need = max((font.measure(v) for v in self.dev_combo.cget("values")), default=0) + 40
        extra = max(0, need - self.dev_combo.winfo_width())
        ttk.Style(self).configure("Std.TCombobox", postoffset=(0, 0, extra, 0))

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
        lbl.configure(text=text, foreground="#9a5b00" if warn else "#666666")
        lbl.grid() if text else lbl.grid_remove()

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
            self.cluster_combo.current(vals.index(want) if want in vals else vals.index(std))
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
            text += " – " + _("Windows zeigt dafür keinen Laufwerksbuchstaben")
        if len(label) > label_max:
            self.label_var.set(label[:label_max])
            text += " – " + _("auf {n} Zeichen gekürzt", n=label_max)
        self._hint(self.label_hint, text)
        self._update()

    def _update(self):
        idle = not self.busy
        dev = self._selected()

        def enable(w, on, *extra):
            w.state(list(extra) + (["!disabled"] if on else ["disabled"]))

        expert = bool(self.expert_var.get())
        enable(self.expert_chk, idle)
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
        enable(self.rename_btn, idle and dev and any(v["fs"] in ("exFAT", "FAT32", "NTFS", "FAT")
                                                     for v in dev.get("volumes", [])))
        self._hint(self.opt_hint, "" if self.quick_var.get() else
                   _("Vollständig: Jede Stelle wird beschrieben und geprüft – findet defekte Bereiche und gefälschte "
                     "Größen, dauert aber je nach Größe bis zu mehreren Stunden."))
        if self.job and self.job.cancellable:
            if not self.cancel_btn.winfo_ismapped():
                self.cancel_btn.pack(side="right", padx=(0, 6))   # links neben "Formatieren …"
        elif self.cancel_btn.winfo_ismapped():
            self.cancel_btn.pack_forget()

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
            self.q.put(("status", _("Suche USB-Datenträger am PC …")))
            try:
                self.q.put(("devices", pc_list()))
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as e:
                self.q.put(("error", _("USB-Datenträger konnten nicht gelesen werden: {e}", e=e)))

        self._run(work)

    def _fill_devices(self, devices, announce=True):
        prev = self._selected()
        self.devices = devices
        self.dev_combo.configure(values=[pc_dev_text(d) for d in devices])
        # nie vorauswählen: am PC hängen oft Platten mit Daten (Sicherungen) - nur die eigene Auswahl bleibt
        idx = next((i for i, d in enumerate(devices) if prev and d["disk"] == prev.get("disk")
                    and d["size"] == prev.get("size")), -1)
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_var.set("")
            self.cluster_pick = {}
        if announce:
            if not devices:
                self.status_var.set(_("Kein USB-Datenträger am PC gefunden. Stick einstecken und „Aktualisieren“ "
                                      "klicken."))
            else:
                self.status_var.set(_("{n} USB-Datenträger gefunden – bitte oben auswählen.", n=len(devices)))
        self._refresh_options()

    # ------------------------------------------------------------------ Aufträge

    def _how(self):
        quick, verify = bool(self.quick_var.get()), bool(self.verify_var.get()) and bool(self.quick_var.get())
        how = _("Schnellformatierung") if quick else \
            _("vollständig – jede Stelle wird beschrieben und geprüft (dauert lange)")
        return quick, verify, how + (", " + _("vorher Echtheit und Tempo prüfen") if verify else "")

    def _scheme_text(self, dev):
        auto = "GPT" if uses_gpt(dev["size"], "auto", dev.get("ss", 512)) else "MBR"
        return {"auto": _("automatisch ({scheme})", scheme=auto), "mbr": "MBR", "gpt": "GPT"}[self.scheme_key]

    def _ask(self, dev, what):
        """Rückfrage vor dem Löschen; what: Beschreibung des neuen Inhalts"""
        vols = pc_volumes_text(dev)
        used = pc_used(dev)
        has_data = used > 64 * MB
        data_warn = _("Auf dem Datenträger liegen {size} Daten – sie sind danach weg.", size=fmt_size(used)) + \
            "\n\n" if has_data else ""
        head = _("{model} (Datenträger {disk}, {size})", model=dev["model"] or _("USB-Datenträger"), disk=dev["disk"],
                 size=fmt_size(dev["size"]))
        return confirm(
            self, _("Alle Daten werden gelöscht"),
            f"{head}\n{vols + chr(10) if vols else ''}\n{what}\n\n{data_warn}"
            + _("Das lässt sich nicht rückgängig machen. Windows fragt anschließend nach Administratorrechten."),
            _("Formatieren"),
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
        args = ["format", str(dev["disk"]), str(dev["size"]), opts["fs"], opts["label"], f"cluster={cluster}",
                f"scheme={opts['scheme']}", "plain"]
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
            if err:
                messagebox.showerror(self.title_text, _("Die Aufteilung passt nicht auf den echten Bereich ({size}): "
                                                        "{error}", size=fmt_size(limit), error=err), parent=self)
                return
        args = ["layout", str(dev["disk"]), str(dev["size"]), f"scheme={self.scheme_key}", "plain"]
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

        self._job(["speed", str(dev["disk"]), str(dev["size"]), "plain"], cancellable=True, after=after)

    def _rename(self):
        dev = self._selected()
        if self.busy or not dev:
            return
        vol = next((v for v in dev["volumes"] if v["fs"] in ("exFAT", "FAT32", "NTFS", "FAT")), None)
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
        target = quote(label) if label else _("keine Bezeichnung (wird entfernt)")
        if not confirm(self, _("Umbenennen"), pc_dev_text(dev) + "\n\n" + _("Neue Bezeichnung: {label}", label=target)
                       + "\n\n" + _("Die Daten bleiben erhalten. Windows fragt anschließend nach Administratorrechten."),
                       _("Umbenennen") if label else _("Entfernen")):
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Umbenennen"), msg, parent=self)

        self._job(["label", str(dev["disk"]), str(dev["size"]), label], after=after)

    def _job(self, args, cancellable=False, after=None):
        """Auftrag mit Administratorrechten ausführen; after(ok, meldung, info) läuft danach im UI-Thread"""
        if self.busy:
            return
        hwnd = hwnd_of(self)

        def work():
            try:
                job = PcJob(args, hwnd)
            except (OSError, RuntimeError) as e:
                self.q.put(("error", str(e)))
                return
            job.cancellable = cancellable
            self.job = job
            self.q.put(("started",))
            ok, msg, info = job.follow(lambda pct, step: self.q.put(("progress", pct, step)))
            try:
                devices = pc_list()
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError):
                devices = None
            self.q.put(("done", ok, msg, info, devices, after))

        self.pbar.stop()
        self.pbar.configure(mode="determinate")
        self.pbar["value"] = 0
        self.status_var.set(_("Windows fragt nach Administratorrechten …"))
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
        elif kind == "devices":
            self._set_busy(False)
            self._fill_devices(msg[1])
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
        if self.job and not messagebox.askyesno(
                self.title_text, _("Der Auftrag läuft am PC im Hintergrund weiter. Fenster trotzdem schließen?"),
                icon="warning", parent=self):
            return False
        if self.job and self.job.cancellable:
            self.job.cancel()   # Prüfung/Tempo nicht ohne Fenster weiterlaufen lassen
        self.after_cancel(self._poll_id)
        return True
