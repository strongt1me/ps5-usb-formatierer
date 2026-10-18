"""Gemeinsamer Teil für USB-Datenträger am PC: den Helfer (Windows: usbfmt_win.exe, Linux: usbfmt_linux) finden,
prüfen und mit Administratorrechten starten (Windows: UAC, Linux: pkexec - siehe pc_linux), Datenträger auflisten,
Einstellungen, Rückfrage-Dialog, Hilfe. Genutzt vom PS5-USB-Formatierer und vom eigenständigen USB-Formatierer."""
import ctypes
import hashlib
import json
import os
import re
import secrets
import struct
import subprocess
import sys
import tempfile
import time
import tkinter as tk
from tkinter import messagebox, ttk

from i18n import _, lang, number, quote, unit

try:   # von build_helper.ps1 erzeugt: Prüfsumme des ausgelieferten Helfers
    from helper_hash import HELPER_SHA256
except ImportError:   # Entwicklung ohne gebauten Helfer: nur sperren, nicht vergleichen
    HELPER_SHA256 = ""

TEMP_PREFIX = "USBFormatierer"
IS_WIN = sys.platform == "win32"
HELPER_NAME = "usbfmt_win.exe" if IS_WIN else "usbfmt_linux"


def resource_path(rel):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, rel)


def settings_file(app_name):
    if IS_WIN:
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    else:   # Linux: ~/.config (bzw. XDG_CONFIG_HOME)
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, app_name, "settings.json")


# bekannte Einstellungen und ihr Typ - von Hand verbogene Werte (Liste statt Text, Zahl statt Ja/Nein) fallen beim
# Laden weg, statt das Programm am Start scheitern zu lassen
_SETTING_TYPES = {"lang": str, "mode": str, "host": str, "label": str, "fs": str, "scheme": str,
                  "verify": bool, "folders": bool, "quick": bool, "expert": bool, "standard": dict}
_LABEL_OK = re.compile(r"^[A-Za-z0-9 _-]{0,32}\Z")


def _clean_settings(data):
    out = {}
    for k, v in data.items():
        want = _SETTING_TYPES.get(k)
        if want is None:
            out[k] = v
        elif isinstance(v, want):
            if want is dict:
                v = _clean_settings(v)
            if k == "label" and not _LABEL_OK.match(v):
                continue   # sonst ginge sie ungeprüft an den Formatierer
            out[k] = v
    return out


def load_settings(app_name):
    try:
        with open(settings_file(app_name), encoding="utf-8") as f:
            data = json.load(f)
        return _clean_settings(data) if isinstance(data, dict) else {}
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
    return number(text) + " " + unit(units[i])


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
    for p in (resource_path(HELPER_NAME), os.path.join(os.path.dirname(os.path.abspath(__file__)), HELPER_NAME)):
        if os.path.isfile(p):
            if not IS_WIN and not os.access(p, os.X_OK):   # ausgepackt ohne Ausführrecht
                try:
                    os.chmod(p, 0o755)
                except OSError:
                    pass
            return os.path.abspath(p)
    return None


def helper_digest():
    """Prüfsumme des mitgelieferten Helfers (vom Bau erzeugt; leer in der Entwicklung)"""
    return HELPER_SHA256


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
        raise OSError(_("Der Helfer usbfmt_win.exe lässt sich nicht öffnen (wird er gerade verändert?)."))
    try:
        with open(exe, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
    except OSError as e:
        k32.CloseHandle(ctypes.c_void_p(h))
        raise OSError(_("Der Helfer usbfmt_win.exe lässt sich nicht lesen: {e}", e=e)) from None
    if HELPER_SHA256 and digest != HELPER_SHA256:
        k32.CloseHandle(ctypes.c_void_p(h))
        raise OSError(_("Der Helfer usbfmt_win.exe wurde verändert – aus Sicherheitsgründen nicht gestartet. "
                        "Bitte das Programm neu herunterladen."))
    return h


def pc_list():
    """USB-Datenträger am PC (ohne Administratorrechte). Die Systemplatte meldet der Helfer nie."""
    exe = find_helper()
    if not exe:
        raise RuntimeError(_("Der Helfer {name} fehlt neben dem Programm.", name=HELPER_NAME))
    r = subprocess.run([exe, "list"], capture_output=True, timeout=30,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if r.returncode != 0:
        raise RuntimeError(_("Helfer meldet Fehler {code}: {text}", code=r.returncode,
                             text=r.stderr.decode("utf-8", "replace").strip() or _("keine Angabe")))
    devices = json.loads(r.stdout.decode("utf-8", "replace") or "[]")
    for d in devices:   # fehlende Felder (andere Helfer-Version) nicht zum Absturz werden lassen
        d.setdefault("model", "")
        d.setdefault("serial", "")
        d.setdefault("ss", 512)
        d.setdefault("volumes", [])
        for v in d["volumes"]:
            for k, default in (("letters", ""), ("label", ""), ("fs", ""), ("total", 0), ("free", 0)):
                v.setdefault(k, default)
    return devices


def serial_args(d):
    """Seriennummer laut Liste für den Helfer: steckt unter der Nummer inzwischen ein baugleicher anderer Datenträger,
    lehnt er ab"""
    return [f"serial={d['serial']}"] if d.get("serial") else []


def pc_used(d):
    return sum(v["total"] - v["free"] for v in d["volumes"] if v["total"])


def disk_name(d):
    """Datenträger, wie das System ihn nennt (Windows: 3, Linux: /dev/sdb)"""
    return str(d["disk"]) if IS_WIN else f"/dev/{d['disk']}"


def pc_dev_text(d):
    parts = []
    for v in d["volumes"]:
        t = v["letters"] or (_("ohne Buchstaben") if IS_WIN else v.get("device") or _("nicht eingehängt"))
        if v["label"]:
            t += " " + quote(v["label"])
        if v["fs"]:
            t += f" {v['fs']}"
        parts.append(t)
    vols = ", ".join(parts) if parts else (_("kein Windows-Laufwerk (leer, Linux oder Mac)") if IS_WIN
                                           else _("ohne erkanntes Dateisystem (leer)"))
    # Datenträgernummer macht den Eintrag eindeutig (zwei gleiche leere Sticks)
    return _("{model} (Datenträger {disk}) – {size} – {vols}", model=d["model"] or _("USB-Datenträger"),
             disk=disk_name(d), size=fmt_size(d["size"]), vols=vols)


def pc_volumes_text(dev):
    """Laufwerke des Datenträgers für Rückfragen, eines je Zeile"""
    lines = []
    for v in dev["volumes"]:
        used = v["total"] - v["free"] if v["total"] else 0
        t = _("Laufwerk {letters}", letters=v["letters"] or (_("(ohne Buchstaben)") if IS_WIN
                                                             else v.get("device") or _("nicht eingehängt")))
        if v["label"]:
            t += " " + quote(v["label"])
        if v["fs"]:
            t += f", {v['fs']}"
        if v["total"]:
            t += ", " + _("{size} belegt", size=fmt_size(used))
        lines.append(t)
    return "\n".join(lines)


# ---------------------------------------------------------------------- Image-Sicherung (.img.zst)

IMAGE_EXT = ".img.zst"
_IMG_MAGIC, _ZSTD_MAGIC = 0x184D2A5A, 0xFD2FB528


def image_info(path):
    """Angaben einer Image-Datei: eigene .img.zst -> dict mit size, used, model, label, fs, created; mit zstd selbst
    gepacktes Image -> {"size": ...} bzw. {} (Größe unbekannt); None = keine zstd-Datei. OSError bei Lesefehlern."""
    with open(path, "rb") as f:
        head = f.read(18)
        if len(head) < 8:
            return None
        magic, n = struct.unpack_from("<II", head)
        if magic == _ZSTD_MAGIC:   # Rahmenkopf: Inhaltsgröße, falls angegeben
            fhd = head[4]
            single, fcs = fhd >> 5 & 1, fhd >> 6
            off = 5 + (0 if single else 1) + (0, 1, 2, 4)[fhd & 3]
            width = (1 if single else 0, 2, 4, 8)[fcs]
            if not width or len(head) < off + width:
                return {}
            size = int.from_bytes(head[off:off + width], "little") + (256 if width == 2 else 0)
            return {"size": size}
        if magic != _IMG_MAGIC or n > 64 * 1024 * 1024:
            return None
        f.seek(8)
        data = f.read(min(n, 8192))
        tag = data[:15]   # "USBFMT-IMAGE 2\n" (2.5.0: Fassung 1)
        if tag not in (b"USBFMT-IMAGE 1\n", b"USBFMT-IMAGE 2\n"):
            return None
        try:
            info = json.loads(data[len(tag):].split(b"\n", 1)[0].decode("utf-8", "replace"))
        except ValueError:
            return None
        return info if isinstance(info, dict) and isinstance(info.get("size"), int) else None


def image_file_name(dev):
    """Vorschlag "<Modell>_<Bezeichnung>_<Datum_Zeit>.img.zst" (nur A-Z a-z 0-9 - _), wie der PS5-Teil"""
    def clean(t):
        return re.sub(r"_+", "_", re.sub(r"[^A-Za-z0-9-]", "_", t or "")).strip("_")
    vols = dev.get("volumes", [])
    parts = [clean(dev.get("model")) or "USB", clean(vols[0]["label"]) if len(vols) == 1 else "",
             time.strftime("%Y-%m-%d_%H_%M")]
    return "_".join(p for p in parts if p) + IMAGE_EXT


def on_device(path, dev):
    """Liegt die Datei auf einem Laufwerk dieses Datenträgers? (Das wird beim Sichern/Wiederherstellen gesperrt.)"""
    full = os.path.realpath(os.path.abspath(path))
    for v in dev.get("volumes", []):
        if IS_WIN:
            drive = os.path.splitdrive(full)[0].upper()
            if drive and drive in v["letters"].upper().split():
                return True
        elif v["letters"] and (full == v["letters"] or full.startswith(v["letters"].rstrip("/") + "/")):
            return True
    return False


class _ShellExecuteInfo(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_ulong), ("fMask", ctypes.c_ulong), ("hwnd", ctypes.c_void_p),
                ("lpVerb", ctypes.c_wchar_p), ("lpFile", ctypes.c_wchar_p), ("lpParameters", ctypes.c_wchar_p),
                ("lpDirectory", ctypes.c_wchar_p), ("nShow", ctypes.c_int), ("hInstApp", ctypes.c_void_p),
                ("lpIDList", ctypes.c_void_p), ("lpClass", ctypes.c_wchar_p), ("hkeyClass", ctypes.c_void_p),
                ("dwHotKey", ctypes.c_ulong), ("hIconOrMonitor", ctypes.c_void_p), ("hProcess", ctypes.c_void_p)]


class _WinJob:
    """Startet den Helfer mit Administratorrechten (UAC-Abfrage von Windows) und liest seine Ausgabedatei mit"""
    STALL_S = 30 * 60   # so lange ohne neue Ausgabe: Helfer gilt als hängend

    def __init__(self, args, hwnd=None):
        exe = find_helper()
        if not exe:
            raise RuntimeError(_("Der Helfer {name} fehlt neben dem Programm.", name=HELPER_NAME))
        lock = lock_helper(exe)   # bis der Prozess läuft: niemand kann die Datei austauschen
        # eigener Ordner mit Zufallsnamen je Auftrag: ein fester Ordner ließe sich vorab umleiten (Junction).
        # Nicht tempfile.mkdtemp: Das gibt unter Windows nur "Eigentümer" Rechte - die Ausgabedatei legt aber der
        # Helfer als Administrator an (Eigentümer: Administratoren), das Programm ohne Adminrechte dürfte sie dann
        # nicht lesen. os.mkdir erbt die Rechte des Temp-Ordners (dort steht der Benutzer selbst).
        self.tmpdir = None
        for _attempt in range(20):
            d = os.path.join(tempfile.gettempdir(), f"{TEMP_PREFIX}-{secrets.token_hex(8)}")
            try:
                os.mkdir(d)
                self.tmpdir = d
                break
            except FileExistsError:
                continue
        if not self.tmpdir:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(lock))
            raise OSError(_("Arbeitsordner im Temp-Verzeichnis ließ sich nicht anlegen."))
        self.out = os.path.join(self.tmpdir, "auftrag.txt")
        self.cancel_file = os.path.join(self.tmpdir, "abbruch")
        self.cancellable = False
        sei = _ShellExecuteInfo()
        sei.cbSize = ctypes.sizeof(sei)
        sei.fMask = 0x40 | 0x400          # SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI
        sei.lpVerb = "runas"
        sei.lpFile = exe
        sei.lpParameters = subprocess.list2cmdline(["--out", self.out, "--cancel", self.cancel_file, "--lang", lang()]
                                                   + args)
        sei.nShow = 0                     # SW_HIDE
        sei.hwnd = hwnd                   # UAC-Abfrage vor dem Programmfenster statt blinkend in der Taskleiste
        shell32 = ctypes.WinDLL("shell32", use_last_error=True)
        try:
            if not shell32.ShellExecuteExW(ctypes.byref(sei)):
                code = ctypes.get_last_error()
                self._cleanup()
                if code == 1223:   # ERROR_CANCELLED: Benutzer hat die UAC-Abfrage abgelehnt
                    raise PermissionError(_("Ohne Administratorrechte lässt Windows keine Datenträger bearbeiten."))
                raise OSError(_("Helfer konnte nicht gestartet werden (Windows-Fehler {code}).", code=code))
        finally:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(lock))   # läuft jetzt: Windows sperrt die Datei selbst
        if not sei.hProcess:
            self._cleanup()
            raise OSError(_("Helfer wurde gestartet, aber Windows liefert keinen Prozess dazu."))
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
                # nur an \n trennen - splitlines() trennte auch an U+2028 & Co. mitten in einer Meldung
                for line in data[:end].decode("utf-8", "replace").replace("\r\n", "\n").rstrip("\n").split("\n"):
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
                    result = (False, _("Der Helfer wurde unerwartet beendet (Code {code}).", code=code.value), {})
                elif result is None and time.time() - last_change > self.STALL_S:
                    self.cancel()
                    # wirklich beenden, sonst schreibt er womöglich weiter, während schon ein neuer Auftrag läuft.
                    # Mitten im Formatieren abgebrochen bleibt ein leerer Stick (Tabelle wird zuletzt geschrieben).
                    ctypes.windll.kernel32.TerminateProcess(ctypes.c_void_p(self.proc), 1)
                    result = (False, _("Der Helfer meldet seit 30 Minuten nichts mehr – abgebrochen."), {})
                elif result is None:
                    time.sleep(0.3)
        finally:
            ctypes.windll.kernel32.WaitForSingleObject(ctypes.c_void_p(self.proc), 5000)   # Datei freigeben lassen
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.proc))
            self._cleanup()
        return result


if IS_WIN:
    PcJob = _WinJob
else:
    import pc_linux   # noqa: E402  (pkexec statt UAC)
    PcJob = pc_linux.LinuxJob


def admin_wait_text():
    """Statuszeile, solange das System nach den Rechten fragt"""
    return _("Windows fragt nach Administratorrechten …") if IS_WIN else \
        _("Das System fragt nach dem Administrator-Passwort …")


def irreversible_admin_text():
    return _("Das lässt sich nicht rückgängig machen. Windows fragt anschließend nach Administratorrechten.") if IS_WIN \
        else _("Das lässt sich nicht rückgängig machen. Anschließend fragt das System nach dem Administrator-Passwort.")


def data_kept_admin_text():
    return _("Die Daten bleiben erhalten. Windows fragt anschließend nach Administratorrechten.") if IS_WIN \
        else _("Die Daten bleiben erhalten. Anschließend fragt das System nach dem Administrator-Passwort.")


# ---------------------------------------------------------------------- Oberfläche

def setup_window(root):
    """Programmsymbol (auch für Dialoge) und Stil: Windows wie das System, Linux "clam" (ordentlicher als "default")"""
    if IS_WIN:
        icon = resource_path("app.ico")
        if os.path.isfile(icon):
            root.iconbitmap(default=icon)
    else:
        png = resource_path("app.png")
        if os.path.isfile(png):
            try:
                img = tk.PhotoImage(file=png)
                root.iconphoto(True, img)
                root._app_icon = img   # Verweis halten, sonst verschwindet das Bild
            except tk.TclError:
                pass
    style = ttk.Style(root)
    for theme in ("vista", "clam"):
        if theme in style.theme_names():
            style.theme_use(theme)
            break
    return style


def _inside(win, top, x, y):
    """Lage eines Dialogs, in den Bildschirm des Hauptfensters gerückt (auch bei Monitoren links/über dem Hauptmonitor,
    deren Koordinaten negativ sind)"""
    x0, y0, aw, ah = work_area(top)
    x = min(max(x, x0), x0 + max(0, aw - win.winfo_reqwidth()))
    y = min(max(y, y0), y0 + max(0, ah - win.winfo_reqheight()))
    return f"+{x}+{y}"


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
    cancel_btn = ttk.Button(btns, text=_("Abbrechen"), command=lambda: done(False))
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
    win.geometry(_inside(win, top, x, y))
    win.deiconify()
    cancel_btn.focus_set()
    win.grab_set()
    root.wait_window(win)
    return result["ok"]


def ask_string(root, title, prompt, initial=""):
    """Texteingabe mit Knöpfen in der gewählten Sprache (simpledialog zeigt immer englische). None = abgebrochen."""
    win = tk.Toplevel(root)
    win.withdraw()
    win.title(title)
    win.transient(root)
    win.resizable(False, False)
    frm = ttk.Frame(win, padding=(18, 14, 18, 14))
    frm.pack(fill="both", expand=True)
    ttk.Label(frm, text=prompt, wraplength=420, justify="left").pack(anchor="w")
    var = tk.StringVar(value=initial)
    entry = ttk.Entry(frm, textvariable=var, width=40)
    entry.pack(anchor="w", fill="x", pady=(10, 0))
    result = {"text": None}

    def done(ok):
        result["text"] = var.get() if ok else None
        win.destroy()

    btns = ttk.Frame(frm)
    btns.pack(anchor="e", pady=(14, 0))
    ttk.Button(btns, text=_("OK"), command=lambda: done(True)).pack(side="left")
    ttk.Button(btns, text=_("Abbrechen"), command=lambda: done(False)).pack(side="left", padx=(6, 0))
    win.bind("<Return>", lambda e: done(True))
    win.bind("<Escape>", lambda e: done(False))
    win.protocol("WM_DELETE_WINDOW", lambda: done(False))
    win.update_idletasks()
    top = root.winfo_toplevel()
    x = top.winfo_rootx() + (top.winfo_width() - win.winfo_reqwidth()) // 2
    y = top.winfo_rooty() + (top.winfo_height() - win.winfo_reqheight()) // 3
    win.geometry(_inside(win, top, x, y))
    win.deiconify()
    entry.focus_set()
    entry.select_range(0, "end")
    win.grab_set()
    root.wait_window(win)
    return result["text"]


def ask_choice(root, title, prompt, options, ok_text):
    """Auswahl aus einer Liste (Optionsfelder, das erste vorgewählt). Liefert den Index oder None (abgebrochen)."""
    win = tk.Toplevel(root)
    win.withdraw()
    win.title(title)
    win.transient(root)
    win.resizable(False, False)
    frm = ttk.Frame(win, padding=(18, 14, 18, 14))
    frm.pack(fill="both", expand=True)
    ttk.Label(frm, text=prompt, wraplength=520, justify="left").pack(anchor="w")
    var = tk.IntVar(value=0)
    for i, text in enumerate(options):
        ttk.Radiobutton(frm, text=text, value=i, variable=var).pack(anchor="w", pady=(8 if i == 0 else 3, 0))
    result = {"i": None}

    def done(ok):
        result["i"] = var.get() if ok else None
        win.destroy()

    btns = ttk.Frame(frm)
    btns.pack(anchor="e", pady=(16, 0))
    ok_btn = ttk.Button(btns, text=ok_text, command=lambda: done(True))
    ok_btn.pack(side="left")
    ttk.Button(btns, text=_("Abbrechen"), command=lambda: done(False)).pack(side="left", padx=(6, 0))
    win.bind("<Escape>", lambda e: done(False))
    win.protocol("WM_DELETE_WINDOW", lambda: done(False))
    win.update_idletasks()
    top = root.winfo_toplevel()
    x = top.winfo_rootx() + (top.winfo_width() - win.winfo_reqwidth()) // 2
    y = top.winfo_rooty() + (top.winfo_height() - win.winfo_reqheight()) // 3
    win.geometry(_inside(win, top, x, y))
    win.deiconify()
    ok_btn.focus_set()
    win.grab_set()
    root.wait_window(win)
    return result["i"]


def work_area(win):
    """Nutzbarer Bereich (ohne Taskleiste) des Bildschirms, auf dem das Fenster steht: (x, y, breite, höhe)"""
    if IS_WIN:
        try:
            from ctypes import wintypes

            class MonitorInfo(ctypes.Structure):
                _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                            ("dwFlags", wintypes.DWORD)]
            user32 = ctypes.windll.user32
            user32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
            user32.MonitorFromWindow.restype = wintypes.HANDLE
            user32.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
            user32.MonitorFromPoint.restype = wintypes.HANDLE
            user32.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MonitorInfo)]
            hwnd = hwnd_of(win) if win.winfo_ismapped() else None
            # MONITOR_DEFAULTTONEAREST (2) bzw. MONITOR_DEFAULTTOPRIMARY (1)
            mon = user32.MonitorFromWindow(hwnd, 2) if hwnd else user32.MonitorFromPoint(wintypes.POINT(0, 0), 1)
            mi = MonitorInfo()
            mi.cbSize = ctypes.sizeof(mi)
            if mon and user32.GetMonitorInfoW(mon, ctypes.byref(mi)):
                r = mi.rcWork
                return r.left, r.top, r.right - r.left, r.bottom - r.top
        except (AttributeError, OSError, ValueError):
            pass
        return 0, 0, win.winfo_screenwidth(), win.winfo_screenheight() - 48
    # Linux: der Monitor, auf dem das Fenster steht (bzw. beim ersten Anzeigen der mit dem Mauszeiger) - Tk kennt nur den
    # ganzen Bildschirm über alle Monitore. Leisten der Arbeitsumgebung oben und unten freihalten.
    x, y, w, h = 0, 0, win.winfo_screenwidth(), win.winfo_screenheight()
    mons = _x11_monitors()
    if mons:
        if win.winfo_ismapped():
            px, py = win.winfo_rootx() + win.winfo_width() // 2, win.winfo_rooty() + win.winfo_height() // 2
        else:
            px, py = win.winfo_pointerxy()
        hit = next((m for m in mons if m[0] <= px < m[0] + m[2] and m[1] <= py < m[1] + m[3]), None)
        x, y, w, h = (hit or next((m for m in mons if m[4]), mons[0]))[:4]
    return x, y + 32, w, h - 32 - 48


_monitors = {"when": 0.0, "list": []}


def _x11_monitors():
    """Monitore laut xrandr: Liste (x, y, breite, höhe, primär); leer, wenn xrandr fehlt. Kurz zwischengespeichert,
    das Zentrieren fragt bei jeder Größenänderung."""
    if time.time() - _monitors["when"] < 5:
        return _monitors["list"]
    mons = []
    try:
        r = subprocess.run(["xrandr", "--listactivemonitors"], capture_output=True, text=True, timeout=2)
        # " 0: +*DP-1 2560/597x1440/336+0+0  DP-1"
        for m in re.finditer(r"^\s*\d+:\s*\+?(\*?)\S*\s+(\d+)/\d+x(\d+)/\d+\+(-?\d+)\+(-?\d+)", r.stdout, re.M):
            mons.append((int(m.group(4)), int(m.group(5)), int(m.group(2)), int(m.group(3)), bool(m.group(1))))
    except (OSError, subprocess.SubprocessError):
        pass
    _monitors.update(when=time.time(), list=mons)
    return mons


def center_window(win):
    """Fenster mittig in den nutzbaren Bildschirmbereich setzen, samt Titelleiste und Rahmen ganz sichtbar. Ist es
    höher als der Platz, wird es oben angelegt (die Titelleiste bleibt erreichbar)."""
    win.update_idletasks()
    x0, y0, aw, ah = work_area(win)
    w, h = win.winfo_reqwidth(), win.winfo_reqheight()
    if win.winfo_ismapped():   # Rahmen und Titelleiste, wie das System sie wirklich zeichnet
        side = max(0, win.winfo_rootx() - win.winfo_x())
        top = max(0, win.winfo_rooty() - win.winfo_y())
    else:                      # noch nicht angezeigt: übliche Maße, skaliert
        scale = win.winfo_fpixels("1i") / 96
        side, top = round(8 * scale), round(31 * scale)
    fw, fh = w + 2 * side, h + top + side
    win.geometry(f"+{x0 + max(0, (aw - fw) // 2)}+{y0 + max(0, (ah - fh) // 2)}")


def show_centered(win):
    """Beim Start mittig anzeigen und mit dem echten Rahmen gleich noch einmal ausrichten. Danach jedes Mal wieder
    mittig, wenn sich die Größe ändert (Moduswechsel, Expertenmodus, Sprache, nachgeladene Listen) - nur verschieben
    (z. B. mit der Maus) lässt das Fenster, wo es ist."""
    center_window(win)
    win.deiconify()
    win.after(80, lambda: center_window(win))
    size = {"wh": None, "job": None}

    def changed(e):
        if e.widget is not win:
            return
        wh = (e.width, e.height)
        if size["wh"] is not None and wh != size["wh"]:
            if size["job"]:
                win.after_cancel(size["job"])
            size["job"] = win.after(60, lambda: (size.update(job=None), center_window(win)))
        size["wh"] = wh
    win.bind("<Configure>", changed, add="+")


keep_on_screen = center_window   # nach jeder Größenänderung: wieder mittig


def lang_selector(parent, current, on_change):
    """Auswahl der Sprache (oben rechts); on_change(code) nach einer Änderung"""
    from i18n import LANGS
    codes = [c for c, _n in LANGS]
    box = ttk.Combobox(parent, values=[n for _c, n in LANGS], state="readonly", width=10)
    box.set(dict(LANGS).get(current, "English"))
    box.bind("<<ComboboxSelected>>", lambda e: on_change(codes[box.current()]))
    return box


def help_source():
    """Anleitung (help.html): in der exe mitgeliefert, in der Entwicklung aus der Weboberfläche des Payloads"""
    path = resource_path("help.html")
    if os.path.isfile(path):
        return path
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "usb_format", "web", "help.html")


_help_dir = None


def open_help(lang_code, parent=None):
    """Anleitung im Standardbrowser öffnen, in der Sprache des Programms. Dafür eine Kopie mit dieser Sprache als
    Vorgabe im Temp-Ordner - an Dateien reicht Windows ?lang=… bzw. #… nicht zuverlässig an den Browser weiter."""
    try:
        with open(help_source(), encoding="utf-8") as f:
            page = f.read()
        page = page.replace('data-default-lang=""', f'data-default-lang="{lang_code}"', 1)
        global _help_dir
        if not _help_dir or not os.path.isdir(_help_dir):   # eigener Ordner: in /tmp könnte ein anderer Benutzer die
            _help_dir = tempfile.mkdtemp(prefix=TEMP_PREFIX + "-Hilfe-")   # Datei sonst vorher anlegen/umleiten
        path = os.path.join(_help_dir, f"USB-Formatierer-Anleitung-{lang_code}.html")
        with open(path, "w", encoding="utf-8") as f:
            f.write(page)
        if IS_WIN:
            os.startfile(path)
        else:   # Linux: Standardbrowser (xdg-open)
            import pathlib
            import webbrowser
            if not webbrowser.open(pathlib.Path(path).as_uri()):
                raise OSError(_("kein Browser gefunden"))
    except OSError as e:
        messagebox.showerror(_("Hilfe"), _("Die Anleitung ließ sich nicht öffnen: {e}", e=e), parent=parent)


AUTHOR = "strongt1me"
AUTHOR_URL = "https://github.com/strongt1me"


def app_version():
    """Version der Programme: Datei VERSION (im fertigen Programm mitgeliefert, in der Entwicklung im Projektordner)"""
    for path in (resource_path("VERSION"),
                 os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "VERSION")):
        try:
            with open(path, encoding="ascii") as f:
                return f.read().strip()
        except (OSError, ValueError):
            pass
    return ""


APP_VERSION = app_version()


def credit(parent, ps5=True):
    """Profilbild und Name des Entwicklers, als Link zum GitHub-Profil; der Community-Spruch nur bei den PS5-Programmen"""
    frm = ttk.Frame(parent)
    scale = parent.winfo_fpixels("1i") / 96
    path = resource_path(f"avatar-{44 if scale >= 1.5 else 22}.png")
    if not os.path.isfile(path):   # Entwicklung: neben dem Programm
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.path.basename(path))
    try:
        img = tk.PhotoImage(file=path)
        pic = ttk.Label(frm, image=img, cursor="hand2")
        pic.image = img   # Verweis halten, sonst verschwindet das Bild
        pic.pack(side="left", padx=(0, 6))
    except tk.TclError:
        pic = None
    msg = (_("Entwickelt von {name} · für die PS5-Community", name=AUTHOR) if ps5
           else _("Entwickelt von {name}", name=AUTHOR))
    text = ttk.Label(frm, text=msg, foreground="#6b7280", cursor="hand2")
    text.pack(side="left")

    def visit(_e=None):
        import webbrowser
        webbrowser.open(AUTHOR_URL)
    for w in (pic, text):
        if w is not None:
            w.bind("<Button-1>", visit)
    return frm


def footer(parent, ps5=True):
    """Fußzeile unten: Trennlinie und mittig der Entwickler, daneben die Version"""
    frm = ttk.Frame(parent)
    ttk.Separator(frm).pack(fill="x", pady=(12, 8))
    row = ttk.Frame(frm)
    row.pack()
    credit(row, ps5).pack(side="left")
    if APP_VERSION:
        ttk.Label(row, text=f"·  v{APP_VERSION}", foreground="#6b7280").pack(side="left", padx=(8, 0))
    return frm


def top_bar(parent, current, on_lang, on_help):
    """Leiste oben rechts: Hilfe und Sprache"""
    bar = ttk.Frame(parent)
    top = ttk.Frame(bar)
    top.pack(side="right")
    ttk.Button(top, text="? " + _("Hilfe"), command=on_help, width=0).pack(side="left", padx=(0, 10))
    if IS_WIN:   # Tk unter Linux kann keine Farb-Emojis (zeigte ein Kästchen)
        ttk.Label(top, text="🌐").pack(side="left", padx=(0, 4))
    box = lang_selector(top, current, on_lang)
    box.pack(side="left")
    return bar, box


def hwnd_of(widget):
    """Fenster-Handle (im UI-Thread holen): Elternfenster der UAC-Abfrage"""
    try:
        return int(widget.winfo_toplevel().wm_frame(), 16)
    except (tk.TclError, ValueError):
        return None
