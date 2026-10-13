"""PS5-USB-Formatierer fuer Windows.

Findet die PS5 im Netz (ps5_discovery) und spricht mit dem usb_format-Payload (HTTP-API,
Port 8090). Laeuft der Payload noch nicht, wird er an den ELF-Loader der PS5 (Port 9021) geschickt.
"""
import json
import os
import queue
import re
import socket
import sys
import threading
import time
import tkinter as tk
import urllib.error
import urllib.request
from tkinter import messagebox, ttk

import ps5_discovery

APP_NAME = "PS5USBFormatierer"
API_PORT = 8090
LOADER_PORT = 9021
LABEL_RE = re.compile(r"^[A-Za-z0-9 _-]{0,11}$")
IP_RE = re.compile(r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$")


def resource_path(rel):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, rel)


def find_payload():
    for p in (resource_path("usb_format.elf"),
              os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "usb_format", "usb_format.elf")):
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


def settings_file():
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, APP_NAME, "settings.json")


def load_settings():
    try:
        with open(settings_file(), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_settings(data):
    path = settings_file()
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f)
    except OSError:
        pass


def fmt_size(b):
    units = ["Bytes", "KB", "MB", "GB", "TB"]
    v, i = float(b), 0
    while v >= 1024 and i < len(units) - 1:
        v /= 1024
        i += 1
    text = f"{v:.1f}".rstrip("0").rstrip(".") if i else str(int(v))
    return text.replace(".", ",") + " " + units[i]


def dev_title(d):
    return d.get("model") or "USB-Laufwerk"


def formattable(d):
    # Eingebundene Sticks werden nicht selbst ausgehängt (CE-108255-1): Der Benutzer zieht sie an der PS5 ab und
    # steckt sie wieder ein, der Formatierer übernimmt sie vor der PS5 ("replug")
    return not d["locked"]


def is_replug_step(step):
    return "abziehen" in step or "einstecken" in step or "neu angemeldet" in step


def fat32_cluster_kb(size):
    g = 1024 ** 3
    return 4 if size <= 8 * g else 8 if size <= 16 * g else 16 if size <= 32 * g else 32


class ApiError(Exception):
    pass


class Api:
    def __init__(self, host):
        self.base = f"http://{host}:{API_PORT}"

    def _call(self, path, body=None, timeout=4):
        data = None
        headers = {}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base + path, data=data, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read().decode("utf-8")).get("error")
            except ValueError:
                msg = None
            raise ApiError(msg or f"HTTP-Fehler {e.code}") from None
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise ConnectionError(str(e)) from None

    def devices(self, timeout=4):
        return self._call("/api/devices", timeout=timeout)

    def status(self):
        return self._call("/api/status")

    def format(self, device, fs, label, size, replug=False):
        return self._call("/api/format", {"device": device, "fs": fs, "label": label, "size": size,
                                          "replug": replug})


def send_payload(host, path):
    with open(path, "rb") as f:
        data = f.read()
    with socket.create_connection((host, LOADER_PORT), timeout=5) as s:
        s.sendall(data)


def enable_dpi_awareness():
    if sys.platform == "win32":
        import ctypes
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            try:
                ctypes.windll.user32.SetProcessDPIAware()
            except Exception:
                pass


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.withdraw()
        self.title("PS5-USB formatieren")
        self.resizable(False, False)
        self.report_callback_exception = self._on_exception
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        icon = resource_path("app.ico")
        if os.path.isfile(icon):
            self.iconbitmap(icon)

        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")

        self.settings = load_settings()
        self.q = queue.Queue()
        self.api = None
        self.devices = []
        self.busy = False          # Netzwerkaktion oder Formatierung laeuft
        self.formatting = False
        self.watching = False      # Abfrage nach fremden Aufträgen unterwegs
        self.consoles = {}         # Anzeigetext -> Fund aus ps5_discovery

        self.host_var = tk.StringVar(value=self.settings.get("host", ""))
        self.dev_var = tk.StringVar()
        self.fs_var = tk.StringVar(value="exFAT")
        self.cluster_var = tk.StringVar(value="64 Kilobytes")
        self.label_var = tk.StringVar(value=self.settings.get("label", ""))
        self.status_var = tk.StringVar(value="Nicht verbunden")

        self._build_ui()
        self._update()

        self.update_idletasks()
        w, h = self.winfo_width(), self.winfo_height()
        self.geometry(f"+{(self.winfo_screenwidth() - w) // 2}+{(self.winfo_screenheight() - h) // 3}")
        self.deiconify()
        self._poll_id = self.after(50, self._poll)
        self.after(200, self._search)
        self._watch_id = self.after(3000, self._watch)

    # ------------------------------------------------------------------ UI

    def _build_ui(self):
        frm = ttk.Frame(self, padding=(14, 10, 14, 12))
        frm.grid(sticky="nsew")
        frm.columnconfigure(0, weight=1)
        row = 0

        def caption(text):
            nonlocal row
            ttk.Label(frm, text=text).grid(row=row, column=0, columnspan=2, sticky="w", pady=(8, 2))
            row += 1

        caption("PS5:")
        self.host_entry = ttk.Combobox(frm, textvariable=self.host_var, width=34)
        self.host_entry.grid(row=row, column=0, sticky="ew")
        self.host_entry.bind("<Return>", lambda e: self._connect())
        self.host_entry.bind("<<ComboboxSelected>>", lambda e: self._connect())
        self.search_btn = ttk.Button(frm, text="Suchen", command=self._search)
        self.search_btn.grid(row=row, column=1, sticky="ew", padx=(6, 0))
        row += 1

        caption("Laufwerk:")
        self.dev_combo = ttk.Combobox(frm, textvariable=self.dev_var, state="readonly")
        self.dev_combo.grid(row=row, column=0, sticky="ew")
        self.dev_combo.bind("<<ComboboxSelected>>", lambda e: self._update())
        self.reload_btn = ttk.Button(frm, text="Aktualisieren", command=self._reload)
        self.reload_btn.grid(row=row, column=1, padx=(6, 0))
        row += 1

        caption("Dateisystem:")
        self.fs_combo = ttk.Combobox(frm, textvariable=self.fs_var, values=["exFAT", "FAT32"], state="readonly")
        self.fs_combo.grid(row=row, column=0, columnspan=2, sticky="ew")
        self.fs_combo.bind("<<ComboboxSelected>>", lambda e: self._update())
        row += 1

        caption("Größe der Zuordnungseinheiten:")
        self.cluster_combo = ttk.Combobox(frm, textvariable=self.cluster_var, state="disabled")
        self.cluster_combo.grid(row=row, column=0, columnspan=2, sticky="ew")
        row += 1

        caption("Volumebezeichnung:")
        vcmd = (self.register(lambda p: bool(LABEL_RE.match(p))), "%P")
        self.label_entry = ttk.Entry(frm, textvariable=self.label_var, validate="key", validatecommand=vcmd)
        self.label_entry.grid(row=row, column=0, columnspan=2, sticky="ew")
        row += 1
        self.label_hint = ttk.Label(frm, foreground="#666666")
        self.label_hint.grid(row=row, column=0, columnspan=2, sticky="w")
        row += 1

        self.pbar = ttk.Progressbar(frm, maximum=100)
        self.pbar.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(14, 4))
        row += 1
        ttk.Label(frm, textvariable=self.status_var, wraplength=330, justify="left").grid(
            row=row, column=0, columnspan=2, sticky="w")
        row += 1

        btns = ttk.Frame(frm)
        btns.grid(row=row, column=0, columnspan=2, sticky="e", pady=(12, 0))
        self.start_btn = ttk.Button(btns, text="Starten", command=self._start)
        self.start_btn.pack(side="left")
        ttk.Button(btns, text="Schließen", command=self._on_close).pack(side="left", padx=(6, 0))

    def _selected_device(self):
        i = self.dev_combo.current()
        return self.devices[i] if 0 <= i < len(self.devices) else None

    def _update(self):
        dev = self._selected_device()
        if self.fs_var.get() == "exFAT":
            self.cluster_var.set("64 Kilobytes")
            self.label_hint.configure(text="Bis zu 11 Zeichen: A–Z, 0–9, Leerzeichen, - und _")
        else:
            self.cluster_var.set(f"{fat32_cluster_kb(dev['size'])} Kilobytes (Standard)" if dev else "Standardgröße")
            self.label_hint.configure(text="Bis zu 11 Zeichen, wird bei FAT32 großgeschrieben")

        idle = not self.busy
        for w in (self.host_entry, self.label_entry):
            w.state(["!disabled"] if idle else ["disabled"])
        self.search_btn.state(["!disabled"] if idle else ["disabled"])
        self.reload_btn.state(["!disabled"] if idle and self.api else ["disabled"])
        self.dev_combo.state(["readonly", "!disabled"] if idle and self.devices else ["disabled"])
        self.fs_combo.state(["readonly", "!disabled"] if idle else ["disabled"])
        self.start_btn.state(["!disabled"] if idle and dev and formattable(dev) else ["disabled"])

    def _fill_devices(self, devices):
        prev = self._selected_device()
        self.devices = devices
        names = []
        for d in devices:
            if d["locked"]:
                where = "vom System verwendet (gesperrt)"
            elif d["mounts"]:
                where = "von der PS5 eingebunden – wird zum Formatieren kurz abgemeldet"
            else:
                where = "frei, bereit"
            fmt = d.get("format") or ""
            fmt = {"leer": "leer", "keine Partition": "ohne Partition", "unbekannt": "Format unbekannt"}.get(fmt, fmt)
            text = f"{dev_title(d)} ({d['name']}) – {fmt_size(d['size'])}{' – ' + fmt if fmt else ''} – {where}"
            names.append(text)
        self.dev_combo.configure(values=names)
        idx = -1
        if prev:
            idx = next((i for i, d in enumerate(devices) if d["name"] == prev["name"]), -1)
        if idx < 0:
            # vorausgewählt wird nur ein freier Stick, nie ein eingebundener (z. B. die Spiele-SSD)
            free = [i for i, d in enumerate(devices) if formattable(d) and not d["mounts"]]
            idx = free[0] if len(free) == 1 else -1
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_var.set("")
        if not devices:
            self.status_var.set("Kein USB-Stick an der PS5 gefunden. Stick einstecken und „Aktualisieren“ klicken.")

    # ------------------------------------------------------------------ Aktionen

    def _run(self, work):
        self.busy = True
        self._update()
        threading.Thread(target=self._guard(work), daemon=True).start()

    def _guard(self, work):
        def run():
            try:
                work()
            except Exception as e:  # unerwartete Fehler im Thread sichtbar machen
                self.q.put(("error", f"Unerwarteter Fehler: {e}"))
        return run

    @staticmethod
    def _console_text(c):
        extra = " (Formatierer läuft)" if c["formatter"] else " (Ruhemodus)" if c["standby"] else ""
        return f"{c['name']} – {c['ip']}{extra}"

    def _search(self):
        if self.busy:
            return

        def work():
            self.q.put(("status", "Suche PS5 im Netzwerk …"))
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
            self.status_var.set(f"{len(consoles)} PS5 gefunden – bitte oben auswählen.")
        elif saved:
            self.host_var.set(saved)
            self.status_var.set(f"Keine PS5 gefunden – versuche zuletzt verwendete Adresse {saved} …")
            self._connect()
        else:
            self.status_var.set("Keine PS5 gefunden. Ist sie eingeschaltet und im selben Netzwerk? "
                                "Die IP-Adresse kann auch oben eingegeben werden (Enter).")

    def _connect(self):
        if self.busy:
            return
        text = self.host_var.get().strip()
        console = self.consoles.get(text)
        host = console["ip"] if console else text
        m = IP_RE.match(host)
        if not m or any(int(x) > 255 for x in m.groups()):
            messagebox.showerror(self.title(), "Bitte eine gefundene PS5 auswählen oder ihre IP-Adresse eingeben, "
                                 "z. B. 192.168.178.40.\n\nSie steht auf der PS5 unter Einstellungen › Netzwerk › "
                                 "Verbindungsstatus.", parent=self)
            return
        if console and console["standby"] and not console["formatter"]:
            messagebox.showerror(self.title(), f"{console['name']} ist im Ruhemodus. Bitte die PS5 einschalten, "
                                 "den ELF-Loader starten und dann „Suchen“ klicken.", parent=self)
            return
        self.settings["host"] = host
        save_settings(self.settings)
        api = Api(host)

        def work():
            self.q.put(("status", f"Verbinde mit {host} …"))
            try:
                self.q.put(("connected", api, api.devices(timeout=2)))
                return
            except ApiError as e:
                self.q.put(("error", str(e)))
                return
            except ConnectionError:
                pass

            payload = find_payload()
            if not payload:
                self.q.put(("error", "usb_format.elf nicht gefunden. Sie muss neben dem Programm liegen."))
                return
            self.q.put(("status", "Formatierer läuft noch nicht – sende Payload an die PS5 …"))
            try:
                send_payload(host, payload)
            except OSError:
                self.q.put(("error", f"Keine Verbindung zu {host}. Läuft auf der PS5 ein ELF-Loader "
                                     f"(Port {LOADER_PORT}) und ist die IP-Adresse richtig?"))
                return
            for _ in range(20):
                time.sleep(0.5)
                try:
                    self.q.put(("connected", api, api.devices(timeout=2)))
                    return
                except (ConnectionError, ApiError):
                    pass
            self.q.put(("error", "Der Payload wurde gesendet, antwortet aber nicht. "
                                 "Bitte die PS5-Benachrichtigungen prüfen."))

        self._run(work)

    def _reload(self):
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                self.q.put(("devices", api.devices()))
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", f"Geräteliste konnte nicht geladen werden: {e}"))

        self._run(work)

    def _start(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return
        label = self.label_var.get()
        fs = "exfat" if self.fs_var.get() == "exFAT" else "fat32"
        fs_text = "exFAT mit 64 KB Zuordnungseinheiten" if fs == "exfat" else "FAT32"
        replug = bool(dev["mounts"])
        replug_text = ("Die PS5 nutzt diesen Datenträger gerade. Der Formatierer meldet ihn per Software kurz ab "
                       "und übernimmt ihn, bevor die PS5 ihn wieder einbindet. Klappt das nicht, bittet er, den "
                       "Stick an der PS5 abzuziehen und wieder einzustecken. Spiele und Apps, die davon laufen, "
                       "vorher beenden.\n\n") if replug else ""
        if not messagebox.askokcancel(
                "PS5-USB formatieren",
                f"WARNUNG: Durch das Formatieren werden ALLE Daten auf diesem Datenträger gelöscht.\n\n"
                f"{dev_title(dev)} ({dev['name']}, {fmt_size(dev['size'])}) wird als {fs_text} formatiert.\n\n"
                f"{replug_text}"
                f"Klicken Sie auf „OK“, um den Datenträger zu formatieren, oder auf „Abbrechen“, "
                f"um den Vorgang abzubrechen.",
                icon="warning", default="cancel", parent=self):
            return
        self.settings["label"] = label
        save_settings(self.settings)
        api = self.api

        def work():
            try:
                api.format(dev["name"], fs, label, dev["size"], replug)
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", f"Formatieren konnte nicht gestartet werden: {e}"))
                return
            self._follow(api, foreign=False)

        self.pbar["value"] = 0
        self._run(work)

    def _follow(self, api, foreign):
        """Auftrag auf der PS5 bis zum Ende verfolgen (im Thread). foreign: woanders gestartet (PS5-Browser,
        anderer PC) - dann nur anzeigen, ohne Rückfrage-Dialoge des Starters."""
        self.q.put(("formatting", foreign))
        failures = 0
        while True:
            time.sleep(0.5)
            try:
                st = api.status()
                failures = 0
            except (ConnectionError, ApiError):
                failures += 1
                if failures >= 20:
                    self.q.put(("finished", False, "Verbindung zur PS5 verloren. Bitte die "
                                                   "PS5-Benachrichtigungen prüfen.", foreign))
                    return
                continue
            self.q.put(("progress", st["percent"], st["step"]))
            if not st["busy"]:
                try:   # erst die Liste, dann das Ergebnis - sonst überschreibt der Listenhinweis die Meldung
                    self.q.put(("devices", api.devices()))
                except (ConnectionError, ApiError):
                    pass
                self.q.put(("finished", st["ok"], st["message"], foreign))
                return

    def _watch(self):
        """Alle 3 Sekunden nachsehen, ob auf der PS5 ein Auftrag läuft, den jemand anderes gestartet hat"""
        if self.api and not self.busy and not self.watching:
            api = self.api
            self.watching = True

            def check():
                try:
                    if api.status().get("busy"):
                        self.q.put(("foreign", api))
                except (ConnectionError, ApiError):
                    pass
                self.watching = False

            threading.Thread(target=self._guard(check), daemon=True).start()
        self._watch_id = self.after(3000, self._watch)

    # ------------------------------------------------------------------ Ereignisse aus Threads

    def _poll(self):
        try:
            while True:
                msg = self.q.get_nowait()
                kind = msg[0]
                if kind == "status":
                    self.status_var.set(msg[1])
                elif kind == "found":
                    self.busy = False
                    self._on_found(msg[1])
                elif kind == "connected":
                    self.api = msg[1]
                    self.busy = False
                    self.status_var.set(f"Verbunden mit {self.host_var.get().strip()}")
                    self._fill_devices(msg[2]["devices"])
                elif kind == "devices":
                    self.busy = self.formatting
                    self._fill_devices(msg[1]["devices"])
                elif kind == "formatting":
                    self.formatting = True
                elif kind == "foreign":
                    if not self.busy:   # woanders gestartet: hier mitverfolgen
                        self.status_var.set("Auf der PS5 läuft ein Auftrag …")
                        self.pbar["value"] = 0
                        api = msg[1]
                        self._run(lambda: self._follow(api, foreign=True))
                elif kind == "progress":
                    pct, step = msg[1], msg[2]
                    # Schritte ohne Prozentangabe (Warten auf das Abmelden, Werkzeuge): Lauf-Animation statt Stillstand
                    moving = is_replug_step(step) or (pct == 0 and step not in ("", "Fertig"))
                    if moving and str(self.pbar["mode"]) != "indeterminate":
                        self.pbar.configure(mode="indeterminate")
                        self.pbar.start(12)
                    elif not moving:
                        if str(self.pbar["mode"]) != "determinate":
                            self.pbar.stop()
                            self.pbar.configure(mode="determinate")
                        self.pbar["value"] = pct
                    self.status_var.set(f"{step} …" if moving else f"{step} … {pct} %")
                elif kind == "finished":
                    self.formatting = False
                    self.busy = False
                    self.pbar.stop()
                    self.pbar.configure(mode="determinate")
                    self.status_var.set(msg[2])
                    foreign = len(msg) > 3 and msg[3]
                    if msg[1]:
                        self.pbar["value"] = 100
                        if not foreign:
                            messagebox.showinfo("PS5-USB formatieren", "Datenträger erfolgreich formatiert.\n\n" + msg[2],
                                                parent=self)
                    else:
                        self.pbar["value"] = 0
                        if not foreign:
                            messagebox.showerror("PS5-USB formatieren", msg[2], parent=self)
                elif kind == "error":
                    self.busy = self.formatting = False
                    self.status_var.set(msg[1])
                    messagebox.showerror("PS5-USB formatieren", msg[1], parent=self)
                self._update()
        except queue.Empty:
            pass
        self._poll_id = self.after(50, self._poll)

    def _on_close(self):
        if self.formatting and not messagebox.askyesno(
                self.title(), "Die Formatierung läuft auf der PS5 weiter. Fenster trotzdem schließen?",
                icon="warning", parent=self):
            return
        self.after_cancel(self._poll_id)
        self.after_cancel(self._watch_id)
        self.destroy()

    def _on_exception(self, exc, val, tb):
        messagebox.showerror(self.title(), f"Unerwarteter Fehler: {val}", parent=self)


if __name__ == "__main__":
    enable_dpi_awareness()
    App().mainloop()
