"""Gemeinsamer Teil für USB-Datenträger am PC: den Helfer (Windows: usbfmt_win.exe, Linux: usbfmt_linux) finden,
prüfen und mit Administratorrechten starten (Windows: UAC, Linux: pkexec - siehe pc_linux), Datenträger auflisten,
Einstellungen, Rückfrage-Dialog, Hilfe. Genutzt vom PS5-USB-Formatierer und vom eigenständigen USB-Formatierer."""
import ctypes
import hashlib
import json
import os
import platform
import re
import secrets
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import urllib.request
from tkinter import ttk

import ui_msgbox as messagebox
import ui_theme
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
_SETTING_TYPES = {"lang": str, "theme": str, "mode": str, "host": str, "label": str, "fs": str, "scheme": str,
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


_settings_base = {}   # je Programm: Stand der Datei beim Laden bzw. letzten Speichern (für das Zusammenführen)


def _read_settings(app_name, strict=False):
    """Einstellungen aus der Datei; fehlt sie, {}. Ist sie unlesbar oder kaputt: {} bzw. mit strict None - beim
    Zusammenführen hieße {} sonst "das andere Fenster hat alles gelöscht"."""
    try:
        with open(settings_file(app_name), encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError):
        return None if strict else {}
    return _clean_settings(data) if isinstance(data, dict) else (None if strict else {})


def load_settings(app_name):
    data = _read_settings(app_name)
    _settings_base[app_name] = json.loads(json.dumps(data))
    return data


_MISSING = object()


def _merge3(base, mine, disk):
    """Dreiseitig zusammenführen: was hier (mine) gegenüber base geändert oder gelöscht wurde, gewinnt; sonst gilt die Datei (disk,
    womöglich von einem anderen Fenster geändert). Wörterbücher werden Schlüssel für Schlüssel zusammengeführt."""
    out = {}
    for k in set(base) | set(mine) | set(disk):
        b, m, d = base.get(k, _MISSING), mine.get(k, _MISSING), disk.get(k, _MISSING)
        if m is _MISSING:
            if b is _MISSING and d is not _MISSING:
                out[k] = d                     # vom anderen Fenster neu
            continue                           # hier gelöscht (oder nirgends)
        if b is not _MISSING and m == b:       # hier unverändert: was die Datei sagt
            if d is not _MISSING:
                out[k] = d
            continue
        if isinstance(m, dict) and isinstance(d, dict):
            out[k] = _merge3(b if isinstance(b, dict) else {}, m, d)
        else:
            out[k] = m                         # hier geändert
    return out


def _sync_into(target, src):
    """target an src angleichen, verschachtelte Wörterbücher dabei als dieselben Objekte behalten (andere Teile des
    Programms halten Verweise darauf)"""
    for k in list(target):
        if k not in src:
            del target[k]
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(target.get(k), dict):
            _sync_into(target[k], v)
        else:
            target[k] = v


def save_settings(app_name, data):
    """Atomar (erst neue Datei, dann ersetzen): Ein Absturz mitten im Schreiben hinterlässt keine halbe Datei.
    Laufen zwei Fenster desselben Programms, überschreibt keines die Änderungen des anderen: übernommen wird nur, was
    sich hier seit dem Laden geändert hat; was das andere inzwischen gespeichert hat, kommt dafür auch in data an."""
    base, disk = _settings_base.get(app_name), _read_settings(app_name, strict=True)
    if base is not None and disk is not None:
        _sync_into(data, _merge3(base, data, disk))
    path = settings_file(app_name)
    tmp = None
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # eigener Name je Speichern: zwei Programmfenster schreiben nicht in dieselbe Zwischendatei
        fd, tmp = tempfile.mkstemp(prefix="settings-", suffix=".tmp", dir=os.path.dirname(path))
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f)
            f.flush()
            os.fsync(f.fileno())   # erst auf der Platte, dann ersetzen - auch bei Stromausfall keine leere Datei
        for attempt in range(5):   # kurz gesperrt (Virenscanner, OneDrive): nochmal versuchen
            try:
                os.replace(tmp, path)
                tmp = None
                # erst jetzt gilt das als Stand der Datei - scheiterte das Schreiben, bleibt die Änderung hier
                # gegenüber dem alten Stand "geändert" und geht beim nächsten Speichern nicht verloren
                _settings_base[app_name] = json.loads(json.dumps(data))
                break
            except PermissionError:
                if attempt == 4:
                    raise
                time.sleep(0.1)
    except OSError:
        pass
    finally:
        if tmp:
            try:
                os.remove(tmp)
            except OSError:
                pass


def error_log_file(app_name):
    """fehler.log neben den Einstellungen"""
    return os.path.join(os.path.dirname(settings_file(app_name)), "fehler.log")


def log_error(app_name, exc_info, note=""):
    """Unerwarteten Fehler (Zeit, Version, Ablauf) an fehler.log anhängen; die Datei bleibt klein. Darf nie selbst scheitern."""
    try:
        import traceback
        path = error_log_file(app_name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        text = "".join(traceback.format_exception(*exc_info)) if exc_info else ""
        with open(path, "a", encoding="utf-8") as f:
            f.write("--- %s %s%s\n%s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), APP_VERSION, (" " + note) if note else "", text))
        if os.path.getsize(path) > 200000:   # nur das Neueste behalten
            with open(path, "rb") as f:
                f.seek(-100000, os.SEEK_END)
                tail = f.read()
            with open(path, "wb") as f:
                f.write(tail[tail.find(b"\n--- ") + 1:] if b"\n--- " in tail else tail)
    except Exception:
        pass


def error_report(app_name, extra=""):
    """Text zum Weitergeben: Version, System, Sprache, Zusatzangaben und die letzten Fehler aus fehler.log (keine Zugangsdaten)"""
    lines = ["%s %s" % (app_name, APP_VERSION), "System: %s · Python %s" % (platform.platform(), platform.python_version()),
             "Sprache: %s" % lang()]
    if extra:
        lines.append(extra)
    try:
        with open(error_log_file(app_name), "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - 6000))
            lines += ["", "Letzte Fehler:", f.read().decode("utf-8", "replace")]
    except OSError:
        lines.append("(keine Fehler protokolliert)")
    return "\n".join(lines)


def run_selftest(app_name, factory, extra=(), tabs=(), out=None):
    """Selbsttest des gebauten Programms (--selftest [Datei]): Version, Übersetzungstabelle (fehlt sie im Paket, fällt die Oberfläche
    still auf Deutsch zurück), Helfer samt Prüfsumme, zusätzliche Prüfungen (extra: Aufrufe, die (ok, Text) liefern) und der Aufbau
    der Oberfläche samt aller Reiter. Ergebnis in die Datei out (das Fenster-Programm hat keine Konsole) und als Rückgabe 0/1."""
    lines, problems = [], []

    def note(ok, text):
        lines.append(("OK      " if ok else "FEHLER  ") + text)
        if not ok:
            problems.append(text)

    note(bool(re.fullmatch(r"\d+\.\d+\.\d+", APP_VERSION or "")), "Version %s" % (APP_VERSION or "(leer)"))
    try:
        import i18n
        cat = i18n._catalog()
        note(len(cat) >= 400, "Übersetzungstabelle geladen (%d Texte)" % len(cat))
        for code in ("en", "it", "es", "fr"):
            t = (cat.get("Abbrechen") or {}).get(code)
            note(bool(t) and t != "Abbrechen", "Übersetzung %s vorhanden: Abbrechen = %s" % (code, t))
    except Exception as e:
        note(False, "Übersetzungstabelle: %r" % e)
    if IS_WIN:
        h = find_helper()
        note(bool(h), "Helfer usbfmt_win.exe gefunden")
        if h and getattr(sys, "frozen", False):
            note(bool(HELPER_SHA256), "Helfer-Prüfsumme eingebaut")
            if HELPER_SHA256:
                with open(h, "rb") as f:
                    note(hashlib.sha256(f.read()).hexdigest() == HELPER_SHA256, "Helfer stimmt mit der eingebauten Prüfsumme überein")
    for fn in extra:
        try:
            note(*fn())
        except Exception as e:
            note(False, "Zusatzprüfung: %r" % e)
    # Oberfläche: eigene Einstellungen, keine Suche im Netz, kein Update-Hinweis
    tmp = tempfile.mkdtemp(prefix="selftest-")
    os.environ["APPDATA"] = os.environ["XDG_CONFIG_HOME"] = tmp
    os.environ["USBFMT_NO_UPDATE_CHECK"] = os.environ["USBFMT_NO_DISCOVERY"] = "1"
    app = None
    try:
        app = factory()
        app.update()
        count = 0
        stack = [app]
        while stack:
            w = stack.pop()
            count += 1
            stack.extend(w.winfo_children())
        note(count > 40, "Oberfläche aufgebaut (%d Bedienelemente)" % count)
        for name in tabs:
            app._set_tab(name, False)
            app.update()
        if tabs:
            note(True, "alle %d Reiter lassen sich öffnen" % len(tabs))
    except Exception as e:
        note(False, "Oberfläche startet nicht: %r" % e)
    finally:
        try:
            if app is not None:
                app.destroy()
        except Exception:
            pass
    text = "\n".join(lines + ["", "Selbsttest %s" % ("FEHLGESCHLAGEN: %d Problem(e)" % len(problems) if problems else "bestanden")]) + "\n"
    try:
        if out:
            with open(out, "w", encoding="utf-8") as f:
                f.write(text)
        elif sys.stdout:
            sys.stdout.write(text)
    except OSError:
        pass
    return 1 if problems else 0


def selftest_arg():
    """Wert nach --selftest (Ausgabedatei) oder None, wenn das Programm nicht im Selbsttest läuft: (läuft, Datei)"""
    if "--selftest" not in sys.argv:
        return False, None
    i = sys.argv.index("--selftest")
    nxt = sys.argv[i + 1] if i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith("--") else None
    return True, nxt


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
    if getattr(sys, "frozen", False) and not HELPER_SHA256:   # ausgeliefert, aber ohne Prüfsumme: Helfer nicht ungeprüft starten
        k32.CloseHandle(ctypes.c_void_p(h))
        raise OSError(_("Der Helfer usbfmt_win.exe wurde verändert – aus Sicherheitsgründen nicht gestartet. "
                        "Bitte das Programm neu herunterladen."))
    if HELPER_SHA256 and digest != HELPER_SHA256:
        k32.CloseHandle(ctypes.c_void_p(h))
        raise OSError(_("Der Helfer usbfmt_win.exe wurde verändert – aus Sicherheitsgründen nicht gestartet. "
                        "Bitte das Programm neu herunterladen."))
    return h


def pc_scan_text(pending, elapsed):
    """Statuszeile beim Einlesen: welche Datenträger noch gelesen werden - nach ein paar Sekunden mit dem Grund"""
    text = _("Lese Datenträger: {names} …", names=", ".join(pending)) if pending else _("Suche USB-Datenträger am PC …")
    if elapsed >= 3:
        text += " " + _("Schlafende Festplatten laufen dabei erst an – das kann einige Sekunden dauern.")
    return text


def pc_list(progress=None):
    """USB-Datenträger am PC (ohne Administratorrechte). Die Systemplatte meldet der Helfer nie.
    progress(prozent, text) läuft im Lesethread: Der Helfer meldet auf stderr, welche Datenträger er noch liest -
    schlafende Festplatten laufen dabei erst an (Sekunden je Platte)."""
    exe = find_helper()
    if not exe:
        raise RuntimeError(_("Der Helfer {name} fehlt neben dem Programm.", name=HELPER_NAME))
    proc = subprocess.Popen([exe, "list"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    got, errors, started = {}, [], time.time()
    last = {"pct": 0, "pending": [], "at": started, "top": 0}
    show_lock = threading.Lock()

    def show(pct, pending):
        with show_lock:   # Lesethread und Wartschleife melden beide: Höchstwert und Meldung gehören zusammen
            last["top"] = pct = max(pct, last["top"])   # der Balken geht nie zurück
            if progress:
                progress(pct, pc_scan_text(pending, time.time() - started))

    def report(pct, pending):
        last.update(pct=pct, pending=pending, at=time.time())
        show(pct, pending)

    def read_out():
        got["out"] = proc.stdout.read()

    def read_err():
        for raw in proc.stderr:
            parts = raw.decode("utf-8", "replace").rstrip("\r\n").split("\t")
            if parts[0] == "P" and len(parts) > 1 and parts[1].isdigit():
                report(min(int(parts[1]), 100), [p for p in parts[2:] if p])
            elif any(parts):
                errors.append("\t".join(parts))

    readers = [threading.Thread(target=f, daemon=True) for f in (read_out, read_err)]
    for t in readers:
        t.start()
    try:
        # Eine schlafende Platte hält den Helfer Sekunden an einer Stelle auf: dann rückt der Balken langsam bis kurz
        # vor die nächste Meldung vor (nie darüber hinaus) und die Zeile nennt den Grund - er steht nie scheinbar still
        while proc.poll() is None:
            if time.time() - started > 90:   # mehrere schlafende Platten laufen gleichzeitig an - selten länger als 30 s
                raise subprocess.TimeoutExpired(exe, 90)
            time.sleep(0.25)
            idle = time.time() - last["at"]
            if progress and idle >= 0.5 and last["pct"] < 99:
                # bis kurz vor die nächste Meldung: beim Abfragen der Datenträger (bis 30 %) langsam bis 45 (der Balken geht nie zurück), danach bis kurz vor den
                # Anteil, den der nächste fertige Datenträger bringt
                np = max(1, len(last["pending"]))
                ceil = 45 if last["pct"] < 30 and not last["pending"] else last["pct"] + (100 - last["pct"]) * 0.8 / np
                shown = last["pct"] + (ceil - last["pct"]) * (1 - 2.718281828 ** (-idle / 8))
                show(int(shown), last["pending"])
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        raise
    for t in readers:
        t.join(5)
    if proc.returncode != 0:
        raise RuntimeError(_("Helfer meldet Fehler {code}: {text}", code=proc.returncode,
                             text="\n".join(errors).strip() or _("keine Angabe")))
    devices = json.loads(got.get("out", b"").decode("utf-8", "replace") or "[]")
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


def _helper_lines(data, on_progress):
    """Fertige Zeilen der Helfer-Ausgabe auswerten: P = Fortschritt (an on_progress), R = Ergebnis.
    Liefert (ok, meldung, json) oder None, solange kein Ergebnis dabei ist."""
    result = None
    # nur an \n trennen - splitlines() trennte auch an U+2028 & Co. mitten in einer Meldung
    for line in data.decode("utf-8", "replace").replace("\r\n", "\n").rstrip("\n").split("\n"):
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
    return result


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
            except OSError:
                ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(lock))
                raise
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
        self.cancel_requested = True   # für den Verlauf: "abgebrochen" statt "fehlgeschlagen"
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
                    result = _helper_lines(data[:end], on_progress)
                if result is None and not alive:
                    code = ctypes.c_ulong(0)
                    ctypes.windll.kernel32.GetExitCodeProcess(ctypes.c_void_p(self.proc), ctypes.byref(code))
                    result = (False, _("Der Helfer wurde unerwartet beendet (Code {code}).", code=code.value), {})
                elif result is None and time.time() - last_change > self.STALL_S:
                    self.cancel()
                    self.cancel_requested = False   # hängt: "fehlgeschlagen", nicht "abgebrochen"
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


class PlainJob:
    """Helfer OHNE Administratorrechte - für Aufträge ohne Datenträger (Image-Datei prüfen): keine UAC- bzw.
    pkexec-Abfrage. Gleiche Schnittstelle wie PcJob (cancel, follow, cancellable)."""
    STALL_S = 30 * 60

    def __init__(self, args, hwnd=None):
        exe = find_helper()
        if not exe:
            raise RuntimeError(_("Der Helfer {name} fehlt neben dem Programm.", name=HELPER_NAME))
        # läuft mit den eigenen Rechten: ein privater Ordner genügt (unter Linux verlangt der Helfer genau das)
        self.tmpdir = tempfile.mkdtemp(prefix=TEMP_PREFIX + "-")
        self.out = os.path.join(self.tmpdir, "auftrag.txt")
        self.cancel_file = os.path.join(self.tmpdir, "abbruch")
        self.cancellable = False
        try:
            self.proc = subprocess.Popen(
                [exe, "--out", self.out, "--cancel", self.cancel_file, "--lang", lang()] + list(args),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=0x08000000 if IS_WIN else 0)   # CREATE_NO_WINDOW: kein Konsolenfenster
        except OSError:
            self._cleanup()
            raise

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
        self.cancel_requested = True   # für den Verlauf: "abgebrochen" statt "fehlgeschlagen"
        try:
            open(self.cancel_file, "w").close()
        except OSError:
            pass

    def follow(self, on_progress):
        """Bis zum Ende mitlesen; liefert (ok, meldung, json)"""
        pos, result, last_change = 0, None, time.time()
        try:
            while result is None:
                alive = self.proc.poll() is None
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
                    result = _helper_lines(data[:end], on_progress)
                if result is None and not alive:
                    result = (False, _("Der Helfer wurde unerwartet beendet (Code {code}).", code=self.proc.returncode), {})
                elif result is None and time.time() - last_change > self.STALL_S:
                    self.cancel()
                    self.cancel_requested = False   # hängt: "fehlgeschlagen", nicht "abgebrochen"
                    self.proc.kill()
                    result = (False, _("Der Helfer meldet seit 30 Minuten nichts mehr – abgebrochen."), {})
                elif result is None:
                    time.sleep(0.3)
        finally:
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self._cleanup()
        return result


def check_images(widget):
    """Kästchen (leer, angehakt) für Listen - als Bild im Theme (hell/dunkel), mit der Bildschirmskalierung
    mitwachsend. Die Bilder bleiben im Theme gespeichert."""
    return ui_theme.list_checks(widget)


def grab(win):
    """Dialog modal machen. Erst sichtbar werden lassen: unter Linux scheitert grab_set sonst ("window not viewable"),
    wenn der Dialog per Doppelklick aufgeht, solange die Maustaste noch gedrückt ist"""
    try:
        win.wait_visibility()
        win.grab_set()
    except tk.TclError:   # Fenster schon wieder zu oder Grab verweigert: dann eben nicht modal
        pass


def show_scan(pbar, status_var, pct, text):
    """Fortschritt beim Suchen und Einlesen (Datenträger, PS5, Spiele): Balken mit Prozent statt Stillstand"""
    if str(pbar["mode"]) != "determinate":
        pbar.stop()
        pbar.configure(mode="determinate")
    pbar["value"] = pct
    status_var.set(f"{text} {pct} %")


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


# ---------------------------------------------------------------------- Auftragsverlauf
# Die letzten HISTORY_MAX Aufträge am PC mit Ergebnis, neueste zuerst, als JSON-Zeilen neben den Einstellungen
# (history.jsonl; dieselben Felder wie /api/history des Formatierers auf der PS5). Geschrieben wird erst nach
# enable_history (beim Programmstart) - Tests, die die Fenster ohne Start bauen, hinterlassen nichts.
HISTORY_MAX = 100
_history = {"path": None}
_history_lock = threading.Lock()
FS_NAMES = {"exfat": "exFAT", "fat32": "FAT32", "fat16": "FAT16", "ntfs": "NTFS", "ext2": "ext2", "ext3": "ext3",
            "ext4": "ext4", "hfsplus": "HFS+"}


def enable_history(app_name):
    _history["path"] = os.path.join(os.path.dirname(settings_file(app_name)), "history.jsonl")


def history_load():
    """Einträge, neueste zuerst; kaputte Zeilen fallen weg"""
    out = []
    try:
        with open(_history["path"] or "", encoding="utf-8") as f:
            for line in f:
                try:
                    e = json.loads(line)
                except ValueError:
                    continue
                if isinstance(e, dict) and isinstance(e.get("kind"), str):
                    out.append(e)
                if len(out) >= HISTORY_MAX:
                    break
    except OSError:
        pass
    return out


def history_clear():
    with _history_lock:
        try:
            os.remove(_history["path"] or "")
        except OSError:
            pass


SLOW_FACTOR = 0.7


def same_drive(a, b):
    """Verlaufseinträge desselben Datenträgers: Seriennummer, sonst Modell und Größe"""
    if a.get("ident") and b.get("ident"):
        return a["ident"] == b["ident"]
    return (a.get("model") or "") == (b.get("model") or "") and int(a.get("size") or 0) == int(b.get("size") or 0)


def earlier_mbps(history, e):
    """Median der letzten bis zu 5 früheren Lesetempo-Messungen desselben Datenträgers; 0 = keine"""
    v = sorted([x["mbps"] for x in history if (x.get("mbps") or 0) > 0 and same_drive(x, e)][:5])
    return 0 if not v else v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2


def slower_note(history, e):
    """Verschleiß erkennen (SMART liefern USB-Sticks nicht): deutlich langsamer als früher (30 % und 5 MB/s weniger)"""
    before = earlier_mbps(history, e)
    if before and e.get("mbps") and e["mbps"] < before * SLOW_FACTOR and before - e["mbps"] >= 5:
        return " " + _("Deutlich langsamer als bei früheren Messungen ({n} MB/s) – der Datenträger ist womöglich "
                       "verschlissen.", n=round(before))
    return ""


def record_job(args, ok, msg, dev=None, started=None, cancelled=False, info=None):
    """Auftrag des Helfers (args wie an den Helfer) in den Verlauf schreiben - still, wenn es nicht geht. Liefert einen
    Hinweis zum Anhängen an die Meldung (Datenträger liest deutlich langsamer als früher) oder """""
    path = _history["path"]
    if not path or not args:
        return ""
    kind = str(args[0])
    e = {"t": int(time.time()), "kind": kind, "ok": bool(ok), "cancelled": bool(cancelled and not ok),
         "secs": max(0, int(time.time() - started)) if started else 0}
    if dev:
        e.update(device=disk_name(dev), model=dev.get("model") or "", size=int(dev.get("size") or 0),
                 ident=dev.get("serial") or "")
        labels = [v.get("label") for v in dev.get("volumes") or [] if v.get("label")]
        if labels:
            e["label"] = labels[0]
    if kind == "format" and len(args) > 4:
        e["fs"] = FS_NAMES.get(args[3], str(args[3]))
        e["label"] = args[4]
    elif kind == "label" and len(args) > 3:
        e["label"] = args[3]
    elif kind in ("backup", "restore") and len(args) > 3:
        e["file"] = os.path.basename(args[3])
    elif kind == "verify" and len(args) > 1:
        e["file"] = os.path.basename(args[1])
    read = (info or {}).get("read") if ok and isinstance(info, dict) else 0
    if isinstance(read, (int, float)) and read > 0:
        e["mbps"] = round(read)   # Lesetempo: für die Ziel-Auswahl und den Vergleich mit späteren Messungen
    e = {k: v for k, v in e.items() if v != ""}
    with _history_lock:
        old = history_load()
        note = slower_note(old, e) if dev else ""
        e["msg"] = (str(msg or "") + note)[:600]
        entries = [e] + old[:HISTORY_MAX - 1]
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            fd, tmp = tempfile.mkstemp(prefix="history-", suffix=".tmp", dir=os.path.dirname(path))
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
                for x in entries:
                    f.write(json.dumps(x, ensure_ascii=False) + "\n")
            for attempt in range(5):   # kurz gesperrt (Virenscanner, gleichzeitiges Lesen): nochmal versuchen
                try:
                    os.replace(tmp, path)
                    break
                except PermissionError:
                    if attempt == 4:
                        raise
                    time.sleep(0.1)
        except OSError:
            try:
                os.remove(tmp)
            except (OSError, UnboundLocalError):
                pass
    return note


def history_kind(kind):
    return {"format": _("Formatieren"), "layout": _("Partitionen anlegen"), "speed": _("Tempo messen"),
            "check": _("Prüfen & reparieren"), "label": _("Umbenennen"), "delete": _("Spiele löschen"),
            "backup": _("Image sichern"), "restore": _("Image wiederherstellen"), "tile": _("Kachel neu anmelden"),
            "verify": _("Image prüfen"), "save_restore": _("Spielstand wiederherstellen"), "save_backup": _("Spielstände sichern"),
            "move": _("Spiele kopieren"), "pkg_delete": _("Pakete löschen"), "game_verify": _("Spiele prüfen"),
            "game_checksum": _("Prüfsumme erstellen"), "partial_discard": _("Teilkopie verwerfen"), "orphan_delete": _("Aufräumen"),
            "game_compare": _("Inhalt vergleichen"), "eject": _("Auswerfen"),
            "game_toggle": _("Aktivieren / Deaktivieren")}.get(kind, kind)


def history_csv(sources):
    """Verlauf als CSV-Text (Semikolon wie in DE/IT/ES/FR üblich, UTF-8 mit BOM, damit Tabellenprogramme Umlaute richtig lesen):
    Zeit, Ort, Art, Ergebnis, Laufwerk, Modell, Dauer, Lesetempo, Meldung. sources wie bei history_dialog."""
    import csv
    import io
    buf = io.StringIO()
    w = csv.writer(buf, delimiter=";", lineterminator="\r\n")
    w.writerow(["time", "where", "kind", "result", "device", "model", "seconds", "read_mbps", "message"])
    merged = sorted(((o, e) for o, entries in sources for e in entries), key=lambda x: -int(x[1].get("t") or 0))
    for o, e in merged:
        w.writerow([time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(e.get("t") or 0))), o or "", e.get("kind", ""),
                    "ok" if e.get("ok") else "cancelled" if e.get("cancelled") else "failed", e.get("device") or "",
                    e.get("model") or "", int(e.get("secs") or 0), e.get("mbps") or "", e.get("msg") or ""])
    return "\ufeff" + buf.getvalue()


DRIVE_KINDS = ("format", "layout", "check", "label", "speed", "backup", "restore", "delete")


def last_job(entries, model, size, ident=""):
    """letzter Auftrag an diesem Laufwerk (entries neueste zuerst): über die Seriennummer, ohne sie über Modell und
    Größe - Namen wie da2 oder die Datenträgernummer wechseln mit der Steckreihenfolge"""
    for e in entries:
        if e.get("kind") not in DRIVE_KINDS:
            continue
        if e.get("ident") and ident:
            same = e["ident"] == ident
        else:
            same = (e.get("model") or "") == (model or "") and int(e.get("size") or 0) == int(size or 0)
        if same:
            return e
    return None


def last_job_text(e):
    """"Zuletzt: Formatieren (exFAT), 20.10.2026" für die Laufwerkskarte"""
    what = history_kind(e.get("kind", "")) + (f" ({e['fs']})" if e.get("fs") else "")
    if not e.get("ok"):
        what += " – " + (_("abgebrochen") if e.get("cancelled") else _("fehlgeschlagen"))
    fmt = {"de": "%d.%m.%Y", "en": "%Y-%m-%d"}.get(lang(), "%d/%m/%Y")
    return _("Zuletzt: {what}, {date}", what=what, date=time.strftime(fmt, time.localtime(int(e.get("t") or 0))))


def system_report(system):
    """Zeile für den Fehlerbericht aus /api/system des Formatierers (Firmware, ShadowMountPlus, Kernel-Erweiterungen)"""
    if not isinstance(system, dict) or not system:
        return "PS5: -"
    smp = system.get("smp")
    caps = smp.get("caps") or [] if isinstance(smp, dict) else []
    smp_text = ("%s (%d capabilities%s)" % (smp.get("version") or "?", len(caps), ", eject" if "storage_eject" in caps else "")
                if isinstance(smp, dict) else "not reachable")
    return "PS5: Firmware %s · ShadowMountPlus %s · Kernel: %s" % (system.get("fw") or "?", smp_text,
                                                                  system.get("kext") or "not detected")


def speed_trends(entries):
    """Lesetempo je Datenträger (Seriennummer, sonst Modell und Größe) aus Verlaufseinträgen (neueste zuerst), älteste
    Messung zuerst; nur Datenträger mit mindestens zwei Messungen. drop: die letzte liegt deutlich unter dem Mittel der
    früheren (wie der Verschleiß-Hinweis)"""
    groups = {}
    for e in reversed(list(entries)):
        if not isinstance(e.get("mbps"), (int, float)) or e["mbps"] <= 0:
            continue
        k = ("i", e["ident"]) if e.get("ident") else ("m", e.get("model") or "", int(e.get("size") or 0))
        g = groups.setdefault(k, {"name": e.get("model") or e.get("device") or "?", "size": int(e.get("size") or 0),
                                  "vals": []})
        g["vals"].append(e["mbps"])
    out = []
    for g in groups.values():
        if len(g["vals"]) < 2:
            continue
        g["last"] = g["vals"][-1]
        before = earlier_mbps([{"mbps": v} for v in reversed(g["vals"][:-1])], {})
        g["drop"] = bool(before) and g["last"] < before * SLOW_FACTOR and before - g["last"] >= 5
        out.append(g)
    return out


def history_dialog(root, sources, on_clear):
    """Fenster "Verlauf": sources = [(ort, einträge)] (ort "PC"/"PS5", bei nur einer Quelle None). Neueste zuerst,
    unten die Meldung des gewählten Eintrags. on_clear() leert und liefert die neuen sources."""
    c = ui_theme.colors()
    win = tk.Toplevel(root)
    win.withdraw()
    win.title(_("Verlauf"))
    win.transient(root)
    win.configure(bg=c["card"])
    frm = ttk.Frame(win, padding=ui_theme.pad((22, 18, 22, 16)))
    frm.pack(fill="both", expand=True)
    frm.columnconfigure(0, weight=1)
    frm.rowconfigure(3, weight=1)
    ttk.Label(frm, text=_("Verlauf"), style="H2.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
    sub = ttk.Label(frm, style="Muted.TLabel")
    sub.grid(row=1, column=0, columnspan=2, sticky="w", pady=(ui_theme.px(4), ui_theme.px(8)))
    trends = ttk.Frame(frm)   # Lesetempo je Datenträger (nur mit mindestens zwei Messungen)
    trends.grid(row=2, column=0, columnspan=2, sticky="ew")
    where = any(o for o, _e in sources)
    cols = (("when", _("Zeit"), 130), *((("where", _("Ort"), 50),) if where else ()), ("what", _("Auftrag"), 190),
            ("drive", _("Laufwerk"), 320), ("result", _("Ergebnis"), 110))
    tree = ttk.Treeview(frm, columns=[k for k, _t, _w in cols], show="headings", height=12, selectmode="browse")
    for key, title, width in cols:
        tree.heading(key, text=title, anchor="w")
        tree.column(key, width=ui_theme.px(width), anchor="w", stretch=key == "drive")
    tree.tag_configure("fail", foreground=c["danger"])
    tree.tag_configure("cancel", foreground=c["warn"])
    bar = ttk.Scrollbar(frm, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=ui_theme.autohide(bar))
    tree.grid(row=3, column=0, sticky="nsew")
    bar.grid(row=3, column=1, sticky="ns")
    detail = ttk.Label(frm, text="", wraplength=ui_theme.px(720), justify="left", style="Muted.TLabel")
    detail.grid(row=4, column=0, columnspan=2, sticky="w", pady=(ui_theme.px(10), 0))
    rows = {}
    stamp = {"de": "%d.%m.%Y %H:%M", "en": "%Y-%m-%d %H:%M"}.get(lang(), "%d/%m/%Y %H:%M")

    def draw_trends(merged):
        for w in trends.winfo_children():
            w.destroy()
        gs = speed_trends(e for _o, e in merged)
        if not gs:
            return
        ttk.Label(trends, text=_("Lesetempo je Datenträger"), style="Bold.TLabel").grid(row=0, column=0, columnspan=2,
                                                                                      sticky="w")
        w, h = ui_theme.px(140), ui_theme.px(30)
        for r, g in enumerate(gs, 1):
            cv = tk.Canvas(trends, width=w, height=h, bg=c["field"], highlightthickness=0, bd=0)
            cv.grid(row=r, column=0, sticky="w", pady=ui_theme.px(3))
            top, n = max(g["vals"]), len(g["vals"])
            pts = []
            for i, v in enumerate(g["vals"]):
                pts += [4 + i * (w - 8) / (n - 1), h - 4 - v / top * (h - 8)]
            cv.create_line(*pts, fill=c["danger"] if g["drop"] else c["accent"], width=max(2, ui_theme.px(2)),
                           joinstyle="round", capstyle="round")
            mb = lambda v: _("{n} MB/s", n=round(v))
            text = " · ".join(x for x in (g["name"], fmt_size(g["size"]) if g["size"] else "") if x) + " – " + _(
                "{n} Messungen, zuletzt {last}, zwischen {min} und {max}", n=n, last=mb(g["last"]), min=mb(min(g["vals"])),
                max=mb(top)) + (" – " + _("deutlich langsamer als früher") if g["drop"] else "")
            ttk.Label(trends, text=text, style="Muted.TLabel", foreground=c["danger"] if g["drop"] else None,
                      wraplength=ui_theme.px(500), justify="left").grid(row=r, column=1, sticky="w", padx=(ui_theme.px(10), 0))
        ttk.Frame(trends, height=ui_theme.px(8)).grid(row=len(gs) + 1, column=0)

    def fill(srcs):
        tree.delete(*tree.get_children())
        rows.clear()
        merged = sorted(((o, e) for o, entries in srcs for e in entries), key=lambda x: -int(x[1].get("t") or 0))
        draw_trends(merged)
        for i, (o, e) in enumerate(merged):
            state = "ok" if e.get("ok") else "cancel" if e.get("cancelled") else "fail"
            drive = " · ".join(str(x) for x in (
                (e.get("model") or "") + (f" ({e['device']})" if e.get("device") and e.get("model") else
                                          e.get("device") or ""),
                fmt_size(e["size"]) if e.get("size") else "", e.get("fs") or "",
                quote(e["label"]) if e.get("label") else "", e.get("file") or "") if x)
            when = time.strftime(stamp, time.localtime(int(e.get("t") or 0)))
            values = (when, *((o or "",) if where else ()), history_kind(e.get("kind", "")), drive,
                      {"ok": _("erfolgreich"), "cancel": _("abgebrochen"), "fail": _("fehlgeschlagen")}[state])
            iid = str(i)
            tree.insert("", "end", iid=iid, values=values, tags=(state,))
            rows[iid] = e
        n = len(merged)
        sub.configure(text=_("Die letzten Aufträge, neueste zuerst. Klick zeigt die Meldung.") if n
                      else _("Noch keine Aufträge."))
        clear_btn.state(["!disabled"] if n else ["disabled"])
        detail.configure(text="")
        if n:
            tree.selection_set("0")

    def show(_e=None):
        sel = tree.selection()
        e = rows.get(sel[0]) if sel else None
        if not e:
            detail.configure(text="")
            return
        extra = _("Dauer: {s} s", s=int(e["secs"])) if e.get("secs") else ""
        detail.configure(text="\n".join(x for x in (e.get("msg", ""), extra) if x))

    def clear():
        if messagebox.askyesno(_("Verlauf leeren?"), _("Die Liste der bisherigen Aufträge wird gelöscht. Laufwerke "
                                                      "und Sicherungen bleiben unverändert."),
                               parent=win, icon="warning"):
            current["srcs"] = on_clear()
            fill(current["srcs"])

    def save():
        from tkinter import filedialog
        path = filedialog.asksaveasfilename(parent=win, title=_("Verlauf speichern"), defaultextension=".csv",
                                            initialfile="usb-formatierer-verlauf-" + time.strftime("%Y-%m-%d") + ".csv",
                                            filetypes=[(_("CSV-Datei"), "*.csv")])
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(history_csv(current["srcs"]))
        except OSError as e:
            messagebox.showerror(_("Verlauf speichern"), str(e), parent=win)

    tree.bind("<<TreeviewSelect>>", show)
    btns = ttk.Frame(frm)
    btns.grid(row=5, column=0, columnspan=2, sticky="e", pady=(ui_theme.px(14), 0))
    current = {"srcs": sources}
    save_btn = ttk.Button(btns, text=_("Als Datei speichern (CSV)"), command=save)
    save_btn.pack(side="left", padx=(0, ui_theme.px(8)))
    clear_btn = ttk.Button(btns, text=_("Verlauf leeren"), command=clear)
    clear_btn.pack(side="left")
    close_btn = ttk.Button(btns, text=_("Schließen"), style="Accent.TButton", command=win.destroy)
    close_btn.pack(side="left", padx=(ui_theme.px(8), 0))
    fill(sources)
    win.bind("<Escape>", lambda e: win.destroy())
    win.update_idletasks()
    top = root.winfo_toplevel()
    x = top.winfo_rootx() + (top.winfo_width() - win.winfo_reqwidth()) // 2
    y = top.winfo_rooty() + (top.winfo_height() - win.winfo_reqheight()) // 3
    win.geometry(_inside(win, top, x, y))
    win.minsize(win.winfo_reqwidth() // 2, win.winfo_reqheight() // 2)
    win.deiconify()
    ui_theme.dark_titlebar(win)
    close_btn.focus_set()
    return win


# ---------------------------------------------------------------------- Oberfläche

def setup_window(root, theme="auto"):
    """Programmsymbol (auch für Dialoge) und das eigene Theme (hell/dunkel, "auto" = wie das System)"""
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
    ui_theme.apply_theme(root, theme)
    return ttk.Style(root)


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
    danger = bool(ack) or title == _("Alle Daten werden gelöscht")
    ok_btn = ttk.Button(btns, text=ok_text, style="Danger.TButton" if danger else "Accent.TButton",
                        command=lambda: done(True))
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
    ui_theme.dark_titlebar(win)
    cancel_btn.focus_set()
    grab(win)
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
    ttk.Button(btns, text=_("OK"), style="Accent.TButton", command=lambda: done(True)).pack(side="left")
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
    ui_theme.dark_titlebar(win)
    entry.focus_set()
    entry.select_range(0, "end")
    grab(win)
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
    ok_btn = ttk.Button(btns, text=ok_text, style="Accent.TButton", command=lambda: done(True))
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
    ui_theme.dark_titlebar(win)
    ok_btn.focus_set()
    grab(win)
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
    x = x0 + max(0, (aw - fw) // 2)
    if fh > ah:
        # höher als der Bildschirm (z. B. Full HD mit 150 % Skalierung): auf den Platz verkleinern - die
        # Laufwerksliste lässt sich dann scrollen. Wird der Inhalt wieder kleiner, gilt die natürliche Größe.
        win.geometry(f"{w}x{max(200, ah - top - side)}+{x}+{y0}")
        win._uf_clamped = True
        return
    if getattr(win, "_uf_clamped", False):
        win._uf_clamped = False
        win.geometry("")   # wieder so groß wie der Inhalt
    win.geometry(f"+{x}+{y0 + max(0, (ah - fh) // 2)}")


def show_centered(win):
    """Beim Start mittig anzeigen und mit dem echten Rahmen gleich noch einmal ausrichten. Danach jedes Mal wieder
    mittig, wenn sich die Größe ändert (Moduswechsel, Expertenmodus, Sprache, nachgeladene Listen) - nur verschieben
    (z. B. mit der Maus) lässt das Fenster, wo es ist."""
    center_window(win)
    win.deiconify()
    ui_theme.dark_titlebar(win)   # erst jetzt gibt es den Fensterrahmen (apply_theme lief vor dem ersten Zeigen)
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
    box = ttk.Combobox(parent, values=[n for _c, n in LANGS], state="readonly", width=9)
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


def version_tuple(v):
    """"2.5.6" -> (2, 5, 6); None, wenn keine gültige Versionsnummer (ältere Formatierer melden keine)"""
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", str(v or "").strip())
    return tuple(int(x) for x in m.groups()) if m else None


# Neue Version? Einmal je Programmstart bei GitHub nachsehen (nur lesen, nichts wird geladen oder installiert).
# USBFMT_NO_UPDATE_CHECK=1 schaltet es ab (Tests), USBFMT_UPDATE_URL setzt eine andere Quelle (Tests).
REPO_URL = "https://github.com/strongt1me/ps5-usb-formatierer"
UPDATE_API = "https://api.github.com/repos/strongt1me/ps5-usb-formatierer/releases/latest"
_update = {"state": None, "found": None}   # state: None = nicht gefragt, "busy", "done"
_update_lock = threading.Lock()


NOTES_MAX = 20000   # Zeichen der Release-Hinweise, die das Fenster "Was ist neu" höchstens zeigt


def newer_release(data, current=None):
    """Antwort von GitHub (releases/latest) auswerten. Neuer als die laufende Version: {"version", "url" (Release-Seite),
    "notes" (Hinweise als Markdown), "assets" ({Dateiname: Download-Link})}, sonst None. Links, die nicht in dieses
    Projekt führen, werden ersetzt bzw. weggelassen."""
    if not isinstance(data, dict) or data.get("draft") or data.get("prerelease"):
        return None
    tag = str(data.get("tag_name", "")).strip().lstrip("vV")
    new, cur = version_tuple(tag), version_tuple(APP_VERSION if current is None else current)
    if not new or not cur or new <= cur:
        return None
    url = f"{REPO_URL}/releases/tag/v{tag}"   # selbst gebaut: html_url könnte mit ../ aus dem Projekt führen
    assets, digests = {}, {}
    for a in data.get("assets") if isinstance(data.get("assets"), list) else []:
        if isinstance(a, dict):
            name, link = str(a.get("name", "")), str(a.get("browser_download_url", ""))
            if name and "/" not in name and link == f"{REPO_URL}/releases/download/v{tag}/{name}":
                assets[name] = link
                digest = str(a.get("digest") or "")   # SHA-256, von GitHub beim Hochladen berechnet
                if re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
                    digests[name] = digest[7:]
    notes = data.get("body")
    return {"version": ".".join(map(str, new)), "url": url, "assets": assets, "digests": digests,
            "notes": notes[:NOTES_MAX] if isinstance(notes, str) else ""}


def machine():
    """Prozessor-Architektur in der Schreibweise der Release-Dateien: x86_64 oder aarch64"""
    m = platform.machine().lower()
    return {"amd64": "x86_64", "x64": "x86_64", "arm64": "aarch64"}.get(m, m)


def release_file(found, app):
    """passende Datei des Releases für dieses System: (name, link) oder None. Linux: AppImage vor dem Archiv."""
    names = [f"{app}.exe"] if IS_WIN else [f"{app}-{machine()}.AppImage", f"{app}-linux-{machine()}.tar.gz"]
    for name in names:
        if name in found.get("assets", {}):
            return name, found["assets"][name]
    return None


def note_lines(md):
    """Release-Hinweise (Markdown) als schlichte Zeilen [(text, überschrift?)]: Überschriften ohne #, Aufzählungen
    mit •, Links nur als Text, keine doppelten Leerzeilen"""
    out = []
    for line in str(md or "").replace("\r\n", "\n").split("\n"):
        s = line.rstrip()
        head = re.match(r"^\s{0,3}#{1,6}\s+", s)
        s = s[head.end():] if head else s
        s = re.sub(r"^(\s*)[-*+]\s+", lambda m: m.group(1) + "• ", s)
        s = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", s)
        s = re.sub(r"(\*\*|__|`)", "", s)
        if s or (out and out[-1][0]):
            out.append((s, bool(head)))
    while out and not out[-1][0]:
        out.pop()
    return out


def plain_notes(md):
    """Release-Hinweise als schlichter Text (siehe note_lines)"""
    return "\n".join(s for s, _head in note_lines(md))


def _fetch_update():
    found = None
    try:
        req = urllib.request.Request(os.environ.get("USBFMT_UPDATE_URL") or UPDATE_API, headers={
            "Accept": "application/vnd.github+json", "User-Agent": f"USB-Formatierer/{APP_VERSION or '0'}"})
        with urllib.request.urlopen(req, timeout=8) as r:
            if r.getcode() in (None, 200):   # None: file:-Adresse (Tests)
                found = newer_release(json.loads(r.read(2 << 20).decode("utf-8")))
    except (OSError, ValueError):   # offline, gesperrt, Ratenbegrenzung: still bleiben
        pass
    with _update_lock:
        _update.update(state="done", found=found)


def start_update_check():
    if os.environ.get("USBFMT_NO_UPDATE_CHECK") or not APP_VERSION:
        return
    with _update_lock:
        if _update["state"]:
            return
        _update["state"] = "busy"
    threading.Thread(target=_fetch_update, daemon=True).start()


def update_dialog(root, found, app):
    """Fenster "Was ist neu": Release-Hinweise der neuen Version, Knopf zum Laden der passenden Datei (öffnet den
    Download im Browser - das Programm ersetzt sich nicht selbst) und zur Release-Seite"""
    import webbrowser
    c = ui_theme.colors()
    win = tk.Toplevel(root)
    win.withdraw()
    win.title(_("Neue Version {version}", version=found["version"]))
    win.transient(root)
    win.configure(bg=c["card"])
    frm = ttk.Frame(win, padding=ui_theme.pad((22, 18, 22, 16)))
    frm.pack(fill="both", expand=True)
    frm.columnconfigure(0, weight=1)
    frm.rowconfigure(2, weight=1)
    ttk.Label(frm, text=_("Neue Version {version}", version=found["version"]), style="H2.TLabel").grid(
        row=0, column=0, columnspan=2, sticky="w")
    ttk.Label(frm, text=_("Installiert ist v{version}. Was ist neu:", version=APP_VERSION), style="Muted.TLabel").grid(
        row=1, column=0, columnspan=2, sticky="w", pady=(ui_theme.px(4), ui_theme.px(8)))
    text = tk.Text(frm, wrap="word", width=72, height=16, relief="flat", borderwidth=0, highlightthickness=1,
                   highlightbackground=c["border"], highlightcolor=c["border"], bg=c["field"], fg=c["text"],
                   padx=ui_theme.px(10), pady=ui_theme.px(8), font="TkDefaultFont", cursor="arrow")
    bar = ttk.Scrollbar(frm, orient="vertical", command=text.yview)
    text.configure(yscrollcommand=bar.set)
    text.grid(row=2, column=0, sticky="nsew")
    bar.grid(row=2, column=1, sticky="ns")
    text.tag_configure("head", font="UfBold")
    lines = note_lines(found.get("notes")) or [(_("Keine Hinweise zu dieser Version."), False)]
    for i, (s, head) in enumerate(lines):
        text.insert("end", s + ("\n" if i < len(lines) - 1 else ""), ("head",) if head else ())
    text.configure(state="disabled")
    target = release_file(found, app)
    if not target:
        hint = _("Für dieses System liegt keine passende Datei im Release – bitte auf der Release-Seite wählen.")
    elif IS_WIN:
        hint = _("Die neue Datei ersetzt einfach die alte – die Einstellungen bleiben erhalten.")
    elif target[0].endswith(".AppImage"):
        hint = _("Unter Linux die geladene Datei ausführbar machen (chmod +x) und starten.")
    else:
        hint = _("Unter Linux das Archiv entpacken und das Programm starten.")
    ttk.Label(frm, text=hint, style="Hint.TLabel", wraplength=ui_theme.px(520), justify="left").grid(
        row=3, column=0, columnspan=2, sticky="w", pady=(ui_theme.px(10), 0))
    digest = found.get("digests", {}).get(target[0]) if target else None
    if digest:   # zum Vergleichen nach dem Laden - markier- und kopierbar
        ttk.Label(frm, text=_("Prüfsumme (SHA-256) von {file}:", file=target[0]), style="Hint.TLabel").grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(ui_theme.px(8), 0))
        var = tk.StringVar(value=digest)
        ttk.Entry(frm, textvariable=var, state="readonly", font="TkFixedFont").grid(
            row=5, column=0, columnspan=2, sticky="ew", pady=(ui_theme.px(2), 0))
        ttk.Label(frm, text=_("Prüfen: Windows „certutil -hashfile <Datei> SHA256“, Linux „sha256sum <Datei>“."),
                  style="Hint.TLabel", wraplength=ui_theme.px(520), justify="left").grid(
            row=6, column=0, columnspan=2, sticky="w", pady=(ui_theme.px(2), 0))
        win._digest = var   # Verweis halten
    btns = ttk.Frame(frm)
    btns.grid(row=7, column=0, columnspan=2, sticky="e", pady=(ui_theme.px(14), 0))

    def open_url(url):
        webbrowser.open(url)
        win.destroy()

    first = None
    if target:
        first = ttk.Button(btns, text=_("Herunterladen ({file})", file=target[0]), style="Accent.TButton",
                           command=lambda: open_url(target[1]))
        first.pack(side="left")
    page = ttk.Button(btns, text=_("Release-Seite öffnen"), style="TButton" if target else "Accent.TButton",
                      command=lambda: open_url(found["url"]))
    page.pack(side="left", padx=(ui_theme.px(8), 0))
    ttk.Button(btns, text=_("Schließen"), command=win.destroy).pack(side="left", padx=(ui_theme.px(8), 0))
    win.bind("<Escape>", lambda e: win.destroy())
    win.update_idletasks()
    top = root.winfo_toplevel()
    x = top.winfo_rootx() + (top.winfo_width() - win.winfo_reqwidth()) // 2
    y = top.winfo_rooty() + (top.winfo_height() - win.winfo_reqheight()) // 3
    win.geometry(_inside(win, top, x, y))
    win.minsize(win.winfo_reqwidth() // 2, win.winfo_reqheight() // 2)
    win.deiconify()
    ui_theme.dark_titlebar(win)
    (first or page).focus_set()
    return win


def update_note(parent, app):
    """Link "Neue Version … verfügbar" - erscheint, sobald die Prüfung eine neuere Version gefunden hat; ein Klick
    öffnet "Was ist neu" (update_dialog) für das Programm app (Name der Release-Datei)"""
    note = ttk.Label(parent, text="", style="PageLink.TLabel", cursor="hand2")
    after = [None]

    def show(found):
        note.configure(text="·  " + _("Neue Version {version} verfügbar", version=found["version"]))
        note.pack(side="left", padx=(8, 0))
        note.bind("<Button-1>", lambda _e=None: update_dialog(note.winfo_toplevel(), found, app))

    def poll(n=0):
        after[0] = None
        with _update_lock:
            state, found = _update["state"], _update["found"]
        if state == "done":
            if found:
                show(found)
        elif state and n < 60:   # höchstens 30 s warten
            after[0] = note.after(500, poll, n + 1)

    def stop(_e=None):
        if after[0]:
            note.after_cancel(after[0])
            after[0] = None
    note.bind("<Destroy>", stop)
    start_update_check()
    poll()
    return note


def credit(parent, ps5=True):
    """Profilbild und Name des Entwicklers, als Link zum GitHub-Profil; der Community-Spruch nur bei den PS5-Programmen"""
    frm = ttk.Frame(parent, style="Page.TFrame")
    scale = parent.winfo_fpixels("1i") / 96
    path = resource_path(f"avatar-{44 if scale >= 1.5 else 22}.png")
    if not os.path.isfile(path):   # Entwicklung: neben dem Programm
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.path.basename(path))
    try:
        img = tk.PhotoImage(file=path)
        pic = ttk.Label(frm, image=img, cursor="hand2", style="Page.TLabel")
        pic.image = img   # Verweis halten, sonst verschwindet das Bild
        pic.pack(side="left", padx=(0, 6))
    except tk.TclError:
        pic = None
    msg = (_("Entwickelt von {name} · für die PS5-Community", name=AUTHOR) if ps5
           else _("Entwickelt von {name}", name=AUTHOR))
    text = ttk.Label(frm, text=msg, style="Page.TLabel", cursor="hand2")
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
    frm = ttk.Frame(parent, style="Page.TFrame")
    row = ttk.Frame(frm, style="Page.TFrame")
    row.pack(pady=(ui_theme.px(10), 0))
    credit(row, ps5).pack(side="left")
    if APP_VERSION:
        ttk.Label(row, text=f"·  v{APP_VERSION}", style="Page.TLabel").pack(side="left", padx=(8, 0))
        update_note(row, "PS5-USB-Formatierer" if ps5 else "USB-Formatierer")
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
