"""Gemeinsamer Teil für USB-Datenträger am PC: Windows-Helfer (usbfmt_win.exe) finden, prüfen und mit
Administratorrechten starten, Datenträger auflisten, Einstellungen, Rückfrage-Dialog.
Genutzt vom PS5-USB-Formatierer und vom eigenständigen USB-Formatierer (ohne PS5-Teile)."""
import ctypes
import hashlib
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
import tkinter as tk
from tkinter import ttk

try:   # von build_helper.ps1 erzeugt: Prüfsumme des ausgelieferten Helfers
    from helper_hash import HELPER_SHA256
except ImportError:   # Entwicklung ohne gebauten Helfer: nur sperren, nicht vergleichen
    HELPER_SHA256 = ""

TEMP_PREFIX = "USBFormatierer"


def resource_path(rel):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, rel)


def settings_file(app_name):
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, app_name, "settings.json")


def load_settings(app_name):
    try:
        with open(settings_file(app_name), encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(app_name, data):
    """Atomar (erst neue Datei, dann ersetzen): Ein Absturz mitten im Schreiben hinterlässt keine halbe Datei"""
    path = settings_file(app_name)
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(tmp, path)
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


def enable_dpi_awareness():
    if sys.platform == "win32":
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            try:
                ctypes.windll.user32.SetProcessDPIAware()
            except Exception:
                pass


# ---------------------------------------------------------------------- Windows-Helfer

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
        d.setdefault("ss", 512)
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
    vols = ", ".join(parts) if parts else "kein Windows-Laufwerk (leer, Linux oder Mac)"
    # Datenträgernummer macht den Eintrag eindeutig (zwei gleiche leere Sticks)
    return f"{d['model'] or 'USB-Datenträger'} (Datenträger {d['disk']}) – {fmt_size(d['size'])} – {vols}"


def pc_volumes_text(dev):
    """Laufwerke des Datenträgers für Rückfragen, eines je Zeile"""
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
            d = os.path.join(tempfile.gettempdir(), f"{TEMP_PREFIX}-{secrets.token_hex(8)}")
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
        self.cancellable = False
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


# ---------------------------------------------------------------------- Oberfläche

def confirm(root, title, text, ok_text, ack=None):
    """Rückfrage mit eigenen Knopftexten; mit ack muss erst ein Häkchen gesetzt werden (wie in der Web-UI).
    Abbrechen ist vorausgewählt, Esc bricht ab. Liefert True bei Bestätigung."""
    win = tk.Toplevel(root)
    win.withdraw()
    win.title(title)
    win.transient(root)
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
    top = root.winfo_toplevel()
    x = top.winfo_rootx() + (top.winfo_width() - win.winfo_reqwidth()) // 2
    y = top.winfo_rooty() + (top.winfo_height() - win.winfo_reqheight()) // 3
    win.geometry(f"+{max(0, x)}+{max(0, y)}")
    win.deiconify()
    cancel_btn.focus_set()
    win.grab_set()
    root.wait_window(win)
    return result["ok"]


def keep_on_screen(win):
    """Ragt das Fenster nach einer Größenänderung unten aus dem Bildschirm, nach oben schieben (Taskleiste frei)"""
    win.update_idletasks()
    h, y, sh = win.winfo_height(), win.winfo_y(), win.winfo_screenheight()
    if y + h + 80 > sh:
        win.geometry(f"+{win.winfo_x()}+{max(0, sh - h - 80)}")


def hwnd_of(widget):
    """Fenster-Handle (im UI-Thread holen): Elternfenster der UAC-Abfrage"""
    try:
        return int(widget.winfo_toplevel().wm_frame(), 16)
    except (tk.TclError, ValueError):
        return None
