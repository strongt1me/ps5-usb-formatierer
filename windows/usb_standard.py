"""USB-Formatierer für Windows: USB-Sticks, Speicherkarten und externe USB-Laufwerke frei formatieren – exFAT,
FAT32, NTFS, FAT, ext2/3/4 (Linux) oder HFS+ (Mac), mit Wahl der Zuordnungseinheit und des Partitionsschemas;
im Expertenmodus mehrere Partitionen. Ohne PS5-Funktionen.
Die eigentliche Arbeit erledigt der Windows-Helfer usbfmt_win.exe mit Administratorrechten (UAC-Abfrage je Auftrag);
die Systemplatte und interne Laufwerke bietet er nie an."""
import os
import tkinter as tk
from tkinter import messagebox, ttk

from pc_common import enable_dpi_awareness, keep_on_screen, load_settings, resource_path, save_settings
from standard_panel import StandardPanel

APP_NAME = "USBFormatierer"
TITLE = "USB-Datenträger formatieren"


class StandardApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.withdraw()
        self.title(TITLE)
        self.resizable(False, False)
        self.report_callback_exception = self._on_exception
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.bind("<<StdClose>>", lambda e: self._on_close())
        self.bind("<<StdResized>>", lambda e: self.after_idle(lambda: keep_on_screen(self)))
        icon = resource_path("app.ico")
        if os.path.isfile(icon):
            self.iconbitmap(icon)
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")

        self.settings = load_settings(APP_NAME)
        frm = ttk.Frame(self, padding=(14, 4, 14, 12))
        frm.grid(sticky="nsew")
        self.panel = StandardPanel(frm, self.settings, lambda: save_settings(APP_NAME, self.settings), title=TITLE)
        self.panel.grid(sticky="nsew")

        self.update_idletasks()
        w, h = self.winfo_width(), self.winfo_height()
        self.geometry(f"+{(self.winfo_screenwidth() - w) // 2}+{(self.winfo_screenheight() - h) // 3}")
        self.deiconify()
        self.after(100, self.panel.activate)

    def _on_close(self):
        if self.panel.may_close():
            self.destroy()

    def _on_exception(self, exc, val, tb):
        messagebox.showerror(TITLE, f"Unerwarteter Fehler: {val}", parent=self)


if __name__ == "__main__":
    enable_dpi_awareness()
    StandardApp().mainloop()
