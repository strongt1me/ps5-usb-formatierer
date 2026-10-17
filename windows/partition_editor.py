"""Expertenmodus: Partitionen selbst festlegen - Tabelle, Balken mit der Aufteilung und Bearbeiten-Dialog.
Jede Partition: Größe (oder der Rest), Dateisystem, Zuordnungseinheit, Bezeichnung, Partitionstyp, startfähig."""
import tkinter as tk
from tkinter import messagebox, ttk

import fsinfo
from fsinfo import FS_EXPERT, GB, MB, NO_FS, PTYPES, fs_display, size_text, unit_text
from i18n import _, number, quote, unit

COLORS = {"exFAT": "#2f6fb5", "FAT32": "#2a8c8c", "NTFS": "#5b5fc7", "FAT (FAT16)": "#6b7f99",
          "ext4 (Linux)": "#3c8d40", "ext3 (Linux)": "#5a9a3c", "ext2 (Linux)": "#7aa53a",
          "HFS+ (Mac OS Extended)": "#8e6bbf", NO_FS: "#8a8a8a"}
# Kurzname des Partitionstyps für die Tabelle
PTYPE_SHORT = {"auto": "automatisch", "windows": "Windows-Daten", "linux": "Linux", "swap": "Swap",
               "efi": "EFI-System", "apple": "Apple HFS+"}


def ptype_text(key, active=False):
    t = _(PTYPE_SHORT[key])
    return t + ", " + _("aktiv") if active else t


class PartitionDialog(tk.Toplevel):
    """Eine Partition bearbeiten. result: dict oder None (abgebrochen)"""

    def __init__(self, master, part, room, ss, is_last, scheme_mbr):
        super().__init__(master)
        self.withdraw()
        self.title(_("Partition"))
        self.transient(master.winfo_toplevel())
        self.resizable(False, False)
        self.result = None
        self.room, self.ss, self.is_last = room, ss, is_last
        frm = ttk.Frame(self, padding=(16, 12, 16, 12))
        frm.pack(fill="both", expand=True)
        frm.columnconfigure(1, weight=1)

        def label(text, r):
            ttk.Label(frm, text=text).grid(row=r, column=0, sticky="w", pady=4, padx=(0, 10))

        r = 0
        label(_("Größe:"), r)
        sz = ttk.Frame(frm)
        sz.grid(row=r, column=1, sticky="w")
        unit_gb = part["bytes"] >= 10 * GB
        self.size_var = tk.StringVar(value=self._num(part["bytes"] or room, unit_gb))
        # angezeigt wird gerundet (z. B. 14,7 GB) - bleiben Zahl und Einheit unverändert, gilt die genaue alte Größe
        self.orig = (part["bytes"], self.size_var.get(), 1 if unit_gb else 0)
        self.units = ["MB", "GB"]
        self.unit_combo = ttk.Combobox(sz, values=[unit(u) for u in self.units], state="readonly", width=5)
        self.unit_combo.current(1 if unit_gb else 0)
        self.size_entry = ttk.Spinbox(sz, textvariable=self.size_var, from_=1, to=10 ** 9, width=12, increment=1)
        self.size_entry.pack(side="left")
        self.unit_combo.pack(side="left", padx=(6, 0))
        self.unit_combo.bind("<<ComboboxSelected>>", lambda e: self._changed())
        self.rest_var = tk.BooleanVar(value=part["bytes"] == 0 and is_last)
        self.rest_chk = ttk.Checkbutton(sz, text=_("den ganzen Rest ({size})", size=size_text(room)),
                                        variable=self.rest_var, command=self._changed)
        self.rest_chk.pack(side="left", padx=(12, 0))
        if not is_last:
            self.rest_chk.state(["disabled"])
        r += 1

        label(_("Dateisystem:"), r)
        self.fs_combo = ttk.Combobox(frm, values=[fs_display(n) for n in FS_EXPERT], state="readonly", width=34)
        self.fs_combo.current(FS_EXPERT.index(part["fs"]) if part["fs"] in FS_EXPERT else 0)
        self.fs_combo.grid(row=r, column=1, sticky="ew")
        self.fs_combo.bind("<<ComboboxSelected>>", lambda e: self._changed())
        r += 1
        self.fs_hint = ttk.Label(frm, foreground="#666666", wraplength=420, justify="left")
        self.fs_hint.grid(row=r, column=1, sticky="w")
        r += 1

        label(_("Zuordnungseinheit:"), r)
        self.cluster_combo = ttk.Combobox(frm, state="readonly", width=34)
        self.cluster_combo.grid(row=r, column=1, sticky="ew")
        self.cluster_vals = []
        self.cluster_want = part.get("cluster", 0)
        # "Standard" bleibt 0 (folgt späteren Größenänderungen), nur eine selbst gewählte Einheit wird fest
        self.cluster_chosen = bool(self.cluster_want)
        self.cluster_combo.bind("<<ComboboxSelected>>", lambda e: setattr(self, "cluster_chosen", True))
        r += 1

        label(_("Bezeichnung:"), r)
        self.label_var = tk.StringVar(value=part.get("label", ""))
        vcmd = (self.register(lambda p: len(p) <= max(1, fsinfo.label_max(self._fs())) and
                              bool(fsinfo.LABEL_CHARS.match(p))), "%P")
        self.label_entry = ttk.Entry(frm, textvariable=self.label_var, validate="key", validatecommand=vcmd)
        self.label_entry.grid(row=r, column=1, sticky="ew")
        r += 1
        self.label_hint = ttk.Label(frm, foreground="#666666")
        self.label_hint.grid(row=r, column=1, sticky="w")
        r += 1

        label(_("Partitionstyp:"), r)
        self.ptype_combo = ttk.Combobox(frm, values=[_(t) for _k, t in PTYPES], state="readonly", width=34)
        self.ptype_combo.current([k for k, _t in PTYPES].index(part.get("ptype", "auto")))
        self.ptype_combo.grid(row=r, column=1, sticky="ew")
        r += 1
        self.active_var = tk.BooleanVar(value=part.get("active", False))
        self.active_chk = ttk.Checkbutton(frm, text=_("Startfähig (aktiv) – nur bei MBR"), variable=self.active_var)
        self.active_chk.grid(row=r, column=1, sticky="w", pady=(4, 0))
        if not scheme_mbr:
            self.active_var.set(False)
            self.active_chk.state(["disabled"])
        r += 1

        btns = ttk.Frame(frm)
        btns.grid(row=r, column=0, columnspan=2, sticky="e", pady=(14, 0))
        ttk.Button(btns, text=_("Übernehmen"), command=self._ok).pack(side="left")
        ttk.Button(btns, text=_("Abbrechen"), command=self.destroy).pack(side="left", padx=(6, 0))
        self.size_var.trace_add("write", lambda *a: self._changed())
        self.bind("<Escape>", lambda e: self.destroy())
        self.bind("<Return>", lambda e: self._ok())
        self._changed()

        self.update_idletasks()
        top = master.winfo_toplevel()
        x = top.winfo_rootx() + (top.winfo_width() - self.winfo_reqwidth()) // 2
        y = top.winfo_rooty() + (top.winfo_height() - self.winfo_reqheight()) // 3
        self.geometry(f"+{max(0, x)}+{max(0, y)}")
        self.deiconify()
        self.grab_set()
        self.size_entry.focus_set()

    @staticmethod
    def _num(b, gb):
        v = b / (GB if gb else MB)
        return number(f"{v:.1f}".rstrip("0").rstrip(".")) if gb else str(int(v))

    def _fs(self):
        i = self.fs_combo.current()
        return FS_EXPERT[i] if 0 <= i < len(FS_EXPERT) else FS_EXPERT[0]

    def _bytes(self):
        if self.rest_var.get():
            return 0
        if self.orig[0] and self.size_var.get() == self.orig[1] and self.unit_combo.current() == self.orig[2]:
            return self.orig[0]
        try:
            v = float(self.size_var.get().replace(",", ".").strip())
            b = int(v * (GB if self.unit_combo.current() == 1 else MB)) // MB * MB   # "inf"/"nan": Fehler
        except (ValueError, OverflowError):
            return None
        return b if b >= MB else None

    def _changed(self):
        self.size_entry.state(["disabled"] if self.rest_var.get() else ["!disabled"])
        name = self._fs()
        b = self._bytes()
        part = self.room if b in (0, None) else min(b, self.room)
        self.fs_hint.configure(text=fsinfo.fs_hint(name))
        vals, std = fsinfo.cluster_choices(fsinfo.helper_fs(name), part, self.ss)
        if name == NO_FS:
            self.cluster_vals = [0]
            self.cluster_combo.configure(values=["–"])
            self.cluster_combo.current(0)
            self.cluster_combo.state(["disabled"])
        elif vals:
            prev = self._cluster() or self.cluster_want
            self.cluster_vals = vals
            self.cluster_combo.configure(values=[unit_text(v) + (" (" + _("Standard") + ")" if v == std else "")
                                                 for v in vals])
            self.cluster_combo.current(vals.index(prev) if prev in vals else vals.index(std))
            self.cluster_combo.state(["!disabled", "readonly"])
        else:
            self.cluster_vals = []
            self.cluster_combo.configure(values=[_("{fs} passt nicht auf {size}", fs=name, size=size_text(part))])
            self.cluster_combo.current(0)
            self.cluster_combo.state(["disabled"])
        m = fsinfo.label_max(name)
        if len(self.label_var.get()) > m:
            self.label_var.set(self.label_var.get()[:m])
        self.label_entry.state(["!disabled"] if m else ["disabled"])
        self.label_hint.configure(text=_("Bis zu {n} Zeichen: A–Z, 0–9, Leerzeichen, - und _", n=m) if m else "")

    def _cluster(self):
        i = self.cluster_combo.current()
        return self.cluster_vals[i] if 0 <= i < len(self.cluster_vals) else 0

    def _ok(self):
        b = self._bytes()
        if b is None:
            messagebox.showerror(_("Partition"), _("Bitte eine Größe ab 1 MB angeben."), parent=self)
            return
        if b > self.room:
            messagebox.showerror(_("Partition"), _("Es sind nur noch {size} frei.", size=size_text(self.room)),
                                 parent=self)
            return
        if not self.cluster_vals:
            messagebox.showerror(_("Partition"), _("{fs} passt nicht auf diese Größe.", fs=fs_display(self._fs())),
                                 parent=self)
            return
        self.result = {"bytes": b, "fs": self._fs(), "cluster": self._cluster() if self.cluster_chosen else 0,
                       "label": self.label_var.get(), "ptype": PTYPES[max(0, self.ptype_combo.current())][0],
                       "active": bool(self.active_var.get())}
        self.destroy()


class PartitionEditor(ttk.Frame):
    """Tabelle der Partitionen mit Balken; on_change() nach jeder Änderung"""

    def __init__(self, master, on_change=None):
        super().__init__(master)
        self.on_change = on_change or (lambda: None)
        self.parts = [{"bytes": 0, "fs": "exFAT", "cluster": 0, "label": "", "ptype": "auto", "active": False}]
        self.dev, self.scheme = None, "auto"
        self.sizes, self.free, self.error = [], 0, None
        self.columnconfigure(0, weight=1)

        self.bar = tk.Canvas(self, height=38, highlightthickness=1, highlightbackground="#b9b9b9", bg="#f4f4f4")
        self.bar.grid(row=0, column=0, columnspan=2, sticky="ew")
        self.bar.bind("<Configure>", lambda e: self._draw())
        self.bar.bind("<Button-1>", self._bar_click)
        self.bar.bind("<Double-Button-1>", lambda e: self.edit())

        cols = ("nr", "size", "fs", "label", "cluster", "type")
        self.tree = ttk.Treeview(self, columns=cols, show="headings", height=5, selectmode="browse")
        for c, text, w, anchor in (("nr", _("Nr."), 36, "center"), ("size", _("Größe"), 120, "e"),
                                   ("fs", _("Dateisystem"), 160, "w"), ("label", _("Bezeichnung"), 110, "w"),
                                   ("cluster", _("Einheit"), 90, "e"), ("type", _("Typ"), 110, "w")):
            self.tree.heading(c, text=text, anchor=anchor)
            self.tree.column(c, width=w, minwidth=30, anchor=anchor, stretch=c == "fs")
        self.tree.grid(row=1, column=0, sticky="ew", pady=(6, 0))
        self.tree.bind("<Double-1>", lambda e: self.edit())
        self.tree.bind("<<TreeviewSelect>>", lambda e: (self._buttons(), self._draw()))
        side = ttk.Frame(self)
        side.grid(row=1, column=1, sticky="n", padx=(6, 0), pady=(6, 0))
        self.add_btn = ttk.Button(side, text=_("Hinzufügen …"), command=self.add)
        self.edit_btn = ttk.Button(side, text=_("Bearbeiten …"), command=self.edit)
        self.del_btn = ttk.Button(side, text=_("Entfernen"), command=self.remove)
        self.up_btn = ttk.Button(side, text="▲ " + _("nach vorn"), command=lambda: self.move(-1))
        self.down_btn = ttk.Button(side, text="▼ " + _("nach hinten"), command=lambda: self.move(1))
        for b in (self.add_btn, self.edit_btn, self.del_btn, self.up_btn, self.down_btn):
            b.pack(fill="x", pady=(0, 3))
        self.info = ttk.Label(self, foreground="#666666", wraplength=560, justify="left")
        self.info.grid(row=2, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self.enabled = True
        self.refresh()

    # ------------------------------------------------------------------ Zustand

    def set_context(self, dev, scheme):
        self.dev, self.scheme = dev, scheme
        if dev and fsinfo.uses_gpt(dev["size"], scheme, self._ss()):
            for p in self.parts:   # "startfähig" gibt es nur bei MBR - nicht weiter anzeigen und zusagen
                p["active"] = False
        self.refresh()

    def set_enabled(self, on):
        self.enabled = on
        self._buttons()

    def _ss(self):
        return self.dev.get("ss", 512) if self.dev else 512

    def _selected(self):
        sel = self.tree.selection()
        return int(sel[0]) if sel else -1

    def refresh(self):
        sel = self._selected()
        if self.dev:
            self.sizes, self.free, self.error = fsinfo.plan(self.parts, self.dev["size"], self.scheme, self._ss())
            # passt jedes Dateisystem (und eine selbst gewählte Einheit) noch zur Größe? Sonst teilte der Helfer den
            # Datenträger erst neu auf und scheiterte dann beim Formatieren
            for i, p in enumerate(self.parts):
                if self.error or i >= len(self.sizes) or p["fs"] == NO_FS:
                    continue
                vals, _std = fsinfo.cluster_choices(fsinfo.helper_fs(p["fs"]), self.sizes[i], self._ss())
                if not vals:
                    self.error = _("Partition {n}: {fs} passt nicht zu dieser Größe – bitte Dateisystem oder Größe "
                                   "ändern.", n=i + 1, fs=fs_display(p["fs"]))
                elif p["cluster"] and p["cluster"] not in vals:
                    self.error = _("Partition {n}: Die Zuordnungseinheit {size} passt nicht zu dieser Größe – bitte "
                                   "ändern.", n=i + 1, size=unit_text(p["cluster"]))
                elif p["fs"] in ("FAT32", "FAT (FAT16)"):   # Beginn und Größe stehen in 32-Bit-Feldern
                    start = fsinfo.plan_starts(self.sizes)[i]
                    if start // self._ss() > 0xFFFFFFFF or self.sizes[i] // self._ss() > 0xFFFFFFFF:
                        self.error = _("Partition {n}: {fs} geht nur in den ersten {size} des Datenträgers – bitte "
                                       "weiter nach vorn legen oder exFAT wählen.", n=i + 1, fs=fs_display(p["fs"]),
                                       size=size_text(0xFFFFFFFF * self._ss()))
        else:
            self.sizes, self.free, self.error = [], 0, None
        self.tree.delete(*self.tree.get_children())
        for i, p in enumerate(self.parts):
            size = size_text(self.sizes[i]) if i < len(self.sizes) else "?"
            if not p["bytes"]:
                size = _("Rest ({size})", size=size) if i < len(self.sizes) else _("Rest")
            cluster = unit_text(p["cluster"]) if p["cluster"] else "–" if p["fs"] == NO_FS else _("Standard")
            self.tree.insert("", "end", iid=str(i), values=(
                i + 1, size, fs_display(p["fs"]), p["label"] or "–", cluster, ptype_text(p["ptype"], p.get("active"))))
        if 0 <= sel < len(self.parts):
            self.tree.selection_set(str(sel))
        if not self.dev:
            text = _("Erst oben ein Laufwerk wählen.")
        elif self.error:
            text = "⚠ " + self.error
        elif self.free >= MB:
            text = _("{n} Partition(en), noch frei: {size}. Doppelklick auf eine Partition zum Bearbeiten.",
                     n=len(self.parts), size=size_text(self.free))
        else:
            text = _("{n} Partition(en), Datenträger voll aufgeteilt. Doppelklick auf eine Partition zum Bearbeiten.",
                     n=len(self.parts))
        self.info.configure(text=text, foreground="#b3261e" if self.error else "#666666")
        self._buttons()
        self._draw()
        self.on_change()

    def valid(self):
        return bool(self.dev) and not self.error and len(self.sizes) == len(self.parts)

    def _buttons(self):
        i = self._selected()
        on = self.enabled and bool(self.dev)
        mbr_full = self.dev and not fsinfo.uses_gpt(self.dev["size"], self.scheme, self._ss()) and len(self.parts) >= 4
        room = self._room(len(self.parts)) if self.dev else 0
        for b, cond in ((self.add_btn, on and len(self.parts) < fsinfo.MAX_PARTS and not mbr_full and room >= MB),
                        (self.edit_btn, on and i >= 0), (self.del_btn, on and i >= 0 and len(self.parts) > 1),
                        (self.up_btn, on and i > 0), (self.down_btn, on and 0 <= i < len(self.parts) - 1)):
            b.state(["!disabled"] if cond else ["disabled"])

    def _room(self, index):
        """Platz für Partition index, wenn alle anderen ihre feste Größe behalten (Rest-Partitionen zählen 0)"""
        others = [dict(p) for j, p in enumerate(self.parts) if j != index]
        for p in others:
            if not p["bytes"]:
                p["bytes"] = MB
        sizes, free, err = fsinfo.plan(others + [{"bytes": 0}], self.dev["size"], self.scheme, self._ss())
        return 0 if err or not sizes else sizes[-1]

    # ------------------------------------------------------------------ Aktionen

    def _dialog(self, part, index):
        mbr = not fsinfo.uses_gpt(self.dev["size"], self.scheme, self._ss())
        d = PartitionDialog(self, part, self._room(index), self._ss(), index == len(self.parts) - 1 or
                            index == len(self.parts), mbr)
        self.wait_window(d)
        return d.result

    def add(self):
        if not self.dev:
            return
        # bisher bekam die letzte den Rest: Sie behält vorerst die halbe Größe, die neue bekommt den Platz dahinter
        last = self.parts[-1] if self.parts else None
        old = last["bytes"] if last else None
        if last is not None and not last["bytes"]:
            room_last = self.sizes[-1] if len(self.sizes) == len(self.parts) else 0
            if room_last < 2 * MB:
                messagebox.showerror(_("Partition"), _("Kein Platz mehr frei."), parent=self)
                return
            last["bytes"] = room_last // 2 // MB * MB
        new = {"bytes": 0, "fs": "exFAT", "cluster": 0, "label": "", "ptype": "auto", "active": False}
        res = self._dialog(new, len(self.parts))
        if res:
            if res["active"]:   # MBR: nur eine startfähig (wie beim Bearbeiten)
                for p in self.parts:
                    p["active"] = False
            self.parts.append(res)
        elif last is not None:
            last["bytes"] = old   # abgebrochen: alles wie vorher
        self.refresh()
        if res:
            self.tree.selection_set(str(len(self.parts) - 1))

    def edit(self):
        i = self._selected()
        if i < 0 or not self.dev or not self.enabled:
            return
        res = self._dialog(self.parts[i], i)
        if res:
            if res["active"]:   # MBR: nur eine startfähig
                for p in self.parts:
                    p["active"] = False
            self.parts[i] = res
            self.refresh()

    def remove(self):
        i = self._selected()
        if i < 0 or len(self.parts) <= 1:
            return
        self.parts.pop(i)
        self.refresh()

    def move(self, d):
        i = self._selected()
        j = i + d
        if i < 0 or not 0 <= j < len(self.parts):
            return
        sizes = list(self.sizes) if len(self.sizes) == len(self.parts) else None
        self.parts[i], self.parts[j] = self.parts[j], self.parts[i]
        if sizes:
            sizes[i], sizes[j] = sizes[j], sizes[i]
        # "Rest" gibt es nur zuletzt: Wandert eine Rest-Partition nach vorn, behält sie ihre jetzige Größe
        for k, p in enumerate(self.parts[:-1]):
            if not p["bytes"]:
                p["bytes"] = max(MB, (sizes[k] if sizes else MB) // MB * MB)
        self.refresh()
        self.tree.selection_set(str(j))

    # ------------------------------------------------------------------ Balken

    def _draw(self):
        c = self.bar
        c.delete("all")
        w, h = c.winfo_width(), c.winfo_height()
        if w < 10 or not self.dev:
            c.create_text(w // 2, h // 2, text=_("Aufteilung erscheint nach Wahl des Laufwerks"), fill="#777777")
            return
        total = max(1, self.dev["size"])
        x, sel = 2.0, self._selected()
        x += w * MB / total
        for i, n in enumerate(self.sizes):
            pw = max(4.0, (w - 4) * n / total)
            p = self.parts[i]
            color = COLORS.get(p["fs"], "#777777")
            c.create_rectangle(x, 3, x + pw - 1, h - 3, fill=color, outline="#202020" if i == sel else color,
                               width=2 if i == sel else 1, tags=(f"p{i}",))
            text = f"{i + 1}: {fs_display(p['fs']).split(' ')[0]} {size_text(n)}"
            if pw > 60:
                c.create_text(x + pw / 2, h / 2, text=text if pw > len(text) * 6.5 else str(i + 1), fill="white",
                              tags=(f"p{i}",))
            x += pw
        if self.free >= MB:
            c.create_rectangle(x, 3, w - 2, h - 3, fill="#e3e3e3", outline="#c8c8c8", dash=(3, 2))
            if w - x > 70:
                c.create_text((x + w) / 2, h / 2, text=_("frei {size}", size=size_text(self.free)), fill="#555555")

    def _bar_click(self, event):
        hit = self.bar.find_overlapping(event.x, event.y, event.x, event.y)
        for item in hit:
            for tag in self.bar.gettags(item):
                if tag.startswith("p"):
                    self.tree.selection_set(tag[1:])
                    return

    # ------------------------------------------------------------------ für den Helfer

    def cluster_of(self, i):
        """gewählte Einheit oder die Standardgröße für diese Partition (der Helfer nähme sonst seine eigene)"""
        p = self.parts[i]
        if p["cluster"] or p["fs"] == NO_FS or i >= len(self.sizes):
            return p["cluster"]
        vals, std = fsinfo.cluster_choices(fsinfo.helper_fs(p["fs"]), self.sizes[i], self._ss())
        return std if vals else 0

    def args(self):
        return [fsinfo.part_arg(p, self.cluster_of(i)) for i, p in enumerate(self.parts)]

    def describe(self):
        lines = []
        for i, p in enumerate(self.parts):
            t = f"   {i + 1}) {fs_display(p['fs'])}, {size_text(self.sizes[i]) if i < len(self.sizes) else '?'}"
            c = self.cluster_of(i)
            if c:
                t += ", " + _("Einheit {size}", size=unit_text(c))
            if p["label"]:
                t += ", " + quote(p["label"])
            if p["ptype"] != "auto":
                t += ", " + _("Typ {name}", name=_(PTYPE_SHORT[p["ptype"]]))
            if p.get("active"):
                t += ", " + _("startfähig")
            lines.append(t)
        return "\n".join(lines)
