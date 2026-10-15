"""USB-Formatierer für Windows: USB-Sticks, Speicherkarten und externe USB-Laufwerke frei formatieren – exFAT,
FAT32, NTFS, FAT, ext2/3/4 (Linux) oder HFS+ (Mac), mit Wahl der Zuordnungseinheit und des Partitionsschemas;
im Expertenmodus mehrere Partitionen. Ohne PS5-Funktionen. Sprachen: Deutsch, English, Italiano, Español, Français.
Die eigentliche Arbeit erledigt der Windows-Helfer usbfmt_win.exe mit Administratorrechten (UAC-Abfrage je Auftrag);
die Systemplatte und interne Laufwerke bietet er nie an."""
import os
import tkinter as tk
from tkinter import messagebox, ttk

import i18n
from i18n import _
from pc_common import (enable_dpi_awareness, keep_on_screen, load_settings, open_help, resource_path, save_settings,
                       top_bar)
from standard_panel import StandardPanel

APP_NAME = "USBFormatierer"


class StandardApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.withdraw()
        self.resizable(False, False)
        self.report_callback_exception = self._on_exception
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.bind("<<StdClose>>", lambda e: self._on_close())
        self.bind("<<StdResized>>", lambda e: self.after_idle(lambda: keep_on_screen(self)))
        icon = resource_path("app.ico")
        if os.path.isfile(icon):
            self.iconbitmap(default=icon)   # auch für Dialoge und Meldungen
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")

        self.settings = load_settings(APP_NAME)
        i18n.set_lang(self.settings.get("lang") or i18n.system_lang())
        self.frm = None
        self._build()

        self.update_idletasks()
        w, h = self.winfo_width(), self.winfo_height()
        self.geometry(f"+{(self.winfo_screenwidth() - w) // 2}+{(self.winfo_screenheight() - h) // 3}")
        self.deiconify()
        self.after(100, self.panel.activate)

    def _build(self):
        self.title(_("USB-Datenträger formatieren"))
        self.frm = ttk.Frame(self, padding=(14, 8, 14, 12))
        self.frm.grid(sticky="nsew")
        self.frm.columnconfigure(0, weight=1)
        top, self.lang_box = top_bar(self.frm, i18n.lang(), self._set_lang, lambda: open_help(i18n.lang(), self))
        top.grid(row=0, column=0, sticky="e")
        self.panel = StandardPanel(self.frm, self.settings, lambda: save_settings(APP_NAME, self.settings))
        self.panel.grid(row=1, column=0, sticky="nsew")

    def _set_lang(self, code):
        """Sprache wechseln: Bereich in der neuen Sprache neu aufbauen (nicht während eines Auftrags)"""
        if code == i18n.lang():
            return
        if self.panel.busy:
            self.lang_box.set(dict(i18n.LANGS)[i18n.lang()])
            messagebox.showinfo(_("USB-Datenträger formatieren"), _("Die Sprache lässt sich nach dem laufenden "
                                                                     "Auftrag wechseln."), parent=self)
            return
        self.settings["lang"] = code
        save_settings(APP_NAME, self.settings)
        i18n.set_lang(code)
        self.panel.may_close()   # Abfrage-Schleife des alten Bereichs beenden
        self.frm.destroy()
        self._build()
        self.panel.activate()
        self.after_idle(lambda: keep_on_screen(self))

    def _on_close(self):
        if self.panel.may_close():
            self.destroy()

    def _on_exception(self, exc, val, tb):
        messagebox.showerror(_("USB-Datenträger formatieren"), _("Unerwarteter Fehler: {e}", e=val), parent=self)


if __name__ == "__main__":
    enable_dpi_awareness()
    StandardApp().mainloop()
