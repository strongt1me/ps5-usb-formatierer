"""USB-Formatierer für Windows und Linux: USB-Sticks, Speicherkarten und externe USB-Laufwerke frei formatieren – exFAT,
FAT32, NTFS, FAT, ext2/3/4 (Linux) oder HFS+ (Mac), mit Wahl der Zuordnungseinheit und des Partitionsschemas;
im Expertenmodus mehrere Partitionen. Ohne PS5-Funktionen. Sprachen: Deutsch, English, Italiano, Español, Français.
Die eigentliche Arbeit erledigt der Helfer (usbfmt_win.exe bzw. usbfmt_linux) mit Administratorrechten (Abfrage je
Auftrag: UAC bzw. pkexec); die Systemplatte und interne Laufwerke bietet er nie an."""
import tkinter as tk
from tkinter import ttk

import i18n
import ui_msgbox as messagebox
import ui_theme
from i18n import _
from pc_common import (APP_VERSION, enable_dpi_awareness, enable_history, footer, history_clear, history_dialog,
                       history_load, keep_on_screen, lang_selector, load_settings, open_help, save_settings,
                       setup_window, show_centered, work_area)
from standard_panel import StandardPanel
from ui_widgets import THEME_NEXT, app_header

APP_NAME = "USBFormatierer"


class StandardApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.withdraw()
        self.report_callback_exception = self._on_exception
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.bind("<<StdClose>>", lambda e: self._on_close())
        self.bind("<<StdResized>>", lambda e: self.after_idle(lambda: keep_on_screen(self)))
        self.settings = load_settings(APP_NAME)
        setup_window(self, self.settings.get("theme", "auto"))   # Symbol (auch für Dialoge) und Theme
        i18n.set_lang(self.settings.get("lang") or i18n.system_lang())
        self.frm = None
        self._build()

        show_centered(self)   # mittig im nutzbaren Bildschirmbereich, ganz sichtbar
        self.after(100, self.panel.activate)
        self._sys_id = self.after(5000, self._follow_system)
        self.bind_all("<F1>", lambda e: open_help(i18n.lang(), self))
        self.bind_all("<F5>", lambda e: not (self.panel.busy or self.grab_current()) and self.panel.reload())
        for seq in ("<Control-h>", "<Control-H>"):   # nicht in Eingabefeldern: dort ist Strg+H der Rückschritt
            self.bind_all(seq, lambda e: not isinstance(e.widget, (tk.Entry, ttk.Entry)) and not self.grab_current()
                          and self._history())

    def _build(self):
        self.title(_("USB-Datenträger formatieren"))
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        self.frm = ttk.Frame(self, style="Page.TFrame", padding=ui_theme.pad((14, 14, 14, 10)))
        self.frm.grid(sticky="nsew")
        self.frm.columnconfigure(0, weight=1)
        self.frm.rowconfigure(1, weight=1)
        top, self.lang_box = app_header(self.frm, _("USB-Formatierer"), APP_VERSION, self.settings.get("theme", "auto"),
                                        self._toggle_theme,
                                        lambda p: lang_selector(p, i18n.lang(), self._set_lang),
                                        lambda: open_help(i18n.lang(), self), self._history)
        top.grid(row=0, column=0, sticky="ew", pady=(0, ui_theme.px(12)))
        self.panel = StandardPanel(self.frm, self.settings, lambda: save_settings(APP_NAME, self.settings))
        self.panel.grid(row=1, column=0, sticky="nsew")
        footer(self.frm, ps5=False).grid(row=2, column=0, sticky="ew")
        # höchstens so hoch, wie der Bildschirm Platz hat (Full HD mit 150 % und mehr) - center_window verkleinert sonst
        # vergeblich
        self.minsize(ui_theme.px(940), min(ui_theme.px(640), work_area(self)[3] - ui_theme.px(60)))

    def _history(self):
        """Fenster "Verlauf": die letzten Aufträge an diesem PC"""
        def clear():
            history_clear()
            return [(None, history_load())]
        history_dialog(self, [(None, history_load())], clear)

    def _toggle_theme(self):
        """Hell/Dunkel umschalten und merken; die Oberfläche wird im neuen Theme neu aufgebaut"""
        if self.panel.busy:
            messagebox.showinfo(_("USB-Datenträger formatieren"), _("Das Design lässt sich nach dem laufenden "
                                                                     "Auftrag wechseln."), parent=self)
            return
        self.settings["theme"] = THEME_NEXT.get(self.settings.get("theme", "auto"), "auto")
        save_settings(APP_NAME, self.settings)
        ui_theme.apply_theme(self, self.settings["theme"])
        self._rebuild()

    def _follow_system(self):
        """Design "Automatisch": dem System folgen, auch wenn es im laufenden Betrieb umschaltet"""
        self._sys_id = self.after(5000, self._follow_system)
        if self.settings.get("theme", "auto") != "auto" or self.panel.busy or self.grab_current() \
                or any(isinstance(w, tk.Toplevel) for w in self.winfo_children()):
            return   # offener Dialog: sein Auftrag hinge sonst an der abgebauten Oberfläche
        if ("dark" if ui_theme.system_dark() else "light") != ui_theme.mode():
            ui_theme.apply_theme(self, "auto")
            self._rebuild()

    def _rebuild(self):
        self.panel.may_close()   # Abfrage-Schleife des alten Bereichs beenden
        self.frm.destroy()
        self._build()
        self.panel.activate()
        self.after_idle(lambda: keep_on_screen(self))

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
        self._rebuild()

    def _on_close(self):
        if self.panel.may_close():
            self.after_cancel(self._sys_id)
            self.destroy()

    def _on_exception(self, exc, val, tb):
        messagebox.showerror(_("USB-Datenträger formatieren"), _("Unerwarteter Fehler: {e}", e=val), parent=self)


if __name__ == "__main__":
    enable_dpi_awareness()
    enable_history(APP_NAME)
    StandardApp().mainloop()
