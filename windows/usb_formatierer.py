"""PS5-USB-Formatierer fuer Windows.

Findet die PS5 im Netz (ps5_discovery) und spricht mit dem usb_format-Payload (HTTP-API,
Port 8090). Laeuft der Payload noch nicht, wird er an den ELF-Loader der PS5 (Port 9021) geschickt.
"""
import ctypes
import hashlib
import http.client
import json
import os
import queue
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
import urllib.error
import urllib.request
from tkinter import messagebox, simpledialog, ttk

import ps5_discovery

try:   # von build_helper.ps1 erzeugt: Prüfsumme des ausgelieferten Helfers
    from helper_hash import HELPER_SHA256
except ImportError:   # Entwicklung ohne gebauten Helfer: nur sperren, nicht vergleichen
    HELPER_SHA256 = ""

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
        except (urllib.error.URLError, OSError, ValueError, http.client.HTTPException) as e:
            raise ConnectionError(str(e) or type(e).__name__) from None

    def devices(self, timeout=4):
        return self._call("/api/devices", timeout=timeout)

    def status(self):
        return self._call("/api/status")

    def library(self):
        return self._call("/api/library", timeout=8)

    def delete_games(self, keys, uninstall):
        return self._call("/api/delete", {"keys": ",".join(keys), "uninstall": uninstall})

    def format(self, device, fs, label, size, replug=False, limit=0, folders=True, verify=False):
        body = {"device": device, "fs": fs, "label": label, "size": size, "replug": replug,
                "folders": folders, "verify": verify}
        if limit:
            body["limit"] = limit   # gefälschter Stick: nur den echten Bereich nutzen
        return self._call("/api/format", body)

    def cancel(self):
        return self._call("/api/cancel", {})


# ---------------------------------------------------------------------- PC-Modus: USB-Datenträger an diesem PC

def find_helper():
    for p in (resource_path("usbfmt_win.exe"), os.path.join(os.path.dirname(os.path.abspath(__file__)), "usbfmt_win.exe")):
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


def lock_helper(exe):
    """Helfer gegen Ändern/Löschen sperren und seine Prüfsumme prüfen, bevor er Administratorrechte bekommt.
    Er liegt (PyInstaller) im Temp-Ordner, in den auch Programme ohne Adminrechte schreiben dürfen.
    Liefert das Sperr-Handle; der Aufrufer schließt es, sobald der Prozess läuft."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = ctypes.c_void_p
    k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.c_ulong, ctypes.c_void_p]
    # GENERIC_READ, nur FILE_SHARE_READ (kein Schreiben/Löschen durch andere), OPEN_EXISTING
    h = k32.CreateFileW(exe, 0x80000000, 0x1, None, 3, 0x80, None)
    if not h or h == ctypes.c_void_p(-1).value:
        raise OSError("Der Helfer usbfmt_win.exe lässt sich nicht öffnen (wird er gerade verändert?).")
    try:
        with open(exe, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
    except OSError as e:
        k32.CloseHandle(ctypes.c_void_p(h))
        raise OSError(f"Der Helfer usbfmt_win.exe lässt sich nicht lesen: {e}") from None
    if HELPER_SHA256 and digest != HELPER_SHA256:
        k32.CloseHandle(ctypes.c_void_p(h))
        raise OSError("Der Helfer usbfmt_win.exe wurde verändert – aus Sicherheitsgründen nicht gestartet. "
                      "Bitte das Programm neu herunterladen.")
    return h


def pc_list():
    """USB-Datenträger am PC (ohne Administratorrechte). Die Systemplatte meldet der Helfer nie."""
    exe = find_helper()
    if not exe:
        raise RuntimeError("usbfmt_win.exe fehlt neben dem Programm.")
    r = subprocess.run([exe, "list"], capture_output=True, timeout=30,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if r.returncode != 0:
        raise RuntimeError(f"Helfer meldet Fehler {r.returncode}: "
                           f"{r.stderr.decode('utf-8', 'replace').strip() or 'keine Angabe'}")
    devices = json.loads(r.stdout.decode("utf-8") or "[]")
    for d in devices:   # fehlende Felder (andere Helfer-Version) nicht zum Absturz werden lassen
        d.setdefault("model", "")
        d.setdefault("volumes", [])
        for v in d["volumes"]:
            for k, default in (("letters", ""), ("label", ""), ("fs", ""), ("total", 0), ("free", 0)):
                v.setdefault(k, default)
    return devices


def pc_used(d):
    return sum(v["total"] - v["free"] for v in d["volumes"] if v["total"])


def pc_dev_text(d):
    parts = []
    for v in d["volumes"]:
        t = v["letters"] or "ohne Buchstaben"
        if v["label"]:
            t += f" „{v['label']}“"
        if v["fs"]:
            t += f" {v['fs']}"
        parts.append(t)
    vols = ", ".join(parts) if parts else "ohne Partition"
    # Datenträgernummer macht den Eintrag eindeutig (zwei gleiche leere Sticks)
    return f"{d['model'] or 'USB-Datenträger'} (Datenträger {d['disk']}) – {fmt_size(d['size'])} – {vols}"


class _ShellExecuteInfo(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_ulong), ("fMask", ctypes.c_ulong), ("hwnd", ctypes.c_void_p),
                ("lpVerb", ctypes.c_wchar_p), ("lpFile", ctypes.c_wchar_p), ("lpParameters", ctypes.c_wchar_p),
                ("lpDirectory", ctypes.c_wchar_p), ("nShow", ctypes.c_int), ("hInstApp", ctypes.c_void_p),
                ("lpIDList", ctypes.c_void_p), ("lpClass", ctypes.c_wchar_p), ("hkeyClass", ctypes.c_void_p),
                ("dwHotKey", ctypes.c_ulong), ("hIconOrMonitor", ctypes.c_void_p), ("hProcess", ctypes.c_void_p)]


class PcJob:
    """Startet den Helfer mit Administratorrechten (UAC-Abfrage von Windows) und liest seine Ausgabedatei mit"""
    STALL_S = 30 * 60   # so lange ohne neue Ausgabe: Helfer gilt als hängend

    def __init__(self, args, hwnd=None):
        exe = find_helper()
        if not exe:
            raise RuntimeError("usbfmt_win.exe fehlt neben dem Programm.")
        lock = lock_helper(exe)   # bis der Prozess läuft: niemand kann die Datei austauschen
        # eigener Ordner mit Zufallsnamen je Auftrag: ein fester Ordner ließe sich vorab umleiten (Junction).
        # Nicht tempfile.mkdtemp: Das gibt unter Windows nur "Eigentümer" Rechte - die Ausgabedatei legt aber der
        # Helfer als Administrator an (Eigentümer: Administratoren), das Programm ohne Adminrechte dürfte sie dann
        # nicht lesen. os.mkdir erbt die Rechte des Temp-Ordners (dort steht der Benutzer selbst).
        self.tmpdir = None
        for _ in range(20):
            d = os.path.join(tempfile.gettempdir(), f"{APP_NAME}-{secrets.token_hex(8)}")
            try:
                os.mkdir(d)
                self.tmpdir = d
                break
            except FileExistsError:
                continue
        if not self.tmpdir:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(lock))
            raise OSError("Arbeitsordner im Temp-Verzeichnis ließ sich nicht anlegen.")
        self.out = os.path.join(self.tmpdir, "auftrag.txt")
        self.cancel_file = os.path.join(self.tmpdir, "abbruch")
        sei = _ShellExecuteInfo()
        sei.cbSize = ctypes.sizeof(sei)
        sei.fMask = 0x40 | 0x400          # SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI
        sei.lpVerb = "runas"
        sei.lpFile = exe
        sei.lpParameters = subprocess.list2cmdline(["--out", self.out, "--cancel", self.cancel_file] + args)
        sei.nShow = 0                     # SW_HIDE
        sei.hwnd = hwnd                   # UAC-Abfrage vor dem Programmfenster statt blinkend in der Taskleiste
        shell32 = ctypes.WinDLL("shell32", use_last_error=True)
        try:
            if not shell32.ShellExecuteExW(ctypes.byref(sei)):
                code = ctypes.get_last_error()
                self._cleanup()
                if code == 1223:   # ERROR_CANCELLED: Benutzer hat die UAC-Abfrage abgelehnt
                    raise PermissionError("Ohne Administratorrechte lässt Windows keine Datenträger bearbeiten.")
                raise OSError(f"Helfer konnte nicht gestartet werden (Windows-Fehler {code}).")
        finally:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(lock))   # läuft jetzt: Windows sperrt die Datei selbst
        if not sei.hProcess:
            self._cleanup()
            raise OSError("Helfer wurde gestartet, aber Windows liefert keinen Prozess dazu.")
        self.proc = sei.hProcess

    def _cleanup(self):
        for p in (self.out, self.cancel_file):
            try:
                os.remove(p)
            except OSError:
                pass
        try:
            os.rmdir(self.tmpdir)
        except OSError:
            pass

    def cancel(self):
        try:
            open(self.cancel_file, "w").close()
        except OSError:
            pass

    def running(self):
        # 0x102 = WAIT_TIMEOUT: läuft noch; 0 = beendet; WAIT_FAILED (ungültiges Handle) = gilt als beendet
        return ctypes.windll.kernel32.WaitForSingleObject(ctypes.c_void_p(self.proc), 0) == 0x102

    def follow(self, on_progress):
        """Bis zum Ende mitlesen; liefert (ok, meldung, json)"""
        pos, result, last_change = 0, None, time.time()
        try:
            while result is None:
                alive = self.running()
                try:
                    with open(self.out, "rb") as f:
                        f.seek(pos)
                        data = f.read()
                except OSError:
                    data = b""
                end = data.rfind(b"\n") + 1
                pos += end
                if end:
                    last_change = time.time()
                for line in data[:end].decode("utf-8", "replace").splitlines():
                    parts = line.split("\t")
                    if parts[0] == "P" and len(parts) >= 3:
                        try:
                            on_progress(int(parts[1]), parts[2])
                        except ValueError:
                            pass
                    elif parts[0] == "R" and len(parts) >= 4:
                        try:
                            info = json.loads(parts[3])
                        except ValueError:
                            info = {}
                        result = (parts[1] == "1", parts[2], info if isinstance(info, dict) else {})
                if result is None and not alive:
                    code = ctypes.c_ulong(0)
                    ctypes.windll.kernel32.GetExitCodeProcess(ctypes.c_void_p(self.proc), ctypes.byref(code))
                    result = (False, f"Der Helfer wurde unerwartet beendet (Code {code.value}).", {})
                elif result is None and time.time() - last_change > self.STALL_S:
                    self.cancel()
                    # wirklich beenden, sonst schreibt er womöglich weiter, während schon ein neuer Auftrag läuft.
                    # Mitten im Formatieren abgebrochen bleibt ein leerer Stick (Tabelle wird zuletzt geschrieben).
                    ctypes.windll.kernel32.TerminateProcess(ctypes.c_void_p(self.proc), 1)
                    result = (False, "Der Helfer meldet seit 30 Minuten nichts mehr – abgebrochen.", {})
                elif result is None:
                    time.sleep(0.3)
        finally:
            ctypes.windll.kernel32.WaitForSingleObject(ctypes.c_void_p(self.proc), 5000)   # Datei freigeben lassen
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.proc))
            self._cleanup()
        return result


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
        self.ps5_devices = []      # zuletzt gelesene Liste der PS5 (bleibt beim Umschalten erhalten)
        self.pc_job = None         # laufender Auftrag des Windows-Helfers
        self.games = {}            # PS5-Gerätename (da1 ...) -> Spiele darauf (aus /api/library)
        self.lib_loading = False
        self.lib_known = False     # Spieleliste mindestens einmal geladen (sonst Inhalt unbekannt)
        self.ps5_step = ""         # aktueller Schritt eines PS5-Auftrags (für "Abbrechen")

        self.mode_var = tk.StringVar(value="pc" if self.settings.get("mode") == "pc" else "ps5")
        self.verify_var = tk.BooleanVar(value=self.settings.get("verify", False))
        self.folders_var = tk.BooleanVar(value=self.settings.get("folders", True))
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
        self._mode_changed(initial=True)
        self._watch_id = self.after(3000, self._watch)

    # ------------------------------------------------------------------ UI

    def _build_ui(self):
        frm = ttk.Frame(self, padding=(14, 10, 14, 12))
        frm.grid(sticky="nsew")
        frm.columnconfigure(0, weight=1)
        row = 0

        def caption(text):
            nonlocal row
            lbl = ttk.Label(frm, text=text)
            lbl.grid(row=row, column=0, columnspan=2, sticky="w", pady=(8, 2))
            row += 1
            return lbl

        # Schalter: Stick an der PS5 (über das Netzwerk) oder direkt an diesem PC
        modes = ttk.Frame(frm)
        modes.grid(row=row, column=0, columnspan=2, sticky="ew")
        modes.columnconfigure((0, 1), weight=1, uniform="mode")
        self.mode_btns = []
        for col, (val, text) in enumerate((("ps5", "USB an der PS5"), ("pc", "USB an diesem PC"))):
            b = ttk.Radiobutton(modes, text=text, value=val, variable=self.mode_var, style="Toolbutton",
                                command=self._mode_changed)
            b.grid(row=0, column=col, sticky="ew", padx=(0, 3) if col == 0 else (3, 0), ipady=3)
            self.mode_btns.append(b)
        row += 1

        self.ps5_caption = caption("PS5:")
        self.host_entry = ttk.Combobox(frm, textvariable=self.host_var, width=62)
        self.host_entry.grid(row=row, column=0, sticky="ew")
        self.host_entry.bind("<Return>", lambda e: self._connect())
        self.host_entry.bind("<<ComboboxSelected>>", lambda e: self._connect())
        self.search_btn = ttk.Button(frm, text="Suchen", command=self._search)
        self.search_btn.grid(row=row, column=1, sticky="ew", padx=(6, 0))
        row += 1
        # wie in der Weboberfläche: das passiert wirklich, wenn man an der laufenden PS5 umsteckt
        self.ps5_warn = ttk.Label(
            frm, foreground="#9a5b00", wraplength=600, justify="left",
            text="⚠ Während der Nutzung keine USB-Datenträger an der PS5 abziehen oder neu anschließen: Die PS5 bindet "
                 "sie in laufende Spiele und Apps ein – danach starten diese oft erst nach einem Neustart der Konsole "
                 "wieder. Abziehen nur, wenn der Formatierer ausdrücklich dazu auffordert.")
        self.ps5_warn.grid(row=row, column=0, columnspan=2, sticky="w", pady=(6, 0))
        row += 1

        caption("Laufwerk:")
        self.dev_combo = ttk.Combobox(frm, textvariable=self.dev_var, state="readonly", style="Dev.TCombobox",
                                      postcommand=self._widen_dev_list)
        self.dev_combo.grid(row=row, column=0, sticky="ew")
        self.dev_combo.bind("<<ComboboxSelected>>", lambda e: (self._update(), self._show_games()))
        self.reload_btn = ttk.Button(frm, text="Aktualisieren", command=self._reload)
        self.reload_btn.grid(row=row, column=1, padx=(6, 0))
        row += 1

        # Spiele auf dem gewählten Datenträger (nur PS5-Modus, wie "Inhalt" in der Weboberfläche)
        self.games_box = ttk.Frame(frm)
        self.games_box.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        self.games_box.columnconfigure(0, weight=1)
        self.games_caption = ttk.Label(self.games_box, text="Spiele auf diesem Datenträger:")
        self.games_caption.grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, 2))
        cols = ("name", "id", "kind", "size")
        self.games_tree = ttk.Treeview(self.games_box, columns=cols, show="headings", height=5, selectmode="extended")
        self.games_tree.bind("<<TreeviewSelect>>", lambda e: self._update())
        for c, text, w, anchor in (("name", "Titel", 250, "w"), ("id", "Titel-ID", 90, "w"),
                                   ("kind", "Format", 70, "w"), ("size", "Größe", 80, "e")):
            self.games_tree.heading(c, text=text, anchor=anchor)
            self.games_tree.column(c, width=w, minwidth=40, anchor=anchor, stretch=(c == "name"))
        self.games_tree.grid(row=1, column=0, sticky="ew")
        sb = ttk.Scrollbar(self.games_box, orient="vertical", command=self.games_tree.yview)
        sb.grid(row=1, column=1, sticky="ns")
        self.games_tree.configure(yscrollcommand=sb.set)
        # Löschen: Spiele oben markieren (Strg/Umschalt für mehrere)
        gbar = ttk.Frame(self.games_box)
        gbar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        self.del_btn = ttk.Button(gbar, text="Markierte Spiele löschen …", command=self._delete_games)
        self.del_btn.pack(side="right")
        self.uninstall_var = tk.BooleanVar(value=True)
        self.uninstall_chk = ttk.Checkbutton(gbar, variable=self.uninstall_var,
                                             text="Kachel auch von der Startseite entfernen")
        self.uninstall_chk.pack(side="left")
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

        # dieselben Zusatzoptionen wie in der Weboberfläche (an der PS5 und am PC)
        self.pc_opts = ttk.Frame(frm)
        self.pc_opts.grid(row=row, column=0, columnspan=2, sticky="w", pady=(8, 0))
        self.verify_chk = ttk.Checkbutton(self.pc_opts, variable=self.verify_var,
                                          text="Vorher Echtheit und Tempo prüfen (ca. 2–10 Minuten)")
        self.verify_chk.pack(anchor="w")
        self.folders_chk = ttk.Checkbutton(self.pc_opts, variable=self.folders_var,
                                           text="Ordner homebrew, etaHEN/games und pkg anlegen")
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
        self.speed_btn = ttk.Button(self.tools, text="Tempo messen", command=self._pc_speed)
        self.speed_btn.pack(side="left")
        self.rename_btn = ttk.Button(self.tools, text="Umbenennen", command=self._pc_rename)
        self.rename_btn.pack(side="left", padx=(6, 0))
        ttk.Button(btns, text="Schließen", command=self._on_close).pack(side="right", padx=(6, 0))
        self.start_btn = ttk.Button(btns, text="Formatieren …", command=self._start)
        self.start_btn.pack(side="right")
        self.cancel_btn = ttk.Button(btns, text="Abbrechen", command=self._cancel)

    def _confirm(self, title, text, ok_text, ack=None):
        """Rückfrage mit eigenen Knopftexten; mit ack muss erst ein Häkchen gesetzt werden (wie in der Web-UI).
        Abbrechen ist vorausgewählt, Esc bricht ab. Liefert True bei Bestätigung."""
        win = tk.Toplevel(self)
        win.withdraw()
        win.title(title)
        win.transient(self)
        win.resizable(False, False)
        frm = ttk.Frame(win, padding=(18, 14, 18, 14))
        frm.pack(fill="both", expand=True)
        ttk.Label(frm, text=text, wraplength=520, justify="left").pack(anchor="w")
        result = {"ok": False}

        def done(ok):
            result["ok"] = ok
            win.destroy()

        btns = ttk.Frame(frm)
        ok_btn = ttk.Button(btns, text=ok_text, command=lambda: done(True))
        cancel_btn = ttk.Button(btns, text="Abbrechen", command=lambda: done(False))
        if ack:
            var = tk.BooleanVar(value=False)
            ttk.Checkbutton(frm, text=ack, variable=var,
                            command=lambda: ok_btn.state(["!disabled"] if var.get() else ["disabled"])).pack(
                anchor="w", pady=(12, 0))
            ok_btn.state(["disabled"])
        btns.pack(anchor="e", pady=(16, 0))
        cancel_btn.pack(side="right")
        ok_btn.pack(side="right", padx=(0, 6))
        win.bind("<Escape>", lambda e: done(False))
        win.protocol("WM_DELETE_WINDOW", lambda: done(False))
        win.update_idletasks()
        x = self.winfo_rootx() + (self.winfo_width() - win.winfo_reqwidth()) // 2
        y = self.winfo_rooty() + (self.winfo_height() - win.winfo_reqheight()) // 3
        win.geometry(f"+{max(0, x)}+{max(0, y)}")
        win.deiconify()
        cancel_btn.focus_set()
        win.grab_set()
        self.wait_window(win)
        return result["ok"]

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
            self.cluster_var.set("64 Kilobytes")
            self.label_hint.configure(text="Bis zu 11 Zeichen: A–Z, 0–9, Leerzeichen, - und _")
        else:
            self.cluster_var.set(f"{fat32_cluster_kb(dev['size'])} Kilobytes (Standard)" if dev else "Standardgröße")
            self.label_hint.configure(text="Bis zu 11 Zeichen, wird bei FAT32 großgeschrieben")

        idle = not self.busy
        pc = self.mode_var.get() == "pc"

        def enable(w, on, *extra):
            w.state(list(extra) + (["!disabled"] if on else ["disabled"]))

        for w in (self.host_entry, self.label_entry, self.search_btn, self.verify_chk, self.folders_chk,
                  *self.mode_btns):
            enable(w, idle)
        enable(self.reload_btn, idle and (pc or self.api))
        enable(self.dev_combo, idle and self.devices, "readonly")
        enable(self.fs_combo, idle, "readonly")
        enable(self.start_btn, idle and dev and (pc or formattable(dev)))
        enable(self.speed_btn, idle and dev)
        enable(self.del_btn, idle and not pc and self.api and self.games_tree.selection())
        enable(self.uninstall_chk, idle)
        enable(self.rename_btn, idle and dev and any(v["fs"] in ("exFAT", "FAT32") for v in dev.get("volumes", [])))
        # Abbrechen nur bei Warten auf Abziehen, Echtheitsprüfung und Tempomessung (Formatieren selbst dauert Sekunden)
        ps5_cancellable = (not pc and self.formatting and not self.pc_job and
                           (is_replug_step(self.ps5_step) or re.search(r"Echtheit|[Tt]empo|^Prüfen", self.ps5_step)))
        if (self.pc_job and getattr(self.pc_job, "cancellable", False)) or ps5_cancellable:
            if not self.cancel_btn.winfo_ismapped():
                self.cancel_btn.pack(side="right", padx=(0, 6))
        elif self.cancel_btn.winfo_ismapped():
            self.cancel_btn.pack_forget()

    def _mode_changed(self, initial=False):
        pc = self.mode_var.get() == "pc"
        if not initial:
            self.settings["mode"] = self.mode_var.get()
            save_settings(self.settings)
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
                self.status_var.set(f"Verbunden mit {self.host_var.get().strip()}")
            elif initial or not self.consoles:
                self.after(200 if initial else 0, self._search)
            else:
                self.status_var.set("Nicht verbunden")
        self._update()

    def _ps5_dev_text(self, d):
        """Kurz, damit die Auswahlliste nichts abschneidet; Erklärungen stehen in den Rückfragen"""
        fmt = d.get("format") or ""
        fmt = {"leer": "leer", "keine Partition": "ohne Partition", "unbekannt": "Format unbekannt"}.get(fmt, fmt)
        if d.get("label") and fmt:
            fmt += f" „{d['label']}“"
        where = "gesperrt" if d["locked"] else "eingebunden" if d["mounts"] else "frei"
        parts = [f"{dev_title(d)} ({d['name']})", fmt_size(d["size"])] + ([fmt] if fmt else []) + [where]
        if d.get("fake_real"):
            parts.append(f"GEFÄLSCHT – echt nur {fmt_size(d['fake_real'])}")
        n = len(self.games.get(d["name"], []))
        if n:
            parts.append(f"{n} Spiel{'e' if n != 1 else ''}")
        return " · ".join(parts)

    def _show_games(self):
        """Spiele des gewählten PS5-Datenträgers in der Liste darunter"""
        self.games_tree.delete(*self.games_tree.get_children())
        dev = self._selected_device()
        if self.mode_var.get() != "ps5" or not dev:
            self.games_caption.configure(text="Spiele auf diesem Datenträger:")
            return
        games = self.games.get(dev["name"], [])
        for g in sorted(games, key=lambda g: g["name"].lower()):
            self.games_tree.insert("", "end", iid=g.get("key") or None, values=(g["name"] or g["id"], g["id"], g["kind"],
                                                      fmt_size(g["size"]) if g["size"] else "–"))
        if games:
            text = f"Spiele auf diesem Datenträger: {len(games)}"
        elif self.lib_loading:
            text = "Spiele auf diesem Datenträger: werden gesucht …"
        else:
            text = "Spiele auf diesem Datenträger: keine gefunden"
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
                self.q.put(("library", data if isinstance(data, dict) else None))

        threading.Thread(target=self._guard(work, background=True), daemon=True).start()

    def _fill_pc_devices(self, devices):
        prev = self._selected_device()
        self.devices = devices
        self.dev_combo.configure(values=[pc_dev_text(d) for d in devices])
        # am PC wird nie vorausgewählt: dort hängen oft Platten mit Daten (Spiele-Sicherung, Backups)
        idx = next((i for i, d in enumerate(devices) if prev and d["disk"] == prev.get("disk")
                    and d["size"] == prev.get("size")), -1)
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_var.set("")
        if not devices:
            self.status_var.set("Kein USB-Datenträger am PC gefunden. Stick einstecken und „Aktualisieren“ klicken.")
        else:
            self.status_var.set(f"{len(devices)} USB-Datenträger am PC gefunden – bitte oben auswählen.")

    def _fill_devices(self, devices):
        for d in devices:   # ältere/andere Payload-Versionen: fehlende Felder ergänzen
            for k, default in (("name", "?"), ("model", ""), ("size", 0), ("locked", False), ("mounts", []),
                               ("format", ""), ("label", ""), ("fake_real", 0)):
                d.setdefault(k, default)
        self.ps5_devices = devices
        if self.mode_var.get() == "pc":
            return
        prev = self._selected_device()
        if prev and "disk" in prev:
            prev = None
        self.devices = devices
        names = []
        for d in devices:
            names.append(self._ps5_dev_text(d))
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
        if not devices and self.api:
            self.status_var.set("Kein USB-Stick an der PS5 gefunden. Stick einstecken und „Aktualisieren“ klicken.")
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
                self.q.put(("bgerror" if background else "error", f"Unerwarteter Fehler: {e}"))
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
        # alte Verbindung vergessen: scheitert die neue, darf "Starten" nicht die vorige PS5 formatieren
        self.api = None
        self.games = {}
        self._fill_devices([])

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
                self.q.put(("error", f"Geräteliste konnte nicht geladen werden: {e}"))

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
        fs_text = "exFAT mit 64 KB Zuordnungseinheiten" if fs == "exfat" else "FAT32"
        replug = bool(dev["mounts"])
        replug_text = ("Die PS5 nutzt diesen Datenträger gerade. Der Formatierer meldet ihn per Software kurz ab "
                       "und übernimmt ihn, bevor die PS5 ihn wieder einbindet. Klappt das nicht, bittet er, den "
                       "Stick an der PS5 abzuziehen und wieder einzustecken. Spiele und Apps, die davon laufen, "
                       "vorher beenden.\n\n") if replug else ""
        limit = dev.get("fake_real") or 0
        if limit:
            replug_text = (f"GEFÄLSCHTER STICK: Er meldet {fmt_size(dev['size'])}, hat aber nur {fmt_size(limit)} echten "
                           f"Speicher. Es wird nur dieser Bereich formatiert. Für Spiele an der PS5 besser einen echten "
                           f"Datenträger verwenden.\n\n") + replug_text
        games = sorted(g["name"] or g["id"] for g in self.games.get(dev["name"], []))
        ack = None
        if games:
            shown = "\n".join("• " + g for g in games[:8]) + (f"\n… und {len(games) - 8} weitere" if len(games) > 8 else "")
            replug_text = (f"Darauf liegen {len(games)} Spiel{'e' if len(games) != 1 else ''}, die gelöscht werden:\n"
                           f"{shown}\n\n") + replug_text
            ack = "Ich weiß, dass diese Spiele unwiderruflich gelöscht werden"
        elif dev["mounts"] and (self.lib_loading or not self.lib_known):
            # Inhalt noch unbekannt: lieber ausdrücklich bestätigen lassen
            replug_text = "Der Inhalt wird noch durchsucht – möglicherweise liegen Spiele darauf.\n\n" + replug_text
            ack = "Ich weiß, dass alles darauf unwiderruflich gelöscht wird"
        verify = bool(self.verify_var.get())
        if verify:
            replug_text += "Vorher werden Echtheit und Tempo geprüft (einige Minuten).\n\n"
            limit = 0   # die Prüfung testet den ganzen Stick erneut
        if not self._confirm(
                "Alle Daten werden gelöscht",
                f"{dev_title(dev)} ({dev['name']}, {fmt_size(dev['size'])}) wird als {fs_text} formatiert.\n\n"
                f"{replug_text}Das lässt sich nicht rückgängig machen.",
                "Formatieren", ack):
            return
        if self.busy or not self.api:   # während der Rückfrage kann ein anderer Auftrag begonnen haben
            return
        self.settings.update(label=label, verify=verify, folders=self.folders_var.get())
        save_settings(self.settings)
        folders = bool(self.folders_var.get())

        def after(ok, msg, result):
            """eigenes Formatieren: bei gefälschtem Stick anbieten, nur den echten Bereich zu nutzen"""
            if ok:
                messagebox.showinfo("PS5-USB formatieren", "Datenträger erfolgreich formatiert.\n\n" + msg, parent=self)
            elif result.get("fake") and result.get("real", 0) >= 64 * 1024 * 1024:
                real = result["real"]
                if self._confirm("Gefälschter Speicher erkannt",
                                 f"{msg}\n\nNur die echten {fmt_size(real)} formatieren? Für Spiele an der PS5 besser "
                                 f"einen echten Datenträger verwenden.", f"Nur echte {fmt_size(real)} formatieren"):
                    self._ps5_format_job(dev, fs, label, folders, False, real, after)
            else:
                messagebox.showerror("PS5-USB formatieren", msg, parent=self)

        self._ps5_format_job(dev, fs, label, folders, verify, limit, after)

    def _ps5_format_job(self, dev, fs, label, folders, verify, limit, after):
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                api.format(dev["name"], fs, label, dev["size"], bool(dev["mounts"]), limit, folders, verify)
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", f"Formatieren konnte nicht gestartet werden: {e}"))
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
        shown = "\n".join("• " + n for n in names[:10]) + (f"\n… und {len(names) - 10} weitere" if len(names) > 10 else "")
        what = "Dieses Spiel wird" if len(names) == 1 else f"Diese {len(names)} Spiele werden"
        if not messagebox.askokcancel(
                "Spiele löschen",
                f"{what} von {dev_title(dev)} ({dev['name']}) gelöscht:\n\n{shown}\n\n"
                + ("Die Kacheln werden auch von der PS5-Startseite entfernt.\n\n" if uninstall else
                   "Die Kacheln bleiben auf der Startseite (ohne Spiel).\n\n")
                + "Laufende Spiele vorher beenden. Das lässt sich nicht rückgängig machen.",
                icon="warning", default="cancel", parent=self):
            return
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                api.delete_games(keys, uninstall)
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", f"Löschen konnte nicht gestartet werden: {e}"))
                return
            self._follow(api, foreign=False, done_title="Spiele gelöscht.")

        self.pbar["value"] = 0
        self._run(work)

    def _follow(self, api, foreign, done_title="Datenträger erfolgreich formatiert.", after=None):
        """Auftrag auf der PS5 bis zum Ende verfolgen (im Thread). foreign: woanders gestartet (PS5-Browser,
        anderer PC) - dann nur anzeigen, ohne Rückfrage-Dialoge des Starters."""
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
                    self.q.put(("finished", False, "Keine Verbindung zur PS5 seit 3 Minuten. Der Auftrag läuft dort "
                                "vermutlich weiter – den Stick NICHT abziehen. Später „Suchen“ klicken, um das Ergebnis "
                                "zu sehen.", foreign, done_title))
                    return
                if offline >= 5:
                    self.q.put(("status", "Verbindung zur PS5 unterbrochen – der Auftrag läuft dort weiter, frage weiter "
                                          "nach …"))
                time.sleep(min(2.0, offline / 10))   # sanft weiterfragen
                continue
            if not st.get("busy") and not st.get("done"):
                # Formatierer wurde zwischendurch neu gestartet: Ergebnis unbekannt
                self.q.put(("finished", False, "Der Formatierer auf der PS5 wurde zwischendurch neu gestartet – das "
                            "Ergebnis ist unbekannt. Bitte den Datenträger prüfen bzw. erneut formatieren.",
                            foreign, done_title))
                return
            self.q.put(("progress", int(st.get("percent") or 0), st.get("step") or ""))
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
            self.q.put(("status", "Suche USB-Datenträger am PC …"))
            try:
                self.q.put(("pcdevices", pc_list()))
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as e:
                self.q.put(("error", f"USB-Datenträger konnten nicht gelesen werden: {e}"))

        self._run(work)

    def _pc_run(self, op, args, cancellable=False, after=None):
        """Auftrag mit Administratorrechten ausführen; after(ok, meldung, info) läuft danach im UI-Thread"""
        try:
            hwnd = int(self.wm_frame(), 16)   # im UI-Thread holen: Elternfenster der UAC-Abfrage
        except (tk.TclError, ValueError):
            hwnd = None

        def work():
            try:
                job = PcJob(args, hwnd)
            except (OSError, RuntimeError) as e:
                self.q.put(("error", str(e)))
                return
            job.cancellable = cancellable
            self.pc_job = job
            self.q.put(("formatting", False))
            ok, msg, info = job.follow(lambda pct, step: self.q.put(("progress", pct, step)))
            try:
                devices = pc_list()
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError):
                devices = None
            self.q.put(("pcdone", op, ok, msg, info, devices, after))

        self.pbar["value"] = 0
        self.status_var.set("Windows fragt nach Administratorrechten …")
        self._run(work)

    def _pc_volumes_text(self, dev):
        lines = []
        for v in dev["volumes"]:
            used = v["total"] - v["free"] if v["total"] else 0
            t = f"Laufwerk {v['letters'] or '(ohne Buchstaben)'}"
            if v["label"]:
                t += f" „{v['label']}“"
            if v["fs"]:
                t += f", {v['fs']}"
            if v["total"]:
                t += f", {fmt_size(used)} belegt"
            lines.append(t)
        return "\n".join(lines)

    def _pc_format(self, dev, limit=0):
        label = self.label_var.get()
        fs = "exfat" if self.fs_var.get() == "exFAT" else "fat32"
        fs_text = "exFAT mit 64 KB Zuordnungseinheiten" if fs == "exfat" else "FAT32"
        if not limit:
            vols = self._pc_volumes_text(dev)
            used = pc_used(dev)
            has_data = used > 64 * 1024 * 1024
            data_warn = f"\n\nAuf dem Datenträger liegen {fmt_size(used)} Daten – sie sind danach weg." if has_data else ""
            if not self._confirm(
                    "Alle Daten werden gelöscht",
                    f"{dev['model'] or 'USB-Datenträger'} (Datenträger {dev['disk']}, {fmt_size(dev['size'])})\n"
                    f"{vols + chr(10) if vols else ''}"
                    f"\nwird als {fs_text} für die PS5 formatiert.{data_warn}\n\n"
                    f"Das lässt sich nicht rückgängig machen. Windows fragt anschließend nach Administratorrechten.",
                    "Formatieren",
                    f"Ich weiß, dass die {fmt_size(used)} Daten unwiderruflich gelöscht werden" if has_data else None):
                return
        if self.busy or self.mode_var.get() != "pc":
            return
        self.settings.update(label=label, verify=self.verify_var.get(), folders=self.folders_var.get())
        save_settings(self.settings)
        args = ["format", str(dev["disk"]), str(dev["size"]), fs, label]
        if self.verify_var.get() and not limit:
            args.append("verify")
        if self.folders_var.get():
            args.append("folders")
        if limit:
            args.append(f"limit={limit}")

        def after(ok, msg, info):
            if ok:
                messagebox.showinfo("USB-Datenträger am PC", "Datenträger erfolgreich formatiert.\n\n" + msg, parent=self)
            elif info.get("fake") and info.get("real", 0) >= 64 * 1024 * 1024:
                real = info["real"] // (1024 * 1024) * 1024 * 1024
                if messagebox.askyesno(
                        "Gefälschte Größe", f"{msg}\n\nNur den echten Bereich ({fmt_size(real)}) formatieren? "
                        "Dann gehen keine Daten verloren, die über diese Grenze hinaus geschrieben würden.",
                        icon="warning", parent=self):
                    self._pc_format(dev, limit=real)
            else:
                messagebox.showerror("USB-Datenträger am PC", msg, parent=self)

        self._pc_run("format", args, cancellable=bool(self.verify_var.get()) and not limit, after=after)

    def _pc_speed(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)("Tempo messen", msg, parent=self)

        self._pc_run("speed", ["speed", str(dev["disk"]), str(dev["size"])], cancellable=True, after=after)

    def _pc_rename(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return
        current = next((v["label"] for v in dev["volumes"] if v["fs"] in ("exFAT", "FAT32")), "")
        label = simpledialog.askstring("Umbenennen", "Neue Bezeichnung (bis zu 11 Zeichen: A–Z, 0–9, Leerzeichen, - "
                                       "und _; leer = Bezeichnung entfernen):", initialvalue=current, parent=self)
        if label is None or label == current:
            return
        if not LABEL_RE.match(label):
            messagebox.showerror("Umbenennen", "Nur A–Z, 0–9, Leerzeichen, - und _ erlaubt (höchstens 11 Zeichen).",
                                 parent=self)
            return
        target = f"„{label}“" if label else "keine Bezeichnung (wird entfernt)"
        if not self._confirm(
                "Umbenennen", f"{pc_dev_text(dev)}\n\nNeue Bezeichnung: {target}\n\nDie Daten bleiben erhalten. Das "
                "Laufwerk wird dafür kurz abgemeldet – geöffnete Dateien darauf vorher schließen.",
                "Umbenennen" if label else "Entfernen"):
            return
        if self.busy or self.mode_var.get() != "pc":
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)("Umbenennen", msg, parent=self)

        self._pc_run("label", ["label", str(dev["disk"]), str(dev["size"]), label], after=after)

    def _cancel(self):
        """Abbrechen: am PC über die Abbruch-Datei des Helfers, an der PS5 über /api/cancel"""
        self.cancel_btn.state(["disabled"])
        self.status_var.set("Wird abgebrochen …")
        if self.pc_job:
            self.pc_job.cancel()
        elif self.api:
            api = self.api

            def work():
                try:
                    api.cancel()
                except (ConnectionError, ApiError) as e:
                    self.q.put(("bgerror", f"Abbrechen nicht möglich: {e}"))

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
                    self.status_var.set(f"Unerwarteter Fehler: {e}")
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
            self.status_var.set(f"Verbunden mit {self.host_var.get().strip()}")
            self._fill_devices(msg[2].get("devices") or [])
            self._load_library()
        elif kind == "devices":
            self.busy = self.formatting
            self._fill_devices(msg[1].get("devices") or [])
            self._load_library()
        elif kind == "formatting":
            self.formatting = True
        elif kind == "foreign":
            if not self.busy and self.mode_var.get() == "ps5":   # woanders gestartet: hier mitverfolgen
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
            self.ps5_step = step   # für "Abbrechen" (Warten auf Abziehen, Prüfung, Messung)
        elif kind == "finished":
            self.formatting = False
            self.busy = False
            self.pbar.stop()
            self.pbar.configure(mode="determinate")
            self.status_var.set(msg[2])
            foreign = len(msg) > 3 and msg[3]
            done_title = msg[4] if len(msg) > 4 else "Datenträger erfolgreich formatiert."
            result = msg[5] if len(msg) > 5 else {}
            after = msg[6] if len(msg) > 6 else None
            self.ps5_step = ""
            if after and not foreign:   # der Auftraggeber entscheidet selbst (z. B. Fälschung: nur echten Bereich)
                self.pbar["value"] = 100 if msg[1] else 0
                self._update()
                after(msg[1], msg[2], result)
            elif msg[1]:
                self.pbar["value"] = 100
                if not foreign:
                    messagebox.showinfo("PS5-USB formatieren", done_title + "\n\n" + msg[2], parent=self)
            else:
                self.pbar["value"] = 0
                if not foreign:
                    messagebox.showerror("PS5-USB formatieren", msg[2], parent=self)
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
            _, op, ok, text, info, devices, after = msg
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
            messagebox.showerror("PS5-USB formatieren", msg[1], parent=self)
        elif kind == "bgerror":   # Hintergrund (Spieleliste, Wächter): laufende Aufträge nicht berühren
            self.status_var.set(msg[1])

    def _on_close(self):
        if self.formatting and not messagebox.askyesno(
                self.title(), ("Der Auftrag läuft am PC im Hintergrund weiter." if self.pc_job else
                               "Die Formatierung läuft auf der PS5 weiter.") + " Fenster trotzdem schließen?",
                icon="warning", parent=self):
            return
        if self.pc_job and getattr(self.pc_job, "cancellable", False):
            self.pc_job.cancel()   # Prüfung/Tempo am PC nicht ohne Fenster weiterlaufen lassen
        self.after_cancel(self._poll_id)
        self.after_cancel(self._watch_id)
        self.destroy()

    def _on_exception(self, exc, val, tb):
        messagebox.showerror(self.title(), f"Unerwarteter Fehler: {val}", parent=self)


if __name__ == "__main__":
    enable_dpi_awareness()
    App().mainloop()
