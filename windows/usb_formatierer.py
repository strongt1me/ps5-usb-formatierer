"""PS5-USB-Formatierer fuer Windows.

Findet die PS5 im Netz (ps5_discovery) und spricht mit dem usb_format-Payload (HTTP-API,
Port 8090). Laeuft der Payload noch nicht, wird er an den ELF-Loader der PS5 (Port 9021) geschickt.
Am PC formatiert der Windows-Helfer (pc_common) - für die PS5 oder frei ("Am PC – frei", standard_panel).
Sprachen: Deutsch, English, Italiano, Español, Français (i18n).
"""
import hashlib
import http.client
import json
import os
import queue
import re
import socket
import subprocess
import threading
import time
import tkinter as tk
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from tkinter import filedialog, ttk

import i18n
import pc_common
import ps5_discovery
import ui_msgbox as messagebox
import ui_theme
from i18n import _, quote
from pc_common import (APP_VERSION, IMAGE_EXT, PcJob, admin_wait_text, ask_choice, ask_string, check_images, confirm,
                       disk_name, enable_dpi_awareness, enable_history, fmt_size, footer, history_clear, history_dialog,
                       history_load, hwnd_of, image_file_name, image_info, irreversible_admin_text, keep_on_screen,
                       lang_selector, last_job, last_job_text, open_help, pc_dev_text, pc_list, pc_used, pc_volumes_text, record_job,
                       resource_path, serial_args, setup_window, show_centered, show_scan, version_tuple)
from standard_panel import StandardPanel
from ui_widgets import THEME_NEXT, ChoiceChips, DriveList, Tooltip, header_tools

APP_NAME = "PS5USBFormatierer"
API_PORT = 8090
LOADER_PORT = 9021
LABEL_RE = re.compile(r"^[A-Za-z0-9 _-]{0,11}\Z")   # \Z: kein Zeilenumbruch am Ende
IP_RE = re.compile(r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$")
MODES = (("ps5", "An der PS5"), ("pc", "Am PC – für PS5"), ("std", "Am PC – frei"))
# Seitenleiste: An der PS5 drei Seiten, am PC die beiden Modi
NAV = (("An der PS5", (("ps5:format", "Formatieren"), ("ps5:games", "Spiele"), ("ps5:images", "Image-Sicherung"),
                      ("ps5:saves", "Spielstände"))),
       ("Am PC", (("pc", "Für die PS5 formatieren"), ("std", "Frei formatieren"))))


def find_payload():
    for p in (resource_path("usb_format.elf"),
              os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "usb_format", "usb_format.elf")):
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


def load_settings():
    return pc_common.load_settings(APP_NAME)


def save_settings(data):
    pc_common.save_settings(APP_NAME, data)


def dev_title(d):
    return d.get("model") or _("USB-Laufwerk")


def formattable(d):
    # Eingebundene Sticks werden nicht selbst ausgehängt (CE-108255-1): Der Benutzer zieht sie an der PS5 ab und
    # steckt sie wieder ein, der Formatierer übernimmt sie vor der PS5 ("replug")
    return not d["locked"]


def is_replug_step(step, flags=None):
    """Warten auf Abziehen/Einstecken? Neuere Payloads melden es ausdrücklich, ältere nur im (deutschen) Text"""
    if flags and "replug" in flags:
        return bool(flags["replug"])
    return "abziehen" in step or "einstecken" in step or "neu angemeldet" in step


def fat32_cluster_kb(size):
    g = 1024 ** 3
    return 4 if size <= 8 * g else 8 if size <= 16 * g else 16 if size <= 32 * g else 32


class ApiError(Exception):
    pass


# ohne System-Proxy: die PS5 steht im eigenen Netz, ein eingetragener Proxy fände sie nicht
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class Refused(ConnectionError):
    """Port 8090 lehnt ab: dort läuft (noch) kein Formatierer"""


class Api:
    def __init__(self, host):
        self.base = f"http://{host}:{API_PORT}"

    def _call(self, path, body=None, timeout=4):
        data = None
        headers = {}
        if body is not None:
            body = dict(body, lang=i18n.lang())   # Meldungen des Auftrags in dieser Sprache
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        sep = "&" if "?" in path else "?"
        req = urllib.request.Request(f"{self.base}{path}{sep}lang={i18n.lang()}", data=data, headers=headers)
        try:
            with _OPENER.open(req, timeout=timeout) as r:
                result = json.loads(r.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read().decode("utf-8", "replace")).get("error")
            except (ValueError, OSError, AttributeError):
                msg = None
            raise ApiError(str(msg) if msg else _("HTTP-Fehler {code}", code=e.code)) from None
        except (urllib.error.URLError, OSError, ValueError, http.client.HTTPException) as e:
            reason = getattr(e, "reason", e)
            err = Refused if isinstance(e, ConnectionRefusedError) or isinstance(reason, ConnectionRefusedError) \
                else ConnectionError
            raise err(str(e) or type(e).__name__) from None
        if not isinstance(result, dict):   # Formatierer liefert immer ein Objekt - alles andere ist kaputt
            raise ConnectionError(_("HTTP-Fehler {code}", code=200))
        return result

    def devices(self, timeout=4):
        r = self._call("/api/devices", timeout=timeout)
        try:   # Verlauf für "Zuletzt: …" an den Laufwerken - ältere Formatierer haben keinen
            r["history"] = self._call("/api/history", timeout=timeout).get("entries") or []
        except (ApiError, ConnectionError):
            r["history"] = []
        return r

    def status(self):
        return self._call("/api/status")

    def library(self):
        return self._call("/api/library", timeout=8)

    def info(self, timeout=4):
        return self._call("/api/info", timeout=timeout)

    def delete_games(self, keys, uninstall):
        return self._call("/api/delete", {"keys": ",".join(keys), "uninstall": uninstall})

    def format(self, device, fs, label, size, replug=False, limit=0, folders=True, verify=False, ident=""):
        body = {"device": device, "fs": fs, "label": label, "size": size, "replug": replug,
                "folders": folders, "verify": verify}
        if ident:
            body["ident"] = ident   # Seriennummer: ein getauschter baugleicher Stick wird abgelehnt
        if limit:
            body["limit"] = limit   # gefälschter Stick: nur den echten Bereich nutzen
        return self._call("/api/format", body)

    def cancel(self):
        return self._call("/api/cancel", {})

    # Image-Sicherung (.img.zst): Orte und Sicherungen auf der PS5, sichern, zurückschreiben, Download, Hochladen
    @staticmethod
    def _dev(dev):
        return {"device": dev["name"], "size": dev["size"], "ident": dev.get("ident", ""), "replug": bool(dev["mounts"])}

    def images(self):
        return self._call("/api/images", timeout=15)

    def backup(self, dev, dest):
        return self._call("/api/backup", dict(self._dev(dev), dest=dest))

    def restore(self, dev, path):
        return self._call("/api/restore", dict(self._dev(dev), path=path))

    def verify_image(self, path):
        return self._call("/api/images/verify", {"path": path})

    def delete_image(self, path):
        return self._call("/api/images/delete", {"path": path})

    def history(self):
        return self._call("/api/history").get("entries") or []

    def clear_history(self):
        return self._call("/api/history/clear", {})

    @staticmethod
    def _http_error(e):
        try:
            msg = json.loads(e.read().decode("utf-8", "replace")).get("error")
        except (ValueError, OSError, AttributeError):
            msg = None
        return ApiError(str(msg) if msg else _("HTTP-Fehler {code}", code=e.code))

    def download(self, token, path):
        """Sicherung nach path laden (Chunked bis zum Ende). Liefert (Bytes, SHA-256)."""
        url = f"{self.base}/api/backup/download?token={urllib.parse.quote(token)}&lang={i18n.lang()}"
        h, n = hashlib.sha256(), 0
        try:
            with _OPENER.open(url, timeout=300) as r, open(path, "wb") as f:
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    h.update(b)
                    n += len(b)
        except urllib.error.HTTPError as e:
            raise self._http_error(e) from None
        return n, h.hexdigest()

    def upload(self, dev, path):
        """Image-Datei hochladen; die PS5 schreibt sie dabei auf den Datenträger"""
        q = {"device": dev["name"], "size": dev["size"], "ident": dev.get("ident", ""), "lang": i18n.lang()}
        if dev["mounts"]:
            q["replug"] = "1"
        with open(path, "rb") as f:
            req = urllib.request.Request(f"{self.base}/api/restore/upload?{urllib.parse.urlencode(q)}", data=f,
                                         method="POST", headers={"Content-Type": "application/octet-stream",
                                                                 "Content-Length": str(os.path.getsize(path))})
            try:
                with _OPENER.open(req, timeout=300) as r:
                    r.read()
            except urllib.error.HTTPError as e:
                raise self._http_error(e) from None

    def quit(self):
        """Formatierer auf der PS5 beenden (lehnt ab, solange ein Auftrag läuft)"""
        return self._call("/api/quit", {})

    # Spielstände: auflisten, sichern, wiederherstellen
    def get(self, path, timeout=8):
        return self._call(path, timeout=timeout)

    def download_save(self, keys, path):
        """ZIP mit den Spielständen keys (Schlüsselliste) nach path laden (in einem Zug auf die Platte, erst nach
        vollständigem Empfang geprüft und umbenannt). Liefert die Größe."""
        enc = ",".join(urllib.parse.quote(k, safe="") for k in keys)
        url = f"{self.base}/api/saves/download?keys={enc}&lang={i18n.lang()}"
        part = path + ".part"
        n = 0
        try:
            with _OPENER.open(url, timeout=300) as r, open(part, "wb") as f:
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    n += len(b)
            try:
                with zipfile.ZipFile(part) as z:
                    bad = z.testzip()
            except zipfile.BadZipFile:
                bad = "?"
            if bad:
                raise ApiError(_("Die heruntergeladene ZIP-Datei ist beschädigt."))
            os.replace(part, path)
        except urllib.error.HTTPError as e:
            raise self._http_error(e) from None
        except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
            raise ConnectionError(str(e) or type(e).__name__) from None
        finally:
            try:
                os.remove(part)
            except OSError:
                pass
        return n

    def saves_locations(self):
        """Orte für Spielstand-Sicherungen auf der PS5 (intern /data/game_saves, USB <Laufwerk>/game_saves)"""
        return self._call("/api/saves/locations", timeout=15).get("locations") or []

    def saves_backups(self):
        """Auf der PS5 gefundene Sicherungen: (Liste von Einträgen mit path, username, title_name, dir, ...), Orte"""
        r = self._call("/api/saves/backups", timeout=30)
        return r.get("backups") or [], r.get("locations") or []

    def backup_saves(self, keys, dest):
        """Spielstände auf der PS5 sichern (Auftrag "save_backup")"""
        return self._call("/api/saves/backup", {"keys": "\n".join(keys), "dest": dest})

    def restore_saves(self, paths):
        """Sicherungen auf der PS5 zurückschreiben (Auftrag "save_restore")"""
        return self._call("/api/saves/restore", {"paths": "\n".join(paths)})

    def restore_save(self, path):
        """Spielstand-ZIP hochladen; die PS5 schreibt ihn zurück (Auftrag "save_restore", Antwort erst am Ende)"""
        with open(path, "rb") as f:
            req = urllib.request.Request(f"{self.base}/api/saves/restore/upload?lang={i18n.lang()}", data=f,
                                         method="POST", headers={"Content-Type": "application/zip",
                                                                 "Content-Length": str(os.path.getsize(path))})
            try:
                with _OPENER.open(req, timeout=600) as r:
                    r.read()
            except urllib.error.HTTPError as e:
                raise self._http_error(e) from None
            except (urllib.error.URLError, http.client.HTTPException) as e:
                raise ConnectionError(str(getattr(e, "reason", e)) or type(e).__name__) from None


def send_payload(host, path):
    with open(path, "rb") as f:
        data = f.read()
    with socket.create_connection((host, LOADER_PORT), timeout=5) as s:
        s.sendall(data)


def loader_error(host, e, stopped=False):
    """Verständliche Meldung, wenn das Senden an den ELF-Loader (Port 9021) scheitert: Port zu, PS5 antwortet nicht
    oder Übertragung abgebrochen. stopped: der bisherige Formatierer wurde vorher schon beendet."""
    if isinstance(e, ConnectionRefusedError):
        msg = _("Port {port} auf {host} ist geschlossen – auf der PS5 läuft kein ELF-Loader. Bitte den Jailbreak bzw. "
                "ELF-Loader auf der PS5 starten und dann erneut senden.", host=host, port=LOADER_PORT)
    # Zeitüberschreitung, Rechner bzw. Netz nicht erreichbar (Linux 110/113/101, Windows 10060/10065/10051)
    elif isinstance(e, (socket.timeout, TimeoutError)) or getattr(e, "errno", None) in (101, 110, 113, 10051, 10060,
                                                                                          10065):
        msg = _("{host} antwortet nicht (Port {port}). Ist die PS5 eingeschaltet und die IP-Adresse richtig?",
                host=host, port=LOADER_PORT)
    else:
        msg = _("Senden an {host} (Port {port}) abgebrochen: {e}", host=host, port=LOADER_PORT, e=e)
    if stopped:
        msg += "\n\n" + _("Der bisherige Formatierer auf der PS5 wurde schon beendet.")
    return msg


def port_open(host, port, timeout=1.0):
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False


def ask_backups(root, backups, locations=()):
    """Gefundene Sicherungen (nach Benutzer gruppiert, mehrere markierbar). Liefert ("paths", [Pfade]), ("file", None)
    für "Datei vom PC wählen …" oder None (abgebrochen)."""
    px = ui_theme.px
    win = tk.Toplevel(root)
    win.withdraw()
    win.title(_("Spielstand wiederherstellen"))
    win.transient(root)
    frm = ttk.Frame(win, padding=(18, 14, 18, 14))
    frm.pack(fill="both", expand=True)
    ttk.Label(frm, text=_("Gefundene Sicherungen auf der PS5 – markieren, was zurückgeschrieben werden soll. Ein "
                          "vorhandener Spielstand mit gleichem Namen wird ersetzt.") if backups
              else _("Keine Sicherungen auf der PS5 gefunden."), wraplength=560, justify="left").pack(anchor="w")
    imgs = check_images(win)
    checked = set()
    result = {"v": None}
    users = {}
    for b in backups:
        users.setdefault(b.get("username") or b.get("uid", ""), []).append(b)
    if backups:
        box = ttk.Frame(frm)
        box.pack(fill="both", expand=True, pady=(10, 0))
        box.columnconfigure(0, weight=1)
        tree = ttk.Treeview(box, columns=("meta",), show="tree", height=min(12, len(backups) + len(users)),
                            selectmode="none", style="Games.Treeview")
        tree.column("#0", width=px(380), minwidth=160, stretch=True)
        tree.column("meta", width=px(200), minwidth=120, anchor="e", stretch=False)
        tree.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(box, orient="vertical", command=tree.yview)
        sb.grid(row=0, column=1, sticky="ns", padx=(px(4), 0))
        tree.configure(yscrollcommand=sb.set)
        def where_of(b):   # wo die Sicherung liegt (dieselbe kann an mehreren Orten liegen)
            loc = next((x for x in locations if x.get("dir") == b.get("dir")), None)
            return (loc.get("label") or loc.get("device")) if loc and loc.get("device") else _("intern auf der PS5")

        for ui, (who, items) in enumerate(users.items()):
            tree.insert("", "end", iid=f"u{ui}", text=who, open=True)
            for b in items:
                when = time.strftime("%x %H:%M", time.localtime(b.get("time") or 0))
                tree.insert(f"u{ui}", "end", iid=b["path"], text=b.get("title_name") or b.get("title_id", ""),
                            values=(" · ".join(x for x in (b.get("platform") or "PS5", fmt_size(b.get("size", 0)), when, b.get("save_dir", "").removeprefix("sdimg_"), where_of(b)) if x),),
                            image=imgs[False])

    ok_btn = ttk.Button(frm, text=_("Wiederherstellen"), style="Accent.TButton")

    def sync():
        ok_btn.state(["!disabled"] if checked else ["disabled"])

    def toggle(iid):
        if not iid or not tree.exists(iid):
            return
        kids = tree.get_children(iid)
        if kids:   # Benutzerzeile: alle seine Sicherungen
            on = not all(k in checked for k in kids)
            for k in kids:
                (checked.add if on else checked.discard)(k)
                tree.item(k, image=imgs[on])
        else:
            (checked.discard if iid in checked else checked.add)(iid)
            tree.item(iid, image=imgs[iid in checked])
        sync()

    if backups:
        def click(e):
            if tree.identify_region(e.x, e.y) in ("tree", "cell"):
                toggle(tree.identify_row(e.y))
            return "break"

        tree.bind("<Button-1>", click)
        tree.bind("<space>", lambda e: toggle(tree.focus()))

    def done(v):
        result["v"] = v
        win.destroy()

    btns = ttk.Frame(frm)
    btns.pack(fill="x", pady=(16, 0))
    ttk.Button(btns, text=_("Datei vom PC wählen …"), command=lambda: done(("file", None))).pack(side="left")
    ttk.Button(btns, text=_("Abbrechen"), command=lambda: done(None)).pack(side="right")
    ok_btn.configure(command=lambda: done(("paths", [b["path"] for ub in users.values() for b in ub if b["path"] in checked])))
    ok_btn.pack(in_=btns, side="right", padx=(0, 6))
    sync()
    win.bind("<Escape>", lambda e: done(None))
    win.protocol("WM_DELETE_WINDOW", lambda: done(None))
    win.update_idletasks()
    top = root.winfo_toplevel()
    x = top.winfo_rootx() + (top.winfo_width() - win.winfo_reqwidth()) // 2
    y = top.winfo_rooty() + (top.winfo_height() - win.winfo_reqheight()) // 3
    win.geometry(pc_common._inside(win, top, x, y))
    win.deiconify()
    ui_theme.dark_titlebar(win)
    pc_common.grab(win)
    root.wait_window(win)
    return result["v"]


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.withdraw()
        self.report_callback_exception = self._on_exception
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.bind("<<StdClose>>", lambda e: self._on_close())
        self.bind("<<StdResized>>", lambda e: self.after_idle(self._keep_on_screen))
        self.settings = load_settings()
        setup_window(self, self.settings.get("theme", "auto"))   # Symbol (auch für Dialoge) und Theme
        i18n.set_lang(self.settings.get("lang") or i18n.system_lang())
        self.q = queue.Queue()
        self.api = None
        self.devices = []
        self.keep_sel = None   # beim Neuaufbau gewählter Datenträger (siehe _rebuild)
        self.ps5_history = []   # Verlauf des Formatierers ("Zuletzt: …" an den Laufwerkskarten)
        self.busy = False          # Netzwerkaktion oder Formatierung laeuft
        self.formatting = False
        self.elf_sending = False   # "ELF senden" läuft (Formatierer beendet bzw. ELF unterwegs)
        self.transfer = None       # Thread des Hoch-/Herunterladens einer Image-Sicherung (PC <-> PS5)
        self._bar_reset = None     # Zeitgeber: Balken nach dem Einlesen leeren
        self.watching = False      # Abfrage nach fremden Aufträgen unterwegs
        self.consoles = {}         # Anzeigetext -> Fund aus ps5_discovery
        self.ps5_devices = []      # zuletzt gelesene Liste der PS5 (bleibt beim Umschalten erhalten)
        self.pc_job = None         # laufender Auftrag des PC-Helfers
        self.games = {}            # PS5-Gerätename (da1 ...) -> Spiele darauf (aus /api/library)
        self.lib_loading = False
        self.lib_known = False     # Spieleliste mindestens einmal geladen (sonst Inhalt unbekannt)
        self.lib_bar = False       # Fortschritt der Spielesuche steht gerade im Balken
        self.version_asked = set()   # (PS5, Version), zu denen der Versionsabgleich schon gefragt hat
        self.settle_ids = []       # Zeitgeber: Liste nach einem PS5-Auftrag noch einmal leise holen
        self.ps5_images = []       # Sicherungen auf der PS5 (Seite "Image-Sicherung")
        self.ps5_step = ""         # aktueller Schritt eines PS5-Auftrags (für "Abbrechen")
        self.ps5_flags = {}        # dazu: replug/cancellable, falls der Payload sie meldet

        self.std_busy = False      # Auftrag im Bereich "Am PC – frei"
        mode = self.settings.get("mode")
        self.mode_var = tk.StringVar(value=mode if mode in dict(MODES) else "ps5")
        self.verify_var = tk.BooleanVar(value=self.settings.get("verify", False))
        self.folders_var = tk.BooleanVar(value=self.settings.get("folders", True))
        self.host_var = tk.StringVar(value=self.settings.get("host", ""))
        self.page = self.settings.get("page") if self.settings.get("page") in ("format", "games", "images") \
            else "format"
        self.fs_var = tk.StringVar(value="exFAT")
        self.cluster_var = tk.StringVar()
        saved_label = self.settings.get("label", "")   # die Eingabeprüfung greift nur beim Tippen
        self.label_var = tk.StringVar(value=saved_label if LABEL_RE.match(saved_label) else "")
        self.status_var = tk.StringVar(value=_("Nicht verbunden"))
        self.uninstall_var = tk.BooleanVar(value=True)

        self._build_ui()
        self._update()

        show_centered(self)   # mittig im nutzbaren Bildschirmbereich, ganz sichtbar
        self._poll_id = self.after(50, self._poll)
        self._mode_changed(initial=True)
        self._watch_id = self.after(3000, self._watch)
        self._sys_id = self.after(5000, self._follow_system)
        self._shortcuts()

    # ------------------------------------------------------------------ UI

    def _build_ui(self):
        """Links die Seitenleiste, rechts Kopfzeile, Inhalt der gewählten Seite, Fortschritt und Fußzeile"""
        px, gap = ui_theme.px, ui_theme.px(12)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        outer = self.outer = ttk.Frame(self, style="Page.TFrame", padding=ui_theme.pad((14, 14, 14, 10)))
        outer.grid(sticky="nsew")
        outer.columnconfigure(1, weight=1)
        outer.rowconfigure(0, weight=1)

        # ---- Seitenleiste
        side = ui_theme.card(outer, padding=(12, 14))
        side.grid(row=0, column=0, sticky="ns", padx=(0, gap))
        brand = ttk.Frame(side)
        brand.pack(fill="x", pady=(0, px(6)))
        ttk.Label(brand, image=ui_theme.app_tile(self, 32)).pack(side="left")
        names = ttk.Frame(brand)
        names.pack(side="left", padx=(px(10), 0))
        ttk.Label(names, text=_("PS5-USB-Formatierer"), style="Bold.TLabel").pack(anchor="w")
        ttk.Label(names, text="v" + APP_VERSION if APP_VERSION else "", style="Hint.TLabel").pack(anchor="w")
        self.nav_btns = {}
        for group, items in NAV:
            ttk.Label(side, text=_(group).upper(), style="Section.TLabel").pack(anchor="w", pady=(px(14), px(4)))
            for key, text in items:
                b = ttk.Button(side, text=_(text), style="Nav.TButton", command=lambda k=key: self._nav(k))
                b.pack(fill="x", pady=(0, px(2)))
                self.nav_btns[key] = b
                Tooltip(b, _("Strg+{n}", n=len(self.nav_btns)))
        self.mode_btns = list(self.nav_btns.values())
        self.conn_pill = ttk.Label(side, style="PillWarn.TLabel", text=_("Nicht verbunden"))
        self.conn_pill.pack(side="bottom", anchor="w", pady=(px(12), 0))

        # ---- rechts: Kopfzeile, Inhalt, Fortschritt, Fußzeile
        main = ttk.Frame(outer, style="Page.TFrame")
        main.grid(row=0, column=1, sticky="nsew")
        main.columnconfigure(0, weight=1)
        main.rowconfigure(1, weight=1)
        head = ui_theme.card(main, padding=(16, 9))
        head.grid(row=0, column=0, sticky="ew", pady=(0, gap))
        self.page_title = ttk.Label(head, style="H2.TLabel")
        self.page_title.pack(side="left")
        self.lang_box = header_tools(head, self.settings.get("theme", "auto"), self._toggle_theme,
                                     lambda par: lang_selector(par, i18n.lang(), self._set_lang),
                                     lambda: open_help(i18n.lang(), self), self._history)
        content = ttk.Frame(main, style="Page.TFrame")
        content.grid(row=1, column=0, sticky="nsew")
        content.columnconfigure(0, weight=1)
        content.rowconfigure(0, weight=1)

        # "Am PC – frei": eigener Bereich ohne PS5-Optionen (derselbe wie im eigenständigen USB-Formatierer)
        self.std_panel = StandardPanel(content, self.settings.setdefault("standard", {}),
                                       lambda: save_settings(self.settings), on_busy=self._std_busy_changed)
        self.std_panel.grid(row=0, column=0, sticky="nsew")

        # An der PS5 und "Am PC – für PS5": links Verbindung und Laufwerke, rechts die Seite
        body = self.ps5_body = ttk.Frame(content, style="Page.TFrame")
        body.grid(row=0, column=0, sticky="nsew")
        body.columnconfigure(0, weight=5, uniform="col")
        body.columnconfigure(1, weight=6, uniform="col")
        body.rowconfigure(0, weight=1)
        left = ttk.Frame(body, style="Page.TFrame")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, gap))
        left.columnconfigure(0, weight=1)
        left.rowconfigure(1, weight=1)

        conn = self.conn_card = ui_theme.card(left)
        conn.grid(row=0, column=0, sticky="ew", pady=(0, gap))
        conn.columnconfigure(0, weight=1)
        self.ps5_caption = ttk.Label(conn, text="PS5", style="Section.TLabel")
        self.ps5_caption.grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, px(8)))
        self.host_entry = ttk.Combobox(conn, textvariable=self.host_var)
        self.host_entry.grid(row=1, column=0, columnspan=2, sticky="ew")
        self.host_entry.bind("<Return>", lambda e: self._connect())
        self.host_entry.bind("<<ComboboxSelected>>", lambda e: self._connect())
        self.host_btns = ttk.Frame(conn)
        self.host_btns.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(px(8), 0))
        self.host_btns.columnconfigure((0, 1), weight=1, uniform="hb")
        self.search_btn = ttk.Button(self.host_btns, text=_("Suchen"), command=self._search)
        self.search_btn.grid(row=0, column=0, sticky="ew")
        # den Formatierer aus diesem Programm an den ELF-Loader der PS5 schicken (auch: ältere Fassung ersetzen)
        self.elf_btn = ttk.Button(self.host_btns, text=_("ELF senden"), command=self._send_elf)
        self.elf_btn.grid(row=0, column=1, sticky="ew", padx=(px(6), 0))
        Tooltip(self.search_btn, _("PS5 im Heimnetz suchen"))
        Tooltip(self.elf_btn, _("Den Formatierer aus diesem Programm an den ELF-Loader der PS5 senden (Port 9021) – "
                                "ersetzt eine laufende ältere Fassung"))

        drives = ui_theme.card(left)
        drives.grid(row=1, column=0, sticky="nsew")
        dh = ttk.Frame(drives)
        dh.pack(fill="x", pady=(0, px(8)))
        ttk.Label(dh, text=_("Laufwerke").upper(), style="Section.TLabel").pack(side="left")
        ico = ui_theme.icon(self, "refresh", "muted", 15)
        self.reload_btn = ttk.Button(dh, text=" " + _("Aktualisieren"), image=ico, compound="left",
                                     style="Ghost.TButton", command=self._reload)
        self.reload_btn.image = ico
        self.reload_btn.pack(side="right")
        Tooltip(self.reload_btn, lambda: _("Laufwerke neu einlesen (F5)") if not self.reload_btn.instate(["disabled"])
                else self._why(None, need_dev=False))
        # wie in der Weboberfläche: Hinweis ganz unten, eine Zeile (die Erklärung steht in der Anleitung)
        self.ps5_warn = ttk.Label(drives, style="Warn.TLabel", wraplength=px(380), justify="left",
                                  text=_("Während der Nutzung keine USB-Datenträger an der PS5 abziehen oder neu "
                                         "anschließen"))
        self.ps5_warn.pack(side="bottom", anchor="w", pady=(px(8), 0))
        self.dev_combo = DriveList(drives)
        self.dev_combo.pack(fill="both", expand=True)
        self.dev_combo.bind("<<ComboboxSelected>>", lambda e: (self._update(), self._show_games()))

        self.tools = ui_theme.card(left)   # nur am PC
        self.tools.grid(row=2, column=0, sticky="ew", pady=(gap, 0))
        self.tools.columnconfigure((0, 1), weight=1, uniform="tool")
        ttk.Label(self.tools, text=_("Werkzeuge").upper(), style="Section.TLabel").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, px(8)))
        self.speed_btn = ttk.Button(self.tools, text=_("Tempo messen"), command=self._pc_speed)
        self.speed_btn.grid(row=1, column=0, sticky="ew")
        self.rename_btn = ttk.Button(self.tools, text=_("Umbenennen"), command=self._pc_rename)
        self.rename_btn.grid(row=1, column=1, sticky="ew", padx=(px(6), 0))
        Tooltip(self.speed_btn, lambda: self._why(self.speed_btn) or _("Lese- und Schreibtempo messen (die Daten bleiben "
                                                                      "erhalten)"))
        Tooltip(self.rename_btn, lambda: self._why(self.rename_btn) or _("Bezeichnung des Laufwerks ändern"))

        right = ttk.Frame(body, style="Page.TFrame")
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(0, weight=1)

        # -- Seite "Formatieren" (an der PS5 und am PC für die PS5)
        fc = self.format_card = ui_theme.card(right)
        fc.grid(row=0, column=0, sticky="nsew")
        fc.columnconfigure(1, weight=1)
        ttk.Label(fc, text=_("Dateisystem"), style="Muted.TLabel").grid(row=0, column=0, columnspan=2, sticky="w",
                                                                         pady=(0, px(6)))
        self.fs_chips = ChoiceChips(fc, [("exFAT", _("exFAT – für Spiele")), ("FAT32", _("FAT32 – höchstens 4 GB je "
                                                                                        "Datei"))],
                                    self._fs_picked, columns=2)
        self.fs_chips.grid(row=1, column=0, columnspan=2, sticky="ew")
        ttk.Separator(fc).grid(row=2, column=0, columnspan=2, sticky="ew", pady=px(12))
        ttk.Label(fc, text=_("Zuordnungseinheit"), style="Muted.TLabel").grid(row=3, column=0, sticky="w",
                                                                               padx=(0, px(12)))
        self.cluster_combo = ttk.Combobox(fc, textvariable=self.cluster_var, state="disabled")
        self.cluster_combo.grid(row=3, column=1, sticky="ew")
        ttk.Label(fc, text=_("Bezeichnung"), style="Muted.TLabel").grid(row=4, column=0, sticky="w", padx=(0, px(12)),
                                                                         pady=(px(8), 0))
        vcmd = (self.register(lambda p: bool(LABEL_RE.match(p))), "%P")
        self.label_entry = ttk.Entry(fc, textvariable=self.label_var, validate="key", validatecommand=vcmd)
        self.label_entry.grid(row=4, column=1, sticky="ew", pady=(px(8), 0))
        self.label_hint = ttk.Label(fc, style="Hint.TLabel")
        self.label_hint.grid(row=5, column=1, sticky="w", pady=(px(3), 0))
        # dieselben Zusatzoptionen wie in der Weboberfläche (an der PS5 und am PC)
        self.pc_opts = ttk.Frame(fc)
        self.pc_opts.grid(row=6, column=0, columnspan=2, sticky="w", pady=(px(12), 0))
        self.verify_chk = ttk.Checkbutton(self.pc_opts, variable=self.verify_var, style="Switch.TCheckbutton",
                                          text=_("Vorher Echtheit und Tempo prüfen (ca. 2–10 Minuten)"))
        self.verify_chk.pack(anchor="w")
        self.folders_chk = ttk.Checkbutton(self.pc_opts, variable=self.folders_var, style="Switch.TCheckbutton",
                                           text=_("Ordner homebrew, etaHEN/games und pkg anlegen"))
        self.folders_chk.pack(anchor="w")
        fc.rowconfigure(7, weight=1)
        self.start_btn = ttk.Button(fc, text=_("Formatieren …"), style="Danger.TButton", command=self._start)
        self.start_btn.grid(row=8, column=0, columnspan=2, sticky="e", pady=(px(14), 0), ipadx=px(12))
        Tooltip(self.start_btn, lambda: self._why(self.start_btn))

        # -- Seite "Spiele": wie "Inhalt" in der Weboberfläche - Kästchen und Titel, rechts Titel-ID · Format ·
        # Größe. Ein Klick auf die Zeile (oder die Leertaste) hakt an bzw. ab - kein Markieren mit Strg/Umschalt nötig
        gb = self.games_box = ui_theme.card(right)
        gb.grid(row=0, column=0, sticky="nsew")
        gb.columnconfigure(0, weight=1)
        gb.rowconfigure(1, weight=1)
        ghead = ttk.Frame(gb)
        ghead.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, px(8)))
        ghead.columnconfigure(1, weight=1)
        self.games_caption = ttk.Label(ghead, text=_("Spiele auf diesem Datenträger:"), style="Bold.TLabel")
        self.games_caption.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, px(6)))
        self.game_filter = tk.StringVar()   # je Aufbau neu: die Beobachtung gilt nur den Widgets dieses Aufbaus
        self.game_search = ttk.Entry(ghead, textvariable=self.game_filter)
        self.game_search.grid(row=1, column=0, columnspan=2, sticky="ew")
        self._placeholder(self.game_search, self.game_filter, _("Suchen: Name oder Titel-ID …"))
        self.game_filter.trace_add("write", lambda *a: self._show_games())
        self.sort_keys = [("name", _("Name")), ("size", _("Größe")), ("id", _("Titel-ID"))]
        self.sort_combo = ttk.Combobox(ghead, state="readonly", width=10, values=[t for _k, t in self.sort_keys])
        self.sort_combo.current(0)
        self.sort_combo.grid(row=1, column=2, sticky="e", padx=(px(6), 0))
        self.sort_combo.bind("<<ComboboxSelected>>", lambda e: self._show_games())
        Tooltip(self.game_search, _("Spiele filtern (Strg+F)"))
        Tooltip(self.sort_combo, _("Sortieren nach"))
        style = ttk.Style(self)
        style.configure("Games.Treeview", rowheight=px(30))
        self.check_imgs = check_images(self)
        self.games_tree = ttk.Treeview(gb, columns=("meta",), show="tree", height=8, selectmode="none",
                                       style="Games.Treeview")
        self.games_tree.column("#0", width=px(260), minwidth=120, stretch=True)
        self.games_tree.column("meta", width=px(220), minwidth=120, anchor="e", stretch=False)
        self.games_tree.tag_configure("odd", background=ui_theme.colors()["card2"])
        self.games_tree.bind("<Button-1>", self._game_click)
        self.games_tree.bind("<space>", lambda e: self._game_toggle(self.games_tree.focus()))
        self.games_checked = set()
        self.games_tree.grid(row=1, column=0, sticky="nsew")
        sb = ttk.Scrollbar(gb, orient="vertical", command=self.games_tree.yview)
        sb.grid(row=1, column=1, sticky="ns", padx=(px(4), 0))
        self.games_tree.configure(yscrollcommand=sb.set)
        self.uninstall_chk = ttk.Checkbutton(gb, variable=self.uninstall_var, style="Switch.TCheckbutton",
                                             text=_("Kachel auch von der Startseite entfernen"))
        self.uninstall_chk.grid(row=2, column=0, columnspan=2, sticky="w", pady=(px(10), 0))
        gbar = ttk.Frame(gb)
        gbar.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(px(8), 0))
        self.del_btn = ttk.Button(gbar, text=_("Angehakte Spiele löschen …"), style="Danger.TButton",
                                  command=self._delete_games)
        self.del_btn.pack(side="right")
        self.games_sum = ttk.Label(gbar, style="Muted.TLabel")
        self.games_sum.pack(side="left")
        Tooltip(self.del_btn, lambda: self._why(self.del_btn) or _("Die angehakten Spiele vom Laufwerk löschen"))

        # -- Seite "Image-Sicherung" des Datenträgers an der PS5 (wie "Sichern & wiederherstellen" im Browser)
        ic = self.img_row = ui_theme.card(right)
        ic.grid(row=0, column=0, sticky="nsew")
        ic.columnconfigure(0, weight=1)
        ttk.Label(ic, text=_("Gewähltes Laufwerk als Image sichern oder ein Image zurückschreiben."),
                  style="Bold.TLabel", wraplength=px(440), justify="left").grid(row=0, column=0, sticky="w")
        ttk.Label(ic, text=_("Gesichert werden nur die belegten Bereiche, gepackt als .img.zst mit Prüfsumme – "
                             "intern auf der PS5, auf einem anderen USB-Laufwerk oder auf diesem PC."),
                  style="Hint.TLabel", wraplength=px(440), justify="left").grid(row=1, column=0, sticky="w",
                                                                                 pady=(px(6), 0))
        ib = ttk.Frame(ic)
        ib.grid(row=2, column=0, sticky="w", pady=(px(14), 0))
        self.backup_btn = ttk.Button(ib, text=_("Sichern …"), style="Accent.TButton", command=self._ps5_backup)
        self.backup_btn.pack(side="left")
        self.restore_btn = ttk.Button(ib, text=_("Wiederherstellen …"), command=self._ps5_restore)
        self.restore_btn.pack(side="left", padx=(px(8), 0))
        Tooltip(self.backup_btn, lambda: self._why(self.backup_btn) or _("Gewähltes Laufwerk als Image sichern"))
        Tooltip(self.restore_btn, lambda: self._why(self.restore_btn) or _("Ein Image auf das gewählte Laufwerk "
                                                                          "zurückschreiben"))
        ttk.Separator(ic).grid(row=3, column=0, sticky="ew", pady=px(14))
        ih = ttk.Frame(ic)
        ih.grid(row=4, column=0, sticky="ew", pady=(0, px(6)))
        ttk.Label(ih, text=_("Sicherungen auf der PS5").upper(), style="Section.TLabel").pack(side="left")
        ico2 = ui_theme.icon(self, "refresh", "muted", 15)
        self.img_reload_btn = ttk.Button(ih, text=" " + _("Aktualisieren"), image=ico2, compound="left",
                                         style="Ghost.TButton", command=self._load_img_list)
        self.img_reload_btn.image = ico2
        self.img_reload_btn.pack(side="right")
        ic.rowconfigure(5, weight=1)
        lf = ttk.Frame(ic)
        lf.grid(row=5, column=0, sticky="nsew")
        lf.columnconfigure(0, weight=1)
        lf.rowconfigure(0, weight=1)
        self.img_tree = ttk.Treeview(lf, columns=("meta",), show="tree", height=5, selectmode="browse",
                                     style="Games.Treeview")
        self.img_tree.column("#0", width=px(200), minwidth=120, stretch=True)
        self.img_tree.column("meta", width=px(270), minwidth=120, anchor="e", stretch=False)
        self.img_tree.tag_configure("odd", background=ui_theme.colors()["card2"])
        self.img_tree.grid(row=0, column=0, sticky="nsew")
        isb = ttk.Scrollbar(lf, orient="vertical", command=self.img_tree.yview)
        isb.grid(row=0, column=1, sticky="ns", padx=(px(4), 0))
        self.img_tree.configure(yscrollcommand=isb.set)
        self.img_tree.bind("<<TreeviewSelect>>", lambda e: self._update())
        self.img_hint = ttk.Label(ic, style="Hint.TLabel", wraplength=px(440), justify="left")
        self.img_hint.grid(row=6, column=0, sticky="w", pady=(px(6), 0))
        ia = ttk.Frame(ic)
        ia.grid(row=7, column=0, sticky="e", pady=(px(8), 0))
        self.img_verify_btn = ttk.Button(ia, text=_("Prüfen"), command=self._img_verify)
        self.img_verify_btn.pack(side="left")
        self.img_del_btn = ttk.Button(ia, text=_("Löschen …"), command=self._img_delete)
        self.img_del_btn.pack(side="left", padx=(px(8), 0))
        Tooltip(self.img_verify_btn, lambda: self._why(self.img_verify_btn, need_dev=False) or _(
            "Gewählte Sicherung ganz lesen und ihre Prüfsummen vergleichen (auf der PS5)"))
        Tooltip(self.img_del_btn, lambda: self._why(self.img_del_btn, need_dev=False) or _(
            "Gewählte Sicherung samt .sha256-Datei löschen"))

        # ---- Spielstände
        sc = self.saves_card = ui_theme.card(right)
        sc.grid(row=0, column=0, sticky="nsew")
        sc.columnconfigure(0, weight=1)
        ttk.Label(sc, text=_("PS5-Spielstände sichern und wiederherstellen."),
                  style="Bold.TLabel", wraplength=px(440), justify="left").grid(row=0, column=0, sticky="w")
        sh = ttk.Frame(sc)
        sh.grid(row=1, column=0, sticky="ew", pady=(px(10), 0))
        ttk.Label(sh, text=_("Spielstände").upper(), style="Section.TLabel").pack(side="left")
        ico_sr = ui_theme.icon(self, "refresh", "muted", 15)
        self.saves_reload_btn = ttk.Button(sh, text=" " + _("Aktualisieren"), image=ico_sr, compound="left",
                                           style="Ghost.TButton", command=self._load_saves)
        self.saves_reload_btn.image = ico_sr
        self.saves_reload_btn.pack(side="right")
        # Benutzerfilter
        uf = ttk.Frame(sc)
        uf.grid(row=2, column=0, sticky="ew", pady=(px(6), 0))
        uf.columnconfigure(1, weight=1)
        ttk.Label(uf, text=_("Benutzer")).grid(row=0, column=0, sticky="w", padx=(0, px(8)))
        self.saves_user_var = tk.StringVar(value="")
        self.saves_user_cb = ttk.Combobox(uf, textvariable=self.saves_user_var, state="readonly", width=30)
        self.saves_user_cb.grid(row=0, column=1, sticky="ew")
        self.saves_user_cb.bind("<<ComboboxSelected>>", lambda e: self._saves_filter_changed())
        sc.rowconfigure(3, weight=1)
        sf = ttk.Frame(sc)
        sf.grid(row=3, column=0, sticky="nsew", pady=(px(6), 0))
        sf.columnconfigure(0, weight=1)
        sf.rowconfigure(1, weight=1)
        self.saves_all_var = tk.BooleanVar(value=False)
        self.saves_all_chk = ttk.Checkbutton(sf, text=_("Alle auswählen"), variable=self.saves_all_var,
                                             command=self._saves_all_toggle)
        self.saves_all_chk.grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, px(4)))
        self.saves_tree = ttk.Treeview(sf, columns=("user", "console", "size"), show="tree headings", height=6,
                                       selectmode="none", style="Games.Treeview")
        self.saves_tree.heading("#0", text=_("Spiel"), anchor="w")
        self.saves_tree.heading("user", text=_("Benutzer"), anchor="w")
        self.saves_tree.heading("console", text=_("Konsole"), anchor="w")
        self.saves_tree.heading("size", text=_("Größe"), anchor="w")
        self.saves_tree.column("#0", width=px(210), minwidth=120, anchor="w", stretch=True)
        self.saves_tree.column("user", width=px(100), minwidth=70, anchor="w", stretch=False)
        self.saves_tree.column("console", width=px(60), minwidth=45, anchor="w", stretch=False)
        self.saves_tree.column("size", width=px(80), minwidth=60, anchor="w", stretch=False)
        self.saves_tree.tag_configure("odd", background=ui_theme.colors()["card2"])
        self.saves_tree.grid(row=1, column=0, sticky="nsew")
        ssb = ttk.Scrollbar(sf, orient="vertical", command=self.saves_tree.yview)
        ssb.grid(row=1, column=1, sticky="ns", padx=(px(4), 0))
        self.saves_tree.configure(yscrollcommand=ssb.set)
        self.saves_tree.bind("<Button-1>", self._save_click)
        self.saves_tree.bind("<space>", lambda e: self._save_toggle(self.saves_tree.focus()))
        self.saves_checked = set()
        self.saves_hint = ttk.Label(sc, style="Hint.TLabel", wraplength=px(440), justify="left")
        self.saves_hint.grid(row=4, column=0, sticky="w", pady=(px(4), 0))
        sa = ttk.Frame(sc)
        sa.grid(row=5, column=0, sticky="e", pady=(px(8), 0))
        self.saves_down_btn = ttk.Button(sa, text=_("Sichern …"), command=self._saves_download,
                                         style="Accent.TButton")
        self.saves_down_btn.pack(side="left")
        self.saves_rest_btn = ttk.Button(sa, text=_("Wiederherstellen …"), command=self._saves_restore)
        self.saves_rest_btn.pack(side="left", padx=(px(8), 0))
        self._saves_data = []      # [{key, title_id, title_name, save_dir, uid, username, size}]
        self._saves_users = []     # [{uid, username}]

        # ---- Fortschritt (der Bereich "frei" hat seinen eigenen)
        st = self.status_card = ui_theme.card(main, padding=(16, 10))
        st.grid(row=2, column=0, sticky="ew", pady=(gap, 0))
        st.columnconfigure(0, weight=1)
        self.pbar = ttk.Progressbar(st, maximum=100)
        self.pbar.grid(row=0, column=0, sticky="ew")
        cbox = ttk.Frame(st)
        cbox.grid(row=0, column=1, rowspan=2, sticky="e", padx=(px(12), 0))
        self.cancel_btn = ttk.Button(cbox, text=_("Abbrechen"), command=self._cancel)
        ttk.Label(st, textvariable=self.status_var, style="Muted.TLabel", wraplength=px(760), justify="left").grid(
            row=1, column=0, sticky="w", pady=(px(6), 0))
        footer(main).grid(row=3, column=0, sticky="ew")
        # alle Seiten gleich groß: das Fenster springt beim Umschalten nicht (der größte Bereich gibt das Maß vor)
        self.update_idletasks()
        content.columnconfigure(0, minsize=max(self.std_panel.winfo_reqwidth(), body.winfo_reqwidth()))

    def _nav(self, key):
        """Seitenleiste: Bereich wechseln (An der PS5: Seite; Am PC: Modus)"""
        if key.startswith("ps5:"):
            self.page = key[4:]
            self.settings["page"] = self.page
            if self.mode_var.get() != "ps5":
                self.mode_var.set("ps5")
                self._mode_changed()
            else:
                save_settings(self.settings)
                self._show_page()
        elif key != self.mode_var.get():
            self.mode_var.set(key)
            self._mode_changed()

    def _show_page(self):
        """Seitenleiste markieren, passende Karten zeigen, Überschrift setzen"""
        mode = self.mode_var.get()
        key = "ps5:" + self.page if mode == "ps5" else mode
        for k, b in self.nav_btns.items():
            b.configure(style="NavSel.TButton" if k == key else "Nav.TButton")
        titles = {"ps5:format": _("Formatieren"), "ps5:games": _("Spiele"), "ps5:images": _("Image-Sicherung"),
                  "ps5:saves": _("Spielstände"), "pc": _("Am PC – für PS5"), "std": _("Am PC – frei")}
        self.page_title.configure(text=titles.get(key, ""))
        if mode == "std":
            self.ps5_body.grid_remove()
            self.status_card.grid_remove()
            self.std_panel.grid()
            return
        self.std_panel.grid_remove()
        self.ps5_body.grid()
        self.status_card.grid()
        ps5 = mode == "ps5"
        self.conn_card.grid() if ps5 else self.conn_card.grid_remove()
        self.tools.grid_remove() if ps5 else self.tools.grid()
        self.ps5_warn.pack(side="bottom", anchor="w", pady=(ui_theme.px(8), 0)) if ps5 else self.ps5_warn.pack_forget()
        page = self.page if ps5 else "format"
        for name, card in (("format", self.format_card), ("games", self.games_box),
                           ("images", self.img_row), ("saves", self.saves_card)):
            card.grid() if name == page else card.grid_remove()
        if page == "images" and self.api:
            self._load_img_list()
        if page == "saves" and self.api:
            self._load_saves()
        self.dev_combo.set_empty_text(
            _("Kein USB-Datenträger am PC gefunden. Stick einstecken und „Aktualisieren“ klicken.") if not ps5 else
            _("Kein USB-Stick an der PS5 gefunden. Stick einstecken und „Aktualisieren“ klicken.") if self.api else
            _("Erst oben mit der PS5 verbinden."))

    def _shortcuts(self):
        """Tastenkürzel: F1 Anleitung, F5 neu einlesen, Strg+1 … 5 Seiten, Strg+F Spiele suchen"""
        self.bind_all("<F1>", lambda e: open_help(i18n.lang(), self))
        self.bind_all("<F5>", lambda e: self._key_ok() and self._f5())
        for i, key in enumerate(self.nav_btns):
            self.bind_all(f"<Control-Key-{i + 1}>", lambda e, k=key: self._key_ok() and self._nav(k))
        self.bind_all("<Control-f>", lambda e: self._key_ok() and self._find_game())
        self.bind_all("<Control-F>", lambda e: self._key_ok() and self._find_game())
        for seq in ("<Control-h>", "<Control-H>"):
            self.bind_all(seq, lambda e: not isinstance(e.widget, (tk.Entry, ttk.Entry)) and not self.grab_current()
                          and self._history())

    def _key_ok(self):
        """Tastenkürzel, die umschalten oder neu einlesen, nur wenn die Knöpfe dafür auch frei wären: kein Auftrag
        (sonst ließe sich z. B. derselbe Datenträger in einem anderen Bereich ein zweites Mal formatieren) und kein
        offener Dialog (der bestätigte Auftrag ginge sonst verloren)"""
        return not (self.busy or self.formatting or self.std_busy or self.grab_current())

    def _history(self):
        """Fenster "Verlauf": Aufträge am PC und - wenn verbunden - die des Formatierers auf der PS5 (im Hintergrund
        geholt; ältere Formatierer ohne Verlauf zeigen einfach nur den PC-Teil)"""
        api = self.api

        def ps5_entries():
            try:
                return api.history() if api else None
            except (ApiError, ConnectionError):
                return None

        def work():
            self.q.put(("history", api, ps5_entries()))
        threading.Thread(target=work, daemon=True).start()

    def _show_history(self, api, ps5):
        def sources(ps5_list):
            return [("PC", history_load())] + ([("PS5", ps5_list)] if ps5_list is not None else [])

        def clear():
            history_clear()
            if api:   # auf der PS5 im Hintergrund leeren - der PC-Teil ist sofort leer
                def work():
                    try:
                        api.clear_history()
                    except (ApiError, ConnectionError):
                        pass
                threading.Thread(target=work, daemon=True).start()
                return sources([])
            return sources(None)
        history_dialog(self, sources(ps5), clear)

    def _f5(self):
        mode = self.mode_var.get()
        if mode == "std":
            self.std_panel.reload()
        elif not self.reload_btn.instate(["disabled"]):
            self._reload()

    def _find_game(self):
        if self.mode_var.get() != "ps5" or self.page != "games":
            self._nav("ps5:games")
        self.game_search.focus_set()
        self.game_search.select_range(0, "end")
        return "break"

    @staticmethod
    def _placeholder(entry, var, text):
        """grauer Hinweistext im leeren Feld (ttk kennt keinen Platzhalter)"""
        state = {"on": False}

        def show(_e=None):
            if not var.get():
                state["on"] = True
                entry.configure(style="Placeholder.TEntry")
                var.set(text)

        def hide(_e=None):
            if state["on"]:
                state["on"] = False
                var.set("")
                entry.configure(style="TEntry")
        entry.bind("<FocusIn>", hide, add="+")
        entry.bind("<FocusOut>", show, add="+")
        entry.placeholder = (state, text)
        show()

    def _game_query(self):
        st, text = self.game_search.placeholder
        return "" if st["on"] else self.game_filter.get().strip().lower()

    def _why(self, btn, need_dev=True):
        """Kurzinfo eines gesperrten Knopfs: warum - leer, wenn er gerade geht"""
        if btn is not None and not btn.instate(["disabled"]):
            return ""
        if self.busy or self.formatting:
            return _("Es läuft gerade ein Auftrag.")
        if self.mode_var.get() == "ps5" and not self.api:
            return _("Erst mit der PS5 verbinden.")
        dev = self._selected_device()
        if need_dev and not dev:
            return _("Erst ein Laufwerk wählen.")
        if btn is self.del_btn:
            return _("Erst Spiele anhaken.")
        if btn in (self.img_verify_btn, self.img_del_btn):
            return _("Erst eine Sicherung in der Liste wählen.")
        if btn is self.rename_btn:
            return _("Umbenennen geht nur bei genau einem Laufwerk mit exFAT oder FAT32.")
        if dev and dev.get("locked"):
            return _("Dieses Laufwerk ist vom System gesperrt.")
        return ""

    # ------------------------------------------------------------------ Sicherungen auf der PS5

    def _load_img_list(self):
        if not self.api:
            return
        api = self.api

        def work():
            try:
                data = api.images()
            except (ConnectionError, ApiError):
                data = None
            self.q.put(("imglist", api, data))

        threading.Thread(target=self._guard(work, background=True), daemon=True).start()

    def _fill_img_list(self, data):
        imgs = sorted(data.get("images", []), key=lambda i: i.get("created", "") or i.get("name", ""), reverse=True)
        self.ps5_images = imgs
        keep = self.img_tree.selection()
        self.img_tree.delete(*self.img_tree.get_children())
        for n, i in enumerate(imgs):
            src = " ".join(x for x in (i.get("model", ""), quote(i["label"]) if i.get("label") else "") if x)
            where = _("intern") if not i.get("device") else i["device"]
            meta = " · ".join(x for x in (i.get("created", ""), fmt_size(i.get("bytes", 0)), where) if x)
            self.img_tree.insert("", "end", iid=i["path"], text="  " + (src or i["name"]), values=(meta,),
                                 tags=("odd",) if n % 2 else ())
        sel = [k for k in keep if self.img_tree.exists(k)]
        if sel:
            self.img_tree.selection_set(sel)
        self.img_hint.configure(text=_("Noch keine Sicherung auf der PS5.") if not imgs else
                                _("{n} Sicherungen – intern und auf eingebundenen USB-Laufwerken.", n=len(imgs)))
        self._update()

    def _img_selected(self):
        sel = self.img_tree.selection()
        return next((i for i in self.ps5_images if sel and i["path"] == sel[0]), None)

    def _img_verify(self):
        img = self._img_selected()
        if self.busy or not img or not self.api:
            return
        api = self.api

        def work():
            try:
                api.verify_image(img["path"])
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Prüfen konnte nicht gestartet werden: {e}", e=e)))
                return
            self._follow(api, foreign=False, done_title=_("Image geprüft."))

        self.pbar["value"] = 0
        self._run(work)

    def _img_delete(self):
        img = self._img_selected()
        if self.busy or not img or not self.api:
            return
        if not messagebox.askokcancel(
                _("Image löschen"), _("Diese Sicherung wird samt ihrer .sha256-Datei gelöscht:") + "\n\n" + img["name"]
                + "\n" + self.img_tree.item(img["path"], "values")[0] + "\n\n" + _("Das lässt sich nicht rückgängig "
                                                                                    "machen."),
                icon="warning", parent=self):
            return
        api = self.api

        def work():
            try:
                api.delete_image(img["path"])
                self.q.put(("imgdeleted", api, img["name"], ""))
            except (ConnectionError, ApiError) as e:
                self.q.put(("imgdeleted", api, img["name"], str(e)))

        self._run(work)

    # ------------------------------------------------------------------ Spielstände

    def _load_saves(self):
        """Spielstände und Benutzer von der PS5 laden (im Hintergrund)"""
        api = self.api
        if not api:
            return
        self.saves_hint.configure(text=_("Spielstand wird geladen …"))

        def work():
            try:
                saves = api.get("/api/saves")
                users = api.get("/api/saves/users")
                self.q.put(("saves_loaded", saves, users))
            except (ConnectionError, ApiError) as e:
                self.q.put(("saves_loaded_err", str(e)))

        threading.Thread(target=work, daemon=True).start()

    def _fill_saves(self, saves_data, users_data):
        """Spielstand-Liste und Benutzerfilter füllen"""
        self._saves_data = saves_data.get("saves", [])
        self._saves_users = users_data.get("users", [])
        cur = self.saves_user_cb.current()
        keep = self._saves_users[cur - 1]["uid"] if 0 < cur <= len(self._saves_users) else ""
        self.saves_user_cb["values"] = [_("Alle Benutzer")] + [u.get("username") or u["uid"] for u in self._saves_users]
        pos = next((i + 1 for i, u in enumerate(self._saves_users) if u["uid"] == keep), 0)
        self.saves_user_cb.current(pos)
        self._render_saves_tree()

    def _saves_filter_changed(self):
        self._render_saves_tree()

    def _render_saves_tree(self):
        i = self.saves_user_cb.current()
        uid_filter = self._saves_users[i - 1]["uid"] if 0 < i <= len(self._saves_users) else ""
        tree = self.saves_tree
        tree.delete(*tree.get_children())
        self.saves_checked &= {x["key"] for x in self._saves_data}   # nicht mehr vorhandene sind nicht mehr angehakt
        shown = [x for x in self._saves_data if not uid_filter or x.get("uid") == uid_filter]
        self._saves_keys = [x["key"] for x in shown]
        groups = {}   # ein Spiel mit mehreren Containern: Kopfzeile mit den Containern darunter
        for x in shown:
            groups.setdefault((x.get("uid"), x.get("title_id"), x.get("platform")), []).append(x)
        n = 0
        for (uid, tid, plat), items in groups.items():
            first = items[0]
            name = first.get("title_name") or first.get("title_id", "")
            who = first.get("username") or first.get("uid", "")
            if len(items) == 1:
                tree.insert("", "end", iid=first["key"], text=name,
                            values=(who, plat or "PS5", fmt_size(first["size"]) if first.get("size") else ""),
                            image=self.check_imgs[first["key"] in self.saves_checked], tags=("odd",) if n % 2 else ())
            else:
                gid = f"g:{uid}/{tid}/{plat}"
                tree.insert("", "end", iid=gid, text=name, open=False,
                            values=(who, plat or "PS5", fmt_size(sum(x.get("size", 0) for x in items))),
                            image=self.check_imgs[all(x["key"] in self.saves_checked for x in items)],
                            tags=("odd",) if n % 2 else ())
                for x in items:
                    tree.insert(gid, "end", iid=x["key"], text=x["save_dir"].removeprefix("sdimg_"),
                                values=("", "", fmt_size(x["size"]) if x.get("size") else ""),
                                image=self.check_imgs[x["key"] in self.saves_checked], tags=("odd",) if n % 2 else ())
            n += 1
        self.saves_hint.configure(text="" if shown else _("Keine Spielstände gefunden."))
        self._saves_sum()
        self._update()

    def _saves_shown(self):
        return list(getattr(self, "_saves_keys", []))

    def _saves_refresh_images(self):
        """Haken an allen Zeilen nachziehen (Kopfzeilen eines Spiels: angehakt, wenn alle Container angehakt sind)"""
        tree = self.saves_tree
        for iid in tree.get_children():
            kids = tree.get_children(iid)
            if kids:
                for k in kids:
                    tree.item(k, image=self.check_imgs[k in self.saves_checked])
                tree.item(iid, image=self.check_imgs[all(k in self.saves_checked for k in kids)])
            else:
                tree.item(iid, image=self.check_imgs[iid in self.saves_checked])

    def _saves_sum(self):
        shown = self._saves_shown()
        self.saves_all_var.set(bool(shown) and all(k in self.saves_checked for k in shown))
        n = len(self.saves_checked)
        self.saves_all_chk.configure(text=_("{n} ausgewählt", n=n) if n else _("Alle auswählen"))

    def _save_click(self, e):
        if self.saves_tree.identify_region(e.x, e.y) in ("tree", "cell"):
            self._save_toggle(self.saves_tree.identify_row(e.y))
        return "break"

    def _save_toggle(self, iid):
        if not iid or not self.saves_tree.exists(iid) or self.busy:
            return
        self.saves_tree.focus(iid)
        kids = list(self.saves_tree.get_children(iid))
        if kids:   # Kopfzeile: alle Container des Spiels an oder aus
            on = not all(k in self.saves_checked for k in kids)
            for k in kids:
                (self.saves_checked.add if on else self.saves_checked.discard)(k)
        elif iid in self.saves_checked:
            self.saves_checked.discard(iid)
        else:
            self.saves_checked.add(iid)
        self._saves_refresh_images()
        self._saves_sum()
        self._update()

    def _saves_all_toggle(self):
        if self.busy:
            self.saves_all_var.set(not self.saves_all_var.get())
            return
        on = self.saves_all_var.get()
        for k in self._saves_shown():
            (self.saves_checked.add if on else self.saves_checked.discard)(k)
        self._saves_refresh_images()
        self._saves_sum()
        self._update()

    def _saves_picked(self):
        return [x for x in self._saves_data if x["key"] in self.saves_checked]

    @staticmethod
    def _saves_loc_text(loc):
        if not loc.get("device"):
            where = _("intern auf der PS5")
        else:
            where = loc["device"] + (" " + quote(loc["label"]) if loc.get("label") else "")
        free = " · " + _("{size} frei", size=fmt_size(loc["free"])) if loc.get("free") is not None else ""
        return f"{where} · {loc['dir']}{free}"

    def _saves_download(self):
        """Markierte Spielstände sichern: auf den PC (ZIP) oder auf die PS5 (intern / USB, Ordner game_saves)"""
        picked = self._saves_picked()
        if self.busy or not picked or not self.api:
            return
        api = self.api
        try:
            locs = api.saves_locations()
        except (ConnectionError, ApiError) as e:
            messagebox.showerror(_("Spielstände sichern"), str(e), parent=self)
            return
        total = fmt_size(sum(x.get("size", 0) for x in picked))
        pick = ask_choice(self, _("Spielstände sichern"),
                          _("Zu sichern: {n} ({size}). Die Spielstände bleiben unverändert und verschlüsselt.",
                            n=len(picked), size=total),
                          [_("Auf diesen PC (ZIP-Datei speichern …)")] + [self._saves_loc_text(l) for l in locs],
                          _("Sichern"))
        if pick is None:
            return
        keys = [x["key"] for x in picked]
        if pick > 0:
            dest = locs[pick - 1]["dir"]

            def done(ok, msg, result):
                (messagebox.showinfo if ok else messagebox.showerror)(_("Spielstände sichern"), msg, parent=self)
                self._load_saves()

            def work():
                try:
                    api.backup_saves(keys, dest)
                except (ConnectionError, ApiError) as e:
                    self.q.put(("error", _("Sichern konnte nicht gestartet werden: {e}", e=e)))
                    return
                self._follow(api, foreign=False, done_title=_("Spielstände gesichert."), after=done)

            self.pbar["value"] = 0
            self._run(work)
            return
        name = f"{picked[0]['title_id']}_{picked[0]['save_dir']}.zip" if len(picked) == 1 else "Spielstaende.zip"
        path = filedialog.asksaveasfilename(
            parent=self, title=_("Spielstände sichern"), defaultextension=".zip",
            filetypes=[(_("ZIP-Sicherung"), "*.zip"), (_("Alle Dateien"), "*.*")],
            initialdir=self.settings.get("image_dir") or os.path.expanduser("~"), initialfile=name)
        if not path:
            return
        self.status_var.set(_("Spielstand wird geladen …"))

        def work_pc():
            try:
                n = api.download_save(keys, path)
                self.q.put(("saves_msg", True, _("Spielstände gesichert.") + " " + fmt_size(n)))
            except (ConnectionError, ApiError, OSError) as e:
                self.q.put(("saves_msg", False, str(e) or type(e).__name__))

        self._run(work_pc)

    def _saves_restore(self):
        """Wiederherstellen: gefundene Sicherungen auf der PS5 zum Markieren, oder eine ZIP-Datei vom PC"""
        if self.busy or not self.api:
            return
        api = self.api
        try:
            backups, locations = api.saves_backups()
        except (ConnectionError, ApiError) as e:
            messagebox.showerror(_("Spielstand wiederherstellen"), str(e), parent=self)
            return
        res = ask_backups(self, backups, locations)
        if not res:
            return
        kind, paths = res
        path = None
        if kind == "file":
            path = filedialog.askopenfilename(
                parent=self, title=_("Spielstand wiederherstellen"),
                filetypes=[(_("ZIP-Sicherung"), "*.zip"), (_("Alle Dateien"), "*.*")],
                initialdir=self.settings.get("image_dir") or os.path.expanduser("~"))
            if not path:
                return
        if not messagebox.askokcancel(
                _("Spielstand wiederherstellen"),
                _("Ein vorhandener Spielstand mit gleichem Namen wird ersetzt. Das lässt sich nicht rückgängig machen.")
                + "\n\n" + _("Laufende Spiele vorher beenden."),
                icon="warning", default="cancel", parent=self):
            return
        state = {}

        def done(ok, msg, result):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Spielstand wiederherstellen"), msg, parent=self)
            self._load_saves()

        if kind == "paths":
            def work_ps5():
                try:
                    api.restore_saves(paths)
                except (ConnectionError, ApiError) as e:
                    self.q.put(("error", _("Wiederherstellen konnte nicht gestartet werden: {e}", e=e)))
                    return
                self._follow(api, foreign=False, done_title=_("Spielstand wiederhergestellt."), after=done)

            self.pbar["value"] = 0
            self._run(work_ps5)
            return

        def upload():
            try:
                api.restore_save(path)
            except (ConnectionError, ApiError, OSError) as e:
                state["error"] = str(e) or type(e).__name__

        def work():
            try:
                seq = api.status().get("job") or 0
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Wiederherstellen konnte nicht gestartet werden: {e}", e=e)))
                return
            t = threading.Thread(target=upload, daemon=True)
            self.transfer = t
            t.start()
            if not self._wait_claim(api, seq, lambda: "error" in state):
                t.join(5)
                self.q.put(("error", state.get("error") or _("Das Hochladen hat nicht begonnen.")))
                return
            self._follow(api, foreign=False, done_title=_("Spielstand wiederhergestellt."), after=done)

        self.pbar["value"] = 0
        self._run(work)

    def _fs_picked(self, key):
        self.fs_var.set(key)
        self._update()

    def _toggle_theme(self):
        """Hell/Dunkel umschalten und merken; die Oberfläche wird im neuen Theme neu aufgebaut"""
        if self.busy or self.formatting or self.std_busy:
            messagebox.showinfo(self.title(), _("Das Design lässt sich nach dem laufenden Auftrag wechseln."),
                                parent=self)
            return
        self.settings["theme"] = THEME_NEXT.get(self.settings.get("theme", "auto"), "auto")
        save_settings(self.settings)
        ui_theme.apply_theme(self, self.settings["theme"])
        self._rebuild()

    def _follow_system(self):
        """Design "Automatisch": dem System folgen, auch wenn es im laufenden Betrieb umschaltet"""
        self._sys_id = self.after(5000, self._follow_system)
        if self.settings.get("theme", "auto") != "auto" or self.busy or self.formatting or self.std_busy \
                or self.grab_current() or any(isinstance(w, tk.Toplevel) for w in self.winfo_children()):
            return   # offener Dialog: sein Auftrag hinge sonst an der abgebauten Oberfläche
        want = "dark" if ui_theme.system_dark() else "light"
        if want != ui_theme.mode():
            ui_theme.apply_theme(self, "auto")
            self._rebuild()

    def _rebuild(self, relang=False):
        """Oberfläche neu aufbauen (Sprache, Design) - Verbindung, Funde und Laufwerke bleiben"""
        self.std_panel.may_close()   # Abfrage-Schleife des alten Bereichs beenden
        consoles = list(self.consoles.values())
        picked = self.consoles.get(self.host_var.get().strip())
        status = self.status_var.get()
        sel, checked, search = self._selected_device(), set(self.games_checked), self.game_filter.get()
        self.outer.destroy()
        self._build_ui()
        self.consoles = {self._console_text(c): c for c in consoles}   # Anzeigetexte ggf. in der neuen Sprache
        self.host_entry.configure(values=list(self.consoles))
        if picked:
            self.host_var.set(self._console_text(picked))
        if relang:
            self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")) if self.api
                                else _("Nicht verbunden"))
        else:
            self.status_var.set(status)
        # Auswahl, angehakte Spiele und Suche überstehen den Neuaufbau: die Laufwerksliste kommt danach neu (auch im
        # Hintergrund) - beim Füllen wird der gemerkte Datenträger wieder gewählt, wenn er noch derselbe ist
        self.keep_sel = sel
        self.games_checked = checked
        if search:
            self.game_filter.set(search)
        self._mode_changed(initial=True, relang=True)

    def _set_lang(self, code):
        """Sprache wechseln: Oberfläche in der neuen Sprache neu aufbauen (nicht während eines Auftrags)"""
        if code == i18n.lang():
            return
        if self.busy or self.formatting or self.std_busy:
            self.lang_box.set(dict(i18n.LANGS)[i18n.lang()])
            messagebox.showinfo(self.title(), _("Die Sprache lässt sich nach dem laufenden Auftrag wechseln."),
                                parent=self)
            return
        self.settings["lang"] = code
        save_settings(self.settings)
        i18n.set_lang(code)
        self._rebuild(relang=True)

    def _confirm(self, title, text, ok_text, ack=None):
        return confirm(self, title, text, ok_text, ack)

    def _keep_on_screen(self):
        keep_on_screen(self)   # die Bereiche sind verschieden hoch: wieder mittig

    def _std_busy_changed(self, busy):
        """Auftrag in "Am PC – frei": solange nicht umschalten"""
        self.std_busy = busy
        self._update()

    def _selected_device(self):
        i = self.dev_combo.current()
        return self.devices[i] if 0 <= i < len(self.devices) else None

    def _update(self):
        dev = self._selected_device()
        if self.fs_var.get() == "exFAT":
            self.cluster_var.set(_("64 Kilobytes"))
            self.label_hint.configure(text=_("Bis zu {n} Zeichen: A–Z, 0–9, Leerzeichen, - und _", n=11))
        else:
            self.cluster_var.set(_("{n} Kilobytes (Standard)", n=fat32_cluster_kb(dev["size"])) if dev
                                 else _("Standardgröße"))
            self.label_hint.configure(text=_("Bis zu 11 Zeichen, wird bei FAT32 großgeschrieben"))

        idle = not self.busy
        pc = self.mode_var.get() == "pc"

        def enable(w, on, *extra):
            w.state(list(extra) + (["!disabled"] if on else ["disabled"]))

        for w in (self.host_entry, self.label_entry, self.search_btn, self.elf_btn, self.verify_chk, self.folders_chk):
            enable(w, idle)
        for w in self.mode_btns:
            enable(w, idle and not self.std_busy)
        enable(self.lang_box, idle and not self.std_busy and not self.formatting, "readonly")
        enable(self.reload_btn, idle and (pc or self.api))
        enable(self.dev_combo, idle and self.devices, "readonly")
        self.fs_chips.select(self.fs_var.get())
        for key in ("exFAT", "FAT32"):
            self.fs_chips.enable(key, idle)
        if self.api:
            self.conn_pill.configure(style="PillOk.TLabel", text=_("Verbunden · {host}", host=self.settings.get("host", "")))
        else:
            self.conn_pill.configure(style="PillWarn.TLabel", text=_("Nicht verbunden"))
        # formattable() gilt nur für Laufwerke an der PS5 - nach dem Wechsel in "frei" steht hier noch ein PC-Laufwerk
        enable(self.start_btn, idle and dev and (self.mode_var.get() != "ps5" or formattable(dev)))
        enable(self.speed_btn, idle and dev)
        enable(self.del_btn, idle and not pc and self.api and self.games_checked)
        for w in (self.backup_btn, self.restore_btn):
            enable(w, idle and not pc and self.api and dev and not dev.get("locked"))
        has_img = bool(self.img_tree.selection())
        for w in (self.img_verify_btn, self.img_del_btn):
            enable(w, idle and not pc and self.api and has_img)
        enable(self.img_reload_btn, idle and self.api)
        has_save = bool(self.saves_checked)
        enable(self.saves_down_btn, idle and not pc and self.api and has_save)
        enable(self.saves_all_chk, idle and not pc and self.api)
        enable(self.saves_rest_btn, idle and not pc and self.api)
        enable(self.saves_reload_btn, idle and self.api)
        enable(self.uninstall_chk, idle)
        # nur bei genau einem Laufwerk auf dem Datenträger: der Helfer nimmt nicht unbedingt das angezeigte
        vols = dev.get("volumes", []) if dev else []
        enable(self.rename_btn, idle and dev and len(vols) == 1 and vols[0]["fs"] in ("exFAT", "FAT32"))
        # Abbrechen nur bei Warten auf Abziehen, Echtheitsprüfung und Tempomessung (Formatieren selbst dauert Sekunden)
        if "cancellable" in self.ps5_flags:
            ps5_can = bool(self.ps5_flags["cancellable"])
        else:   # ältere Payloads: am (deutschen) Schritt erkennen
            ps5_can = is_replug_step(self.ps5_step) or bool(re.search(r"Echtheit|[Tt]empo|^Prüfen", self.ps5_step))
        ps5_cancellable = not pc and self.formatting and not self.pc_job and ps5_can
        if (self.pc_job and getattr(self.pc_job, "cancellable", False)) or ps5_cancellable:
            if not self.cancel_btn.winfo_ismapped():
                self.cancel_btn.state(["!disabled"])   # nach einem früheren Abbruch noch gesperrt
                self.cancel_btn.pack()
        elif self.cancel_btn.winfo_ismapped():
            self.cancel_btn.pack_forget()

    def _mode_changed(self, initial=False, relang=False):
        mode = self.mode_var.get()
        pc = mode == "pc"
        if not initial:
            self.settings["mode"] = mode
            save_settings(self.settings)
        self.after_idle(self._keep_on_screen)   # die Bereiche sind verschieden hoch
        self._show_page()
        if mode == "std":   # eigener Bereich ohne PS5-Funktionen
            self.title(_("USB-Datenträger formatieren"))
            self.std_panel.activate()
            self._update()
            return
        self.title(_("PS5-USB formatieren"))
        self.pbar["value"] = 0
        if pc:
            self.devices = []
            self.dev_combo.set_items([])
            self._pc_reload()
        else:
            self._fill_devices(self.ps5_devices)
            if self.api:
                self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")))
            elif relang:
                self.status_var.set(_("Nicht verbunden"))
            elif initial or not self.consoles:
                self.after(200 if initial else 0, self._search)
            else:
                self.status_var.set(_("Nicht verbunden"))
        self._update()

    def _ps5_dev_text(self, d):
        """Kurz, damit die Auswahlliste nichts abschneidet; Erklärungen stehen in den Rückfragen"""
        fmt = d.get("format") or ""
        fmt = {"leer": _("leer"), "keine Partition": _("ohne Partition"),
               "unbekannt": _("Format unbekannt")}.get(fmt, fmt)
        if d.get("label") and fmt:
            fmt += " " + quote(d["label"])
        where = _("gesperrt") if d["locked"] else _("eingebunden") if d["mounts"] else _("frei")
        parts = [f"{dev_title(d)} ({d['name']})", fmt_size(d["size"])] + ([fmt] if fmt else []) + [where]
        if d.get("fake_real"):
            parts.append(_("GEFÄLSCHT – echt nur {size}", size=fmt_size(d["fake_real"])))
        n = len(self.games.get(d["name"], []))
        if n:
            parts.append(_("1 Spiel") if n == 1 else _("{n} Spiele", n=n))
        return " · ".join(parts)

    def _ps5_drive_item(self, d):
        """Karte eines Laufwerks an der PS5: Modell, Größe, Format, eingebunden/frei, Fälschung, Spiele, Belegung"""
        fmt = d.get("format") or ""
        fmt = {"leer": _("leer"), "keine Partition": _("ohne Partition"),
               "unbekannt": _("Format unbekannt")}.get(fmt, fmt)
        pills = [(fmt, "Accent")] if fmt else []
        pills.append((_("gesperrt"), "Danger") if d["locked"] else (_("eingebunden"), "Ok") if d["mounts"]
                     else (_("frei"), ""))
        if d.get("fake_real"):
            pills.append((_("gefälscht · echt {size}", size=fmt_size(d["fake_real"])), "Warn"))
        n = len(self.games.get(d["name"], []))
        if n:
            pills.append((_("1 Spiel") if n == 1 else _("{n} Spiele", n=n), ""))
        sub = d["name"] + (" · " + quote(d["label"]) if d.get("label") else "")
        used = None
        tot = sum(m.get("total", 0) for m in d["mounts"])
        if tot:
            used = max(0.0, min(1.0, 1 - sum(m.get("free", 0) for m in d["mounts"]) / tot))
        last = last_job(self.ps5_history, d.get("model"), d.get("size"), d.get("ident", ""))
        return {"text": self._ps5_dev_text(d), "title": dev_title(d), "size": fmt_size(d["size"]), "sub": sub,
                "pills": pills, "used": used, "last": last_job_text(last) if last else ""}

    def _show_games(self):
        """Spiele des gewählten PS5-Datenträgers in der Liste darunter. Während die PS5 sucht, kommt die Liste jede
        Sekunde neu: nur bei Änderungen neu aufbauen, Auswahl und Scrollstand bleiben"""
        dev = self._selected_device()
        if self.mode_var.get() != "ps5" or not dev:
            self.games_tree.delete(*self.games_tree.get_children())
            self.games_checked.clear()
            self._games_shown = None
            self.games_caption.configure(text=_("Spiele auf diesem Datenträger:"))
            return
        games = self.games.get(dev["name"], [])
        q = self._game_query()
        sort = self.sort_keys[max(0, self.sort_combo.current())][0]
        key = {"name": lambda g: (g["name"] or g["id"]).lower(), "size": lambda g: -(g.get("size") or 0),
               "id": lambda g: g.get("id", "")}[sort]
        shown = [g for g in games if not q or q in (g["name"] or "").lower() or q in g.get("id", "").lower()]
        rows = []
        for g in sorted(shown, key=key):
            kind = _("Ordner") if g["kind"] == "Ordner" else _("Abbild {kind}", kind=g["kind"])   # Art kommt deutsch
            meta = " · ".join(x for x in (g["id"], kind, fmt_size(g["size"]) if g["size"] else "") if x)
            rows.append((g.get("key") or None, g["name"] or g["id"], meta))
        if rows != getattr(self, "_games_shown", None):
            top = self.games_tree.yview()[0]
            self.games_tree.delete(*self.games_tree.get_children())
            keys = {iid for iid, _n, _m in rows if iid}
            self.games_checked &= keys   # nicht mehr vorhandene Spiele sind nicht mehr angehakt
            for i, (iid, name, meta) in enumerate(rows):
                self.games_tree.insert("", "end", iid=iid, text="  " + name, values=(meta,),
                                       image=self.check_imgs[iid in self.games_checked], tags=("odd",) if i % 2 else ())
            self.games_tree.yview_moveto(top)
            self._games_shown = rows
        self._games_sum()
        if games and q:
            text = _("Spiele auf diesem Datenträger: {shown} von {n}", shown=len(shown), n=len(games))
        elif games:
            text = _("Spiele auf diesem Datenträger: {n}", n=len(games))
        elif self.lib_loading:
            text = _("Spiele auf diesem Datenträger: werden gesucht …")
        else:
            text = _("Spiele auf diesem Datenträger: keine gefunden")
        self.games_caption.configure(text=text)

    def _game_click(self, e):
        if self.games_tree.identify_region(e.x, e.y) in ("tree", "cell"):
            self._game_toggle(self.games_tree.identify_row(e.y))
        return "break"

    def _game_toggle(self, iid):
        """Spiel an- bzw. abhaken (nur Spiele mit Schlüssel lassen sich löschen)"""
        if not iid or not self.games_tree.exists(iid) or self.busy:
            return
        self.games_tree.focus(iid)
        if iid in self.games_checked:
            self.games_checked.discard(iid)
        else:
            self.games_checked.add(iid)
        self.games_tree.item(iid, image=self.check_imgs[iid in self.games_checked])
        self._games_sum()
        self._update()

    def _games_sum(self):
        """unter der Liste: wie viele Spiele angehakt sind und wie viel Platz das frei macht"""
        dev = self._selected_device()
        games = {g.get("key"): g for g in self.games.get(dev["name"], [])} if dev and "name" in dev else {}
        picked = [games[k] for k in self.games_checked if k in games]
        self.games_sum.configure(text=_("{n} angehakt · {size}", n=len(picked), size=fmt_size(
            sum(g.get("size") or 0 for g in picked))) if picked else "")

    def _relibrary(self, api=None):
        if api is not None and api is not self.api:
            return   # Zeitgeber einer vorher verbundenen PS5: deren Kette endet hier
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
                self.q.put(("library", data if isinstance(data, dict) else None, api))

        threading.Thread(target=self._guard(work, background=True), daemon=True).start()

    def _fill_pc_devices(self, devices):
        prev, self.keep_sel = self._selected_device() or self.keep_sel, None
        self.devices = devices
        self.dev_combo.set_items([StandardPanel._drive_item(d) for d in devices])
        # am PC wird nie vorausgewählt: dort hängen oft Platten mit Daten (Spiele-Sicherung, Backups)
        idx = next((i for i, d in enumerate(devices) if prev and d["disk"] == prev.get("disk")
                    and d["size"] == prev.get("size") and d.get("model") == prev.get("model")
                    and d.get("serial", "") == prev.get("serial", "")), -1)
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_combo.current(-1)
        if not devices:
            self.status_var.set(_("Kein USB-Datenträger am PC gefunden. Stick einstecken und „Aktualisieren“ "
                                  "klicken."))
        else:
            self.status_var.set(_("{n} USB-Datenträger am PC gefunden – bitte oben auswählen.", n=len(devices)))

    def _fill_devices(self, devices):
        for d in devices:   # ältere/andere Payload-Versionen: fehlende Felder ergänzen
            for k, default in (("name", "?"), ("model", ""), ("size", 0), ("locked", False), ("mounts", []),
                               ("format", ""), ("label", ""), ("fake_real", 0), ("ident", "")):
                d.setdefault(k, default)
        self.ps5_devices = devices
        if self.mode_var.get() == "pc":
            return
        prev, self.keep_sel = self._selected_device() or self.keep_sel, None
        if prev and "disk" in prev:
            prev = None
        self.devices = devices
        self.dev_combo.set_items([self._ps5_drive_item(d) for d in devices])
        idx = -1
        if prev:   # gleicher Name reicht nicht: steckt dahinter ein anderer Datenträger (anderer Stick, die
            # Spiele-SSD), wird nichts übernommen
            idx = next((i for i, d in enumerate(devices) if d["name"] == prev["name"] and d["size"] == prev["size"]
                        and d["model"] == prev["model"] and d.get("ident", "") == prev.get("ident", "")), -1)
        if idx < 0:
            # vorausgewählt wird nur ein freier Stick, nie ein eingebundener (z. B. die Spiele-SSD)
            free = [i for i, d in enumerate(devices) if formattable(d) and not d["mounts"]]
            idx = free[0] if len(free) == 1 else -1
        if idx >= 0:
            self.dev_combo.current(idx)
        else:
            self.dev_combo.current(-1)
        if not devices and self.api:
            self.status_var.set(_("Kein USB-Stick an der PS5 gefunden. Stick einstecken und „Aktualisieren“ "
                                  "klicken."))
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
                self.q.put(("bgerror" if background else "error", _("Unerwarteter Fehler: {e}", e=e)))
        return run

    @staticmethod
    def _console_text(c):
        extra = " (" + _("Formatierer läuft") + ")" if c["formatter"] else \
            " (" + _("Ruhemodus") + ")" if c["standby"] else ""
        return f"{c['name']} – {c['ip']}{extra}"

    def _search(self):
        if self.busy:
            return

        def work():
            texts = {"net": _("Suche PS5 im Netzwerk …"),
                     "home": _("Keine PS5 im eigenen Netz – suche in weiteren Heimnetzen …")}
            self.q.put(("scan", 0, texts["net"]))
            self.q.put(("found", ps5_discovery.discover(
                API_PORT, progress=lambda pct, stage: self.q.put(("scan", pct, texts.get(stage, texts["net"]))))))

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
            self.status_var.set(_("{n} PS5 gefunden – bitte oben auswählen.", n=len(consoles)))
        elif saved:
            self.host_var.set(saved)
            self.status_var.set(_("Keine PS5 gefunden – versuche zuletzt verwendete Adresse {host} …", host=saved))
            self._connect()
        else:
            self.status_var.set(_("Keine PS5 gefunden. Ist sie eingeschaltet und im selben Netzwerk? "
                                  "Die IP-Adresse kann auch oben eingegeben werden (Enter)."))

    def _chosen_host(self):
        """Adresse der gewählten bzw. eingetippten PS5 - None (nach einer Meldung), wenn keine gültige oder die PS5
        im Ruhemodus ist"""
        text = self.host_var.get().strip()
        console = self.consoles.get(text)
        host = console["ip"] if console else text
        m = IP_RE.match(host)
        if not m or any(int(x) > 255 for x in m.groups()):
            messagebox.showerror(self.title(), _("Bitte eine gefundene PS5 auswählen oder ihre IP-Adresse eingeben, "
                                                 "z. B. 192.168.178.40.\n\nSie steht auf der PS5 unter Einstellungen › "
                                                 "Netzwerk › Verbindungsstatus."), parent=self)
            return None
        if console and console["standby"] and not console["formatter"]:
            messagebox.showerror(self.title(), _("{name} ist im Ruhemodus. Bitte die PS5 einschalten, den ELF-Loader "
                                                 "starten und dann „Suchen“ klicken.", name=console["name"]),
                                 parent=self)
            return None
        return host

    def _use_host(self, host):
        """Neue PS5 merken; alte Verbindung vergessen: scheitert die neue, darf "Starten" nicht die vorige PS5
        formatieren"""
        self.settings["host"] = host
        save_settings(self.settings)
        self.api = None
        self.games = {}
        self.lib_known = self.lib_loading = False   # Spieleliste gehört zur alten PS5
        self._fill_devices([])
        return Api(host)

    def _connect(self):
        if self.busy:
            return
        host = self._chosen_host()
        if not host:
            return
        api = self._use_host(host)

        def work():
            self.q.put(("status", _("Verbinde mit {host} …", host=host)))
            try:
                self.q.put(("connected", api, api.devices(timeout=2)))
                return
            except ApiError as e:
                self.q.put(("error", str(e)))
                return
            except Refused:
                pass
            except ConnectionError:   # keine Antwort in 2 s: läuft er vielleicht schon (WLAN-Aussetzer)? Geduldiger fragen
                try:
                    self.q.put(("connected", api, api.devices(timeout=8)))
                    return
                except ApiError as e:
                    self.q.put(("error", str(e)))
                    return
                except ConnectionError:
                    pass

            payload = find_payload()
            if not payload:
                self.q.put(("error", _("Die eingebaute usb_format.elf fehlt – bitte das Programm neu herunterladen.")))
                return
            self.q.put(("status", _("Formatierer läuft noch nicht – sende Payload an die PS5 …")))
            self.elf_sending = True   # Schließen fragt nach: sonst bliebe eine halbe ELF beim Loader
            try:
                send_payload(host, payload)
            except OSError as e:
                self.q.put(("error", loader_error(host, e)))
                return
            finally:
                self.elf_sending = False
            for attempt in range(20):
                self.q.put(("scan", attempt * 5, _("Warte, bis der Formatierer auf der PS5 antwortet …")))
                time.sleep(0.5)
                try:
                    self.q.put(("connected", api, api.devices(timeout=2)))
                    return
                except (ConnectionError, ApiError):
                    pass
            self.q.put(("error", _("Der Payload wurde gesendet, antwortet aber nicht. "
                                   "Bitte die PS5-Benachrichtigungen prüfen.")))

        self._run(work)

    def _send_elf(self, ask=True):
        """usb_format.elf aus diesem Programm an den ELF-Loader der PS5 (Port 9021) schicken. Läuft dort schon ein
        Formatierer, wird er nach Rückfrage beendet und ersetzt - nie während eines Auftrags. ask=False: die Rückfrage
        hat schon der Versionsabgleich gestellt."""
        if self.busy:
            return
        host = self._chosen_host()
        if not host:
            return
        payload = find_payload()
        if not payload:
            messagebox.showerror(self.title(), _("Die eingebaute usb_format.elf fehlt – bitte das Programm neu "
                                                 "herunterladen."), parent=self)
            return

        prev_status = self.status_var.get()

        def check():   # erst nachsehen, was auf der PS5 läuft - die Rückfrage hängt davon ab
            self.q.put(("status", _("Prüfe {host} …", host=host)))
            try:
                st = Api(host).status()
            except Refused:      # Port 8090 lehnt ab: dort läuft kein Formatierer
                st = None
            except ApiError:     # es antwortet ein Formatierer (z. B. "zu viele Anfragen"): als laufend behandeln
                st = {"busy": False}
            except ConnectionError:
                # keine Antwort: ob dort einer läuft, ist unklar - ohne ihn zu beenden, startete die neue Fassung nicht
                self.q.put(("elf_ask", host, payload, "unclear", prev_status, ask))
                return
            self.q.put(("elf_ask", host, payload, st, prev_status, ask))

        self._run(check)

    def _elf_ask(self, host, payload, st, prev_status="", ask=True):
        self.status_var.set(prev_status)
        size = fmt_size(os.path.getsize(payload))
        if st == "unclear":
            messagebox.showerror(self.title(), _("{host} antwortet nicht (Port {port}). Ist die PS5 eingeschaltet und die "
                                                 "IP-Adresse richtig?", host=host, port=API_PORT), parent=self)
            return
        if st and st.get("busy"):
            messagebox.showerror(self.title(), _("Auf {host} läuft gerade ein Auftrag. Die ELF erst danach senden.",
                                                 host=host), parent=self)
            return
        running = st is not None
        if running:
            text = _("Auf {host} läuft bereits ein Formatierer. Er wird beendet und durch die in dieses Programm "
                     "eingebaute Fassung ersetzt (usb_format.elf, {size}).", host=host, size=size)
        else:
            text = _("Die in dieses Programm eingebaute usb_format.elf ({size}) an {host} senden – an den ELF-Loader der "
                     "PS5 (Port {port}). Danach startet dort der Formatierer.", host=host, size=size, port=LOADER_PORT)
        if ask and not confirm(self, _("ELF senden"), text, _("Ersetzen") if running else _("Senden")):
            return
        api = Api(host)

        def work():
            self.elf_sending = True   # Schließen fragt nach: sonst bliebe die PS5 ohne Formatierer bzw. mit halber ELF
            try:
                send()
            finally:
                self.elf_sending = False

        def send():
            if running:
                self.q.put(("status", _("Beende den Formatierer auf {host} …", host=host)))
                try:
                    api.quit()
                except ApiError as e:   # z. B. inzwischen ein Auftrag gestartet - die Verbindung bleibt bestehen
                    self.q.put(("error", str(e)))
                    return
                except ConnectionError:
                    pass
                for _attempt in range(30):   # bis Port 8090 frei ist (der alte Formatierer ist beendet)
                    if not port_open(host, API_PORT):
                        break
                    time.sleep(0.5)
                else:
                    self.q.put(("error", _("Der Formatierer auf {host} ließ sich nicht beenden.", host=host)))
                    return
                time.sleep(1)
            elif port_open(host, API_PORT):   # inzwischen gestartet: die neue Fassung bekäme den Port nicht
                self.q.put(("error", _("Auf {host} wurde inzwischen ein Formatierer gestartet. Bitte erneut „ELF "
                                       "senden“.", host=host)))
                return
            self.q.put(("use_host", host))   # erst jetzt: die alte Verbindung ist weg bzw. war nie da
            self.q.put(("status", _("Sende usb_format.elf an {host} (Port {port}) …", host=host, port=LOADER_PORT)))
            try:
                send_payload(host, payload)
            except OSError as e:
                self.q.put(("error", loader_error(host, e, stopped=running)))
                return
            for attempt in range(30):
                self.q.put(("scan", attempt * 100 // 30, _("Warte, bis der Formatierer auf der PS5 antwortet …")))
                time.sleep(0.5)
                try:
                    devs = api.devices(timeout=2)
                except (ConnectionError, ApiError):
                    continue
                self.q.put(("connected", api, devs))
                self.q.put(("status", _("ELF gesendet – der Formatierer läuft. Verbunden mit {host}", host=host)))
                return
            self.q.put(("error", _("Der Payload wurde gesendet, antwortet aber nicht. "
                                   "Bitte die PS5-Benachrichtigungen prüfen.")))

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
                self.q.put(("error", _("Geräteliste konnte nicht geladen werden: {e}", e=e)))

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
        fs_text = _("exFAT mit 64 KB Zuordnungseinheiten") if fs == "exfat" else "FAT32"
        replug = bool(dev["mounts"])
        replug_text = _("Die PS5 nutzt diesen Datenträger gerade. Der Formatierer meldet ihn per Software kurz ab "
                        "und übernimmt ihn, bevor die PS5 ihn wieder einbindet. Klappt das nicht, bittet er, den "
                        "Stick an der PS5 abzuziehen und wieder einzustecken. Spiele und Apps, die davon laufen, "
                        "vorher beenden.") + "\n\n" if replug else ""
        limit = dev.get("fake_real") or 0
        if limit:
            replug_text = _("GEFÄLSCHTER STICK: Er meldet {size}, hat aber nur {real} echten Speicher. Es wird nur "
                            "dieser Bereich formatiert. Für Spiele an der PS5 besser einen echten Datenträger "
                            "verwenden.", size=fmt_size(dev["size"]), real=fmt_size(limit)) + "\n\n" + replug_text
        games = sorted(g["name"] or g["id"] for g in self.games.get(dev["name"], []))
        ack = None
        if games:
            shown = "\n".join("• " + g for g in games[:8])
            if len(games) > 8:
                shown += "\n" + _("… und {n} weitere", n=len(games) - 8)
            head = _("Darauf liegt 1 Spiel, das gelöscht wird:") if len(games) == 1 else \
                _("Darauf liegen {n} Spiele, die gelöscht werden:", n=len(games))
            replug_text = f"{head}\n{shown}\n\n" + replug_text
            ack = _("Ich weiß, dass diese Spiele unwiderruflich gelöscht werden")
        elif dev["mounts"] and (self.lib_loading or not self.lib_known):
            # Inhalt noch unbekannt: lieber ausdrücklich bestätigen lassen
            replug_text = _("Der Inhalt wird noch durchsucht – möglicherweise liegen Spiele darauf.") + "\n\n" + \
                replug_text
            ack = _("Ich weiß, dass alles darauf unwiderruflich gelöscht wird")
        verify = bool(self.verify_var.get())
        if verify:
            replug_text += _("Vorher werden Echtheit und Tempo geprüft (einige Minuten).") + "\n\n"
            limit = 0   # die Prüfung testet den ganzen Stick erneut
        if not self._confirm(
                _("Alle Daten werden gelöscht"),
                _("{name} ({dev}, {size}) wird als {fs} formatiert.", name=dev_title(dev), dev=dev["name"],
                  size=fmt_size(dev["size"]), fs=fs_text) + "\n\n" + replug_text +
                _("Das lässt sich nicht rückgängig machen."),
                _("Formatieren"), ack):
            return
        if self.busy or not self.api:   # während der Rückfrage kann ein anderer Auftrag begonnen haben
            return
        self.settings.update(label=label, verify=verify, folders=self.folders_var.get())
        save_settings(self.settings)
        folders = bool(self.folders_var.get())

        def after(ok, msg, result):
            """eigenes Formatieren: bei gefälschtem Stick anbieten, nur den echten Bereich zu nutzen"""
            if ok:
                messagebox.showinfo(_("PS5-USB formatieren"), _("Datenträger erfolgreich formatiert.") + "\n\n" + msg,
                                    parent=self)
            elif result.get("fake") and result.get("real", 0) >= 64 * 1024 * 1024:
                real = result["real"]
                if self._confirm(_("Gefälschter Speicher erkannt"),
                                 msg + "\n\n" + _("Nur die echten {size} formatieren? Für Spiele an der PS5 besser "
                                                  "einen echten Datenträger verwenden.", size=fmt_size(real)),
                                 _("Nur echte {size} formatieren", size=fmt_size(real))):
                    self._ps5_format_job(dev, fs, label, folders, False, real, after)
            else:
                messagebox.showerror(_("PS5-USB formatieren"), msg, parent=self)

        self._ps5_format_job(dev, fs, label, folders, verify, limit, after)

    def _ps5_format_job(self, dev, fs, label, folders, verify, limit, after):
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                api.format(dev["name"], fs, label, dev["size"], bool(dev["mounts"]), limit, folders, verify,
                           dev.get("ident", ""))
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Formatieren konnte nicht gestartet werden: {e}", e=e)))
                return
            self._follow(api, foreign=False, after=after)

        self.pbar["value"] = 0
        self._run(work)

    def _delete_games(self):
        dev = self._selected_device()
        all_keys = {g.get("key") for g in self.games.get(dev["name"], [])} if dev else set()
        keys = [k for k in self.games_checked if k in all_keys]   # auch angehakte, die die Suche gerade ausblendet
        if self.busy or not dev or not keys or not self.api:
            return
        games = {g.get("key"): g for g in self.games.get(dev["name"], [])}
        names = sorted((games[k]["name"] or games[k]["id"]) for k in keys if k in games)
        if not names:
            return
        uninstall = self.uninstall_var.get()
        shown = "\n".join("• " + n for n in names[:10])
        if len(names) > 10:
            shown += "\n" + _("… und {n} weitere", n=len(names) - 10)
        head = _("Dieses Spiel wird von {name} ({dev}) gelöscht:", name=dev_title(dev), dev=dev["name"]) \
            if len(names) == 1 else _("Diese {n} Spiele werden von {name} ({dev}) gelöscht:", n=len(names),
                                      name=dev_title(dev), dev=dev["name"])
        tiles = _("Die Kacheln werden auch von der PS5-Startseite entfernt.") if uninstall else \
            _("Die Kacheln bleiben auf der Startseite (ohne Spiel).")
        if not messagebox.askokcancel(
                _("Spiele löschen"),
                f"{head}\n\n{shown}\n\n{tiles}\n\n" + _("Laufende Spiele vorher beenden. Das lässt sich nicht "
                                                        "rückgängig machen."),
                icon="warning", default="cancel", parent=self):
            return
        if self.busy or not self.api:
            return
        api = self.api

        def work():
            try:
                api.delete_games(keys, uninstall)
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Löschen konnte nicht gestartet werden: {e}", e=e)))
                return
            self._follow(api, foreign=False, done_title=_("Spiele gelöscht."))

        self.pbar["value"] = 0
        self._run(work)

    # ------------------------------------------------------------------ Image-Sicherung an der PS5

    def _replug_note(self, dev):
        return _("Die PS5 nutzt diesen Datenträger gerade. Der Formatierer meldet ihn dafür kurz per Software ab; "
                 "läuft ein Spiel oder eine App, lehnt er ab – dann erst beenden.") + "\n\n" if dev["mounts"] else ""

    def _loc_text(self, loc):
        if not loc.get("device"):
            where = _("intern auf der PS5")
        else:
            d = next((x for x in self.ps5_devices if x["name"] == loc["device"]), None)
            where = (f"{dev_title(d)} ({loc['device']})" if d else loc["device"]) + \
                (" " + quote(loc["label"]) if loc.get("label") else "")
        free = " · " + _("{size} frei", size=fmt_size(loc["free"])) if loc.get("free") is not None else ""
        return where + free

    def _wait_claim(self, api, seq, failed):
        """Download und Hochladen beginnen den Auftrag erst mit ihrer Verbindung: warten, bis die PS5 ihn annimmt"""
        end = time.time() + 90
        while time.time() < end:
            if failed():
                return False
            try:
                if (api.status().get("job") or 0) > seq:
                    return True
            except (ConnectionError, ApiError):
                pass
            time.sleep(0.4)
        return False

    def _with_images(self, then):
        """Sicherungen und Ziele der PS5 im Hintergrund holen (bis zu 15 s, z. B. wenn dort eine Platte anläuft) - das
        Fenster bleibt bedienbar; danach then(antwort) im UI-Thread"""
        api = self.api

        def work():
            self.q.put(("status", _("Frage die PS5 nach Sicherungen …")))
            try:
                self.q.put(("images", api, api.images(), "", then))
            except (ConnectionError, ApiError) as e:
                self.q.put(("images", api, None, str(e), then))

        self._run(work)

    def _ps5_backup(self):
        dev = self._selected_device()
        if self.busy or not dev or not self.api:
            return
        self._with_images(lambda imgs: self._ps5_backup_to(dev, imgs))

    def _ps5_backup_to(self, dev, imgs):
        if self.busy or not self.api:
            return
        title = _("Image sichern")
        locs = [loc for loc in imgs.get("locations", []) if loc.get("device") != dev["name"]]
        choice = ask_choice(self, title, _("{name} ({dev}, {size}) sichern nach:", name=dev_title(dev), dev=dev["name"],
                                           size=fmt_size(dev["size"])),
                            [self._loc_text(loc) for loc in locs] + [_("auf diesen PC (Datei speichern …)")], _("Weiter"))
        if choice is None:
            return
        to_pc = choice == len(locs)
        path = None
        if to_pc:
            name = image_file_name({"model": dev.get("model", ""), "volumes": [{"label": dev.get("label", "")}]})
            path = filedialog.asksaveasfilename(parent=self, title=title, initialfile=name, defaultextension=IMAGE_EXT,
                                                initialdir=self.settings.get("image_dir") or os.path.expanduser("~"),
                                                filetypes=[(_("Image-Sicherung"), "*" + IMAGE_EXT)])
            if not path:
                return
            if not path.lower().endswith(IMAGE_EXT):
                path += IMAGE_EXT
            self.settings["image_dir"] = os.path.dirname(path)
            save_settings(self.settings)
        target = path if to_pc else self._loc_text(locs[choice])
        if not self._confirm(title, _("{name} ({dev}, {size}) wird gesichert nach:", name=dev_title(dev), dev=dev["name"],
                                      size=fmt_size(dev["size"])) + "\n   " + target + "\n\n" + self._replug_note(dev) +
                             _("Gesichert werden nur die belegten Bereiche, gepackt mit zstd; daneben entsteht eine "
                               ".sha256-Datei. Die Daten auf dem Laufwerk bleiben erhalten."), _("Sichern")):
            return
        if self.busy or not self.api:
            return
        api = self.api
        if not to_pc:
            def shown(ok, msg, result):
                (messagebox.showinfo if ok else messagebox.showerror)(title, msg, parent=self)

            def on_ps5():
                try:
                    api.backup(dev, locs[choice]["dir"])
                except (ConnectionError, ApiError) as e:
                    self.q.put(("error", _("Sichern konnte nicht gestartet werden: {e}", e=e)))
                    return
                self._follow(api, foreign=False, after=shown)

            self.pbar["value"] = 0
            self._run(on_ps5)
            return

        part = path + ".part"
        state = {}

        def download(token):
            try:
                state["bytes"], state["sha"] = api.download(token, part)
            except (ConnectionError, ApiError, OSError) as e:
                state["error"] = str(e) or type(e).__name__
            if state.get("abandon"):   # inzwischen aufgegeben: die halbe Datei nicht liegen lassen
                try:
                    os.remove(part)
                except OSError:
                    pass

        def done(ok, msg, result, deadline=None):
            t = state.get("thread")
            if t and t.is_alive():   # die letzten Bytes kommen gleich nach dem Ende des Auftrags - im Hintergrund
                deadline = deadline or time.time() + 60   # warten, das Fenster bleibt bedienbar
                if time.time() < deadline:
                    self.after(200, lambda: done(ok, msg, result, deadline))
                    return
                state["abandon"] = True
                state.setdefault("error", _("Der Download ist nicht fertig geworden – bitte erneut sichern."))
            good = ok and not state.get("error") and state.get("sha") and state.get("sha") == result.get("sha256")
            if good:
                try:
                    os.replace(part, path)
                    with open(path + ".sha256", "w", encoding="utf-8", newline="\n") as f:
                        f.write(f"{state['sha']}  {os.path.basename(path)}\n")
                except OSError as e:
                    good, msg = False, _("Die Datei lässt sich nicht speichern: {e}", e=e)
            if not good:
                try:
                    os.remove(part)
                except OSError:
                    pass
                if ok and not state.get("error"):
                    msg = _("Der Download ist unvollständig oder verfälscht angekommen – bitte erneut sichern.")
                messagebox.showerror(title, state.get("error") or msg, parent=self)
                return
            messagebox.showinfo(title, msg + "\n\n" + _("Gespeichert: {path}", path=path) + "\n" +
                                _("Die Prüfsumme stimmt (SHA-256 {sha}).", sha=state["sha"][:16] + "…"), parent=self)

        def work():
            try:
                seq = api.status().get("job") or 0
                token = api.backup(dev, "download")["token"]
            except (ConnectionError, ApiError, KeyError) as e:
                self.q.put(("error", _("Sichern konnte nicht gestartet werden: {e}", e=e)))
                return
            t = threading.Thread(target=download, args=(token,), daemon=True)
            state["thread"] = self.transfer = t
            t.start()
            if not self._wait_claim(api, seq, lambda: "error" in state):
                state["abandon"] = True   # hängt der Download noch, räumt er beim Ende selbst auf
                t.join(5)
                try:
                    os.remove(part)
                except OSError:
                    pass
                self.q.put(("error", state.get("error") or _("Der Download hat nicht begonnen.")))
                return
            self._follow(api, foreign=False, after=done)

        self.pbar["value"] = 0
        self._run(work)

    def _ps5_restore(self):
        dev = self._selected_device()
        if self.busy or not dev or not self.api:
            return
        self._with_images(lambda imgs: self._ps5_restore_from(dev, imgs))

    def _ps5_restore_from(self, dev, imgs):
        if self.busy or not self.api:
            return
        title = _("Image wiederherstellen")
        images = [i for i in imgs.get("images", []) if i.get("device") != dev["name"]]

        def img_text(i):
            src = " ".join(x for x in (i.get("model", ""), quote(i["label"]) if i.get("label") else "") if x) or i["name"]
            where = _("intern auf der PS5") if not i.get("device") else i["device"]
            return " · ".join(x for x in (src, i.get("created", ""), fmt_size(i["size"]), where) if x)

        choice = ask_choice(self, title, _("{name} ({dev}, {size}) wiederherstellen aus:", name=dev_title(dev),
                                           dev=dev["name"], size=fmt_size(dev["size"])),
                            [img_text(i) for i in images] + [_("einer Datei auf diesem PC (hochladen) …")], _("Weiter"))
        if choice is None:
            return
        path = None
        if choice == len(images):
            path = filedialog.askopenfilename(parent=self, title=title,
                                              initialdir=self.settings.get("image_dir") or os.path.expanduser("~"),
                                              filetypes=[(_("Image-Sicherung"), "*" + IMAGE_EXT + " *.zst"),
                                                         (_("Alle Dateien"), "*")])
            if not path:
                return
            try:
                info = image_info(path)
            except OSError as e:
                messagebox.showerror(title, _("Die Datei lässt sich nicht lesen: {e}", e=e), parent=self)
                return
            if info is None:
                messagebox.showerror(title, _("Das ist keine Image-Sicherung (.img.zst)."), parent=self)
                return
            info = dict(info, name=os.path.basename(path))
        else:
            info = images[choice]
        size = info.get("size", 0)
        if not size:
            messagebox.showerror(title, _("Die Größe des Images ist unbekannt (mit zstd ohne Größenangabe gepackt)."),
                                 parent=self)
            return
        if size > dev["size"] or (dev.get("fake_real") and size > dev["fake_real"]):
            messagebox.showerror(title, _("Das Image ({image}) ist größer als dieser Datenträger ({size}).",
                                          image=fmt_size(size), size=fmt_size(dev.get("fake_real") or dev["size"])),
                                 parent=self)
            return
        lines = [_("Datei: {name}", name=info.get("name", ""))]
        src = " ".join(x for x in (info.get("model", ""), quote(info["label"]) if info.get("label") else "") if x)
        if src:
            lines.append(_("Gesichert von: {src}", src=src))
        if info.get("created"):
            lines.append(_("Stand: {date}", date=info["created"]))
        lines.append(_("Größe: {size}", size=fmt_size(size)))
        if "created" not in info:
            lines.append(_("Fremdes Image: Es wird vollständig geschrieben."))
        games = sorted(g["name"] or g["id"] for g in self.games.get(dev["name"], []))
        warn = (_("Darauf liegen {n} Spiele, die überschrieben werden.", n=len(games)) + "\n\n") if games else ""
        if not self._confirm(_("Alle Daten werden überschrieben"),
                             _("{name} ({dev}, {size}) wird mit dem Image überschrieben.", name=dev_title(dev),
                               dev=dev["name"], size=fmt_size(dev["size"])) + "\n\n   " + "\n   ".join(lines) + "\n\n" +
                             warn + self._replug_note(dev) +
                             (_("Die Datei wird dabei auf die PS5 hochgeladen.") + "\n\n" if path else "") +
                             _("Das lässt sich nicht rückgängig machen."), _("Wiederherstellen"),
                             _("Ich weiß, dass alles auf dem Laufwerk unwiderruflich überschrieben wird")):
            return
        if self.busy or not self.api:
            return
        api = self.api

        def done(ok, msg, result):
            (messagebox.showinfo if ok else messagebox.showerror)(title, msg, parent=self)

        if not path:
            def work():
                try:
                    api.restore(dev, info["path"])
                except (ConnectionError, ApiError) as e:
                    self.q.put(("error", _("Wiederherstellen konnte nicht gestartet werden: {e}", e=e)))
                    return
                self._follow(api, foreign=False, after=done)

            self.pbar["value"] = 0
            self._run(work)
            return

        state = {}

        def upload():
            try:
                api.upload(dev, path)
            except (ConnectionError, ApiError, OSError) as e:
                state["error"] = str(e) or type(e).__name__

        def work():
            try:
                seq = api.status().get("job") or 0
            except (ConnectionError, ApiError) as e:
                self.q.put(("error", _("Wiederherstellen konnte nicht gestartet werden: {e}", e=e)))
                return
            t = threading.Thread(target=upload, daemon=True)
            self.transfer = t
            t.start()
            if not self._wait_claim(api, seq, lambda: "error" in state):
                t.join(5)
                self.q.put(("error", state.get("error") or _("Das Hochladen hat nicht begonnen.")))
                return
            self._follow(api, foreign=False, after=done)

        self.pbar["value"] = 0
        self._run(work)

    def _follow(self, api, foreign, done_title=None, after=None):
        """Auftrag auf der PS5 bis zum Ende verfolgen (im Thread). foreign: woanders gestartet (PS5-Browser,
        anderer PC) - dann nur anzeigen, ohne Rückfrage-Dialoge des Starters."""
        done_title = done_title or _("Datenträger erfolgreich formatiert.")
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
                    self.q.put(("finished", False, _("Keine Verbindung zur PS5 seit 3 Minuten. Der Auftrag läuft dort "
                                                     "vermutlich weiter – den Stick NICHT abziehen. Später „Suchen“ "
                                                     "klicken, um das Ergebnis zu sehen."), foreign, done_title, {},
                                after))
                    return
                if offline >= 5:
                    self.q.put(("status", _("Verbindung zur PS5 unterbrochen – der Auftrag läuft dort weiter, frage "
                                            "weiter nach …")))
                time.sleep(min(2.0, offline / 10))   # sanft weiterfragen
                continue
            if not st.get("busy") and not st.get("done"):
                # Formatierer wurde zwischendurch neu gestartet: Ergebnis unbekannt
                self.q.put(("finished", False, _("Der Formatierer auf der PS5 wurde zwischendurch neu gestartet – das "
                                                 "Ergebnis ist unbekannt. Bitte den Datenträger prüfen bzw. erneut "
                                                 "formatieren."), foreign, done_title, {}, after))
                return
            flags = {k: st[k] for k in ("replug", "cancellable", "detail") if k in st}
            self.q.put(("progress", int(st.get("percent") or 0), st.get("step") or "", flags))
            if not st.get("busy"):
                try:   # erst die Liste, dann das Ergebnis - sonst überschreibt der Listenhinweis die Meldung
                    self.q.put(("devices", api.devices()))
                except (ConnectionError, ApiError):
                    pass
                result = st.get("result") if isinstance(st.get("result"), dict) else {}
                self.q.put(("finished", bool(st.get("ok")), st.get("message") or "", foreign, done_title, result,
                            after))
                return

    def _settle(self, api):
        """Nach einem Auftrag bindet die PS5 den Stick erst nach einigen Sekunden wieder ein - die Liste dann noch
        ein paar Mal leise holen, damit sie ohne "Aktualisieren" stimmt (eingebunden, Spiele, Belegung)"""
        for i in self.settle_ids:
            self.after_cancel(i)
        self.settle_ids = [self.after(ms, lambda a=api: self._quiet_devices(a)) for ms in (4000, 10000, 20000)]

    def _quiet_devices(self, api):
        if api is not self.api or self.busy or self.formatting or self.mode_var.get() != "ps5":
            return

        def work():
            try:
                data = api.devices()
            except (ConnectionError, ApiError):
                return
            self.q.put(("devices_quiet", api, data))

        threading.Thread(target=self._guard(work, background=True), daemon=True).start()

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
            self.q.put(("scan", 0, _("Suche USB-Datenträger am PC …")))
            try:
                self.q.put(("pcdevices", pc_list(lambda pct, text: self.q.put(("scan", pct, text)))))
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as e:
                self.q.put(("error", _("USB-Datenträger konnten nicht gelesen werden: {e}", e=e)))

        self._run(work)

    def _pc_run(self, op, args, cancellable=False, after=None):
        """Auftrag mit Administratorrechten ausführen; after(ok, meldung, info) läuft danach im UI-Thread"""
        hwnd = hwnd_of(self)
        dev = next((d for d in self.devices if len(args) > 1 and str(d.get("disk")) == str(args[1])), None)

        def work():
            started = time.time()
            try:
                job = PcJob(args, hwnd)
            except (OSError, RuntimeError) as e:
                self.q.put(("error", str(e)))
                return
            job.cancellable = cancellable
            self.pc_job = job
            self.q.put(("formatting", False))
            ok, msg, info = job.follow(lambda pct, step: self.q.put(("progress", pct, step, {})))
            record_job(args, ok, msg, dev, started, getattr(job, "cancel_requested", False))
            self.q.put(("jobended",))   # der Helfer ist fertig - "Abbrechen" weg, auch während des Neueinlesens
            try:
                # danach neu einlesen - mit Fortschritt, schlafende Platten laufen dabei womöglich erst an
                devices = pc_list(lambda pct, text: self.q.put(("scan", pct, text)))
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError):
                devices = None
            self.q.put(("pcdone", op, ok, msg, info, devices, after))

        self.pbar["value"] = 0
        self.status_var.set(admin_wait_text())
        self._run(work)

    def _pc_format(self, dev, limit=0):
        label = self.label_var.get()
        fs = "exfat" if self.fs_var.get() == "exFAT" else "fat32"
        fs_text = _("exFAT mit 64 KB Zuordnungseinheiten") if fs == "exfat" else "FAT32"
        if not limit:
            vols = pc_volumes_text(dev)
            used = pc_used(dev)
            has_data = used > 64 * 1024 * 1024
            data_warn = "\n\n" + _("Auf dem Datenträger liegen {size} Daten – sie sind danach weg.",
                                   size=fmt_size(used)) if has_data else ""
            head = _("{model} (Datenträger {disk}, {size})", model=dev["model"] or _("USB-Datenträger"),
                     disk=disk_name(dev), size=fmt_size(dev["size"]))
            if not self._confirm(
                    _("Alle Daten werden gelöscht"),
                    f"{head}\n{vols + chr(10) if vols else ''}\n" +
                    _("wird als {fs} für die PS5 formatiert.", fs=fs_text) + data_warn + "\n\n" +
                    irreversible_admin_text(),
                    _("Formatieren"),
                    _("Ich weiß, dass die {size} Daten unwiderruflich gelöscht werden", size=fmt_size(used))
                    if has_data else None):
                return
        if self.busy or self.mode_var.get() != "pc":
            return
        self.settings.update(label=label, verify=self.verify_var.get(), folders=self.folders_var.get())
        save_settings(self.settings)
        args = ["format", str(dev["disk"]), str(dev["size"]), fs, label, *serial_args(dev)]
        if self.verify_var.get() and not limit:
            args.append("verify")
        if self.folders_var.get():
            args.append("folders")
        if limit:
            args.append(f"limit={limit}")

        def after(ok, msg, info):
            if ok:
                messagebox.showinfo(_("USB-Datenträger am PC"), _("Datenträger erfolgreich formatiert.") + "\n\n" + msg,
                                    parent=self)
            elif info.get("fake") and info.get("real", 0) >= 64 * 1024 * 1024:
                real = info["real"] // (1024 * 1024) * 1024 * 1024
                if messagebox.askyesno(
                        _("Gefälschte Größe"), msg + "\n\n" + _(
                            "Nur den echten Bereich ({size}) formatieren? Dann gehen keine Daten verloren, die über "
                            "diese Grenze hinaus geschrieben würden.", size=fmt_size(real)),
                        icon="warning", parent=self):
                    self._pc_format(dev, limit=real)
            else:
                messagebox.showerror(_("USB-Datenträger am PC"), msg, parent=self)

        self._pc_run("format", args, cancellable=bool(self.verify_var.get()) and not limit, after=after)

    def _pc_speed(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Tempo messen"), msg, parent=self)

        self._pc_run("speed", ["speed", str(dev["disk"]), str(dev["size"]), *serial_args(dev)], cancellable=True,
                     after=after)

    def _pc_rename(self):
        dev = self._selected_device()
        if self.busy or not dev:
            return
        current = next((v["label"] for v in dev["volumes"] if v["fs"] in ("exFAT", "FAT32")), "")
        label = ask_string(self, _("Umbenennen"), _("Neue Bezeichnung (bis zu 11 Zeichen: A–Z, 0–9, Leerzeichen, - "
                                                    "und _; leer = Bezeichnung entfernen):"), current)
        if label is None or label == current:
            return
        if not LABEL_RE.match(label):
            messagebox.showerror(_("Umbenennen"), _("Nur A–Z, 0–9, Leerzeichen, - und _ erlaubt (höchstens {n} "
                                                    "Zeichen).", n=11), parent=self)
            return
        target = quote(label) if label else _("keine Bezeichnung (wird entfernt)")
        if not self._confirm(
                _("Umbenennen"), pc_dev_text(dev) + "\n\n" + _("Neue Bezeichnung: {label}", label=target) + "\n\n" +
                _("Die Daten bleiben erhalten. Das Laufwerk wird dafür kurz abgemeldet – geöffnete Dateien darauf "
                  "vorher schließen."),
                _("Umbenennen") if label else _("Entfernen")):
            return
        if self.busy or self.mode_var.get() != "pc":
            return

        def after(ok, msg, info):
            (messagebox.showinfo if ok else messagebox.showerror)(_("Umbenennen"), msg, parent=self)

        self._pc_run("label", ["label", str(dev["disk"]), str(dev["size"]), label, *serial_args(dev)], after=after)

    def _cancel(self):
        """Abbrechen: am PC über die Abbruch-Datei des Helfers, an der PS5 über /api/cancel"""
        self.cancel_btn.state(["disabled"])
        self.status_var.set(_("Wird abgebrochen …"))
        if self.pc_job:
            self.pc_job.cancel()
        elif self.api:
            api = self.api

            def work():
                try:
                    api.cancel()
                except (ConnectionError, ApiError) as e:
                    self.q.put(("bgerror", _("Abbrechen nicht möglich: {e}", e=e)))

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
                    self.status_var.set(_("Unerwarteter Fehler: {e}", e=e))
                self._update()
        finally:
            self._poll_id = self.after(50, self._poll)

    def _handle(self, msg):
        kind = msg[0]
        if kind == "history":
            self._show_history(msg[1], msg[2])
            return
        if kind == "status":
            self.status_var.set(msg[1])
        elif kind == "scan":
            show_scan(self.pbar, self.status_var, msg[1], msg[2])
        elif kind == "found":
            self.busy = False
            self._scan_finished()
            self._on_found(msg[1])
        elif kind == "jobended":
            self.pc_job = None
            self.formatting = False
        elif kind == "images":
            _k, api, data, err, then = msg
            self.busy = False
            if self.api:
                self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")))
            if api is not self.api or self.mode_var.get() != "ps5":
                return   # inzwischen andere PS5 oder anderer Modus
            if data is None:
                messagebox.showerror(_("Image-Sicherung"), _("Die PS5 antwortet nicht: {e}", e=err), parent=self)
                return
            then(data)
        elif kind == "elf_ask":
            self.busy = False
            self._elf_ask(*msg[1:])
        elif kind == "use_host":
            self._use_host(msg[1])
        elif kind == "connected":
            self.api = msg[1]
            self.busy = False
            self._scan_finished()
            self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")))
            self._fill_devices(msg[2].get("devices") or [])
            self._load_library()
            self._check_version(msg[1])
        elif kind == "ps5version":
            if msg[1] is self.api:
                self._version_notice(msg[2])
        elif kind == "devices":
            self.busy = self.formatting
            hist = msg[1].get("history")
            self.ps5_history = [e for e in hist if isinstance(e, dict)] if isinstance(hist, list) else []
            self._fill_devices(msg[1].get("devices") or [])
            self._load_library()
        elif kind == "imglist":
            if msg[1] is self.api and msg[2] is not None:
                self._fill_img_list(msg[2])
        elif kind == "imgdeleted":
            self.busy = False
            if msg[3]:
                messagebox.showerror(_("Image löschen"), msg[3], parent=self)
            else:
                self.status_var.set(_("{name} gelöscht.", name=msg[2]))
            if msg[1] is self.api:
                self._load_img_list()
            self._update()
        elif kind == "saves_loaded":
            self._fill_saves(msg[1], msg[2])
        elif kind == "saves_loaded_err":
            self.saves_hint.configure(text=msg[1])
        elif kind == "saves_msg":
            self.busy = False
            if msg[1]:
                self.status_var.set(msg[2])
            else:
                self.status_var.set("")
                messagebox.showerror(_("Spielstände"), msg[2], parent=self)
            self._update()
        elif kind == "devices_quiet":
            # leise nachgeladen (nach einem Auftrag): nur übernehmen, wenn inzwischen nichts anderes läuft
            if msg[1] is self.api and not self.busy and not self.formatting and self.mode_var.get() == "ps5":
                self._fill_devices(msg[2].get("devices") or [])
                self._load_library()
                self._update()
        elif kind == "formatting":
            self.formatting = True
        elif kind == "foreign":
            # woanders gestartet: hier mitverfolgen (nur bei der PS5, die jetzt verbunden ist)
            if not self.busy and self.mode_var.get() == "ps5" and msg[1] is self.api:
                self.status_var.set(_("Auf der PS5 läuft ein Auftrag …"))
                self.pbar["value"] = 0
                api = msg[1]
                self._run(lambda: self._follow(api, foreign=True))
        elif kind == "progress":
            pct, step = msg[1], msg[2]
            flags = msg[3] if len(msg) > 3 else {}
            # Schritte ohne Prozentangabe (Warten auf das Abmelden, Werkzeuge): Lauf-Animation statt Stillstand
            moving = is_replug_step(step, flags) or (pct == 0 and step not in ("", "Fertig", _("Fertig")))
            if moving and str(self.pbar["mode"]) != "indeterminate":
                self.pbar.configure(mode="indeterminate")
                self.pbar.start(12)
            elif not moving:
                if str(self.pbar["mode"]) != "determinate":
                    self.pbar.stop()
                    self.pbar.configure(mode="determinate")
                self.pbar["value"] = pct
            detail = flags.get("detail") or ""
            self.status_var.set(f"{step} …" if moving else f"{step} … {pct} %" + (f" · {detail}" if detail else ""))
            self.ps5_step = step   # für "Abbrechen" (Warten auf Abziehen, Prüfung, Messung)
            self.ps5_flags = flags
        elif kind == "finished":
            self.formatting = False
            self.busy = False
            self.pbar.stop()
            self.pbar.configure(mode="determinate")
            self.status_var.set(msg[2])
            foreign = len(msg) > 3 and msg[3]
            done_title = msg[4] if len(msg) > 4 else _("Datenträger erfolgreich formatiert.")
            result = msg[5] if len(msg) > 5 else {}
            after = msg[6] if len(msg) > 6 else None
            self.ps5_step = ""
            self.ps5_flags = {}
            if self.api and self.mode_var.get() == "ps5":
                self._settle(self.api)
                if self.page == "images":
                    self._load_img_list()
            if after and not foreign:   # der Auftraggeber entscheidet selbst (z. B. Fälschung: nur echten Bereich)
                self.pbar["value"] = 100 if msg[1] else 0
                self._update()
                after(msg[1], msg[2], result)
            elif msg[1]:
                self.pbar["value"] = 100
                if not foreign:
                    messagebox.showinfo(_("PS5-USB formatieren"), done_title + "\n\n" + msg[2], parent=self)
            else:
                self.pbar["value"] = 0
                if not foreign:
                    messagebox.showerror(_("PS5-USB formatieren"), msg[2], parent=self)
        elif kind == "library" and len(msg) > 2 and msg[2] is not self.api:
            pass   # Antwort einer vorher verbundenen PS5: verwerfen
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
                    self._library_progress(data.get("progress"))
                    self.after(1000 if data.get("progress") else 2000, lambda a=self.api: self._relibrary(a))
                elif self.lib_bar:
                    self.lib_bar = False
                    if not self.busy and self.mode_var.get() == "ps5":
                        self.status_var.set(_("Verbunden mit {host}", host=self.settings.get("host", "")))
                        self._scan_finished()
            if self.mode_var.get() == "ps5" and self.ps5_devices:
                self.dev_combo.refresh([self._ps5_drive_item(d) for d in self.ps5_devices])
            self._show_games()
        elif kind == "pcdevices":
            self.busy = False
            self._scan_finished()
            if self.mode_var.get() == "pc":
                self._fill_pc_devices(msg[1])
        elif kind == "pcdone":
            _k, op, ok, text, info, devices, after = msg
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
            messagebox.showerror(_("PS5-USB formatieren"), msg[1], parent=self)
        elif kind == "bgerror":   # Hintergrund (Spieleliste, Wächter): laufende Aufträge nicht berühren
            self.status_var.set(msg[1])

    def _check_version(self, api):
        """Version des Formatierers auf der PS5 im Hintergrund holen (ab 2.5.3 in /api/info)"""
        def work():
            try:
                info = api.info()
            except (ConnectionError, ApiError):
                return
            self.q.put(("ps5version", api, str(info.get("version") or "") if isinstance(info, dict) else ""))

        threading.Thread(target=self._guard(work, background=True), daemon=True).start()

    def _version_notice(self, ps5):
        """Läuft auf der PS5 eine andere Fassung als die eingebaute? Ältere: anbieten, die neue zu senden (einmal je PS5
        und Version); neuere: Hinweis, dieses Programm zu aktualisieren"""
        app, mine, theirs = APP_VERSION, version_tuple(APP_VERSION), version_tuple(ps5)
        if not mine or theirs == mine or self.busy or self.formatting or self.mode_var.get() != "ps5":
            return
        key = (self.settings.get("host", ""), ps5)
        if key in self.version_asked:
            return
        self.version_asked.add(key)
        if theirs and theirs > mine:
            self.status_var.set(_("Auf der PS5 läuft eine neuere Fassung des Formatierers ({ps5}) als in diesem "
                                  "Programm ({app}). Bitte das Programm aktualisieren.", ps5=ps5, app=app))
            return
        if confirm(self, _("Neuere Fassung verfügbar"),
                   _("Auf der PS5 läuft der Formatierer {ps5}, dieses Programm bringt {app} mit. Jetzt die neue Fassung "
                     "an die PS5 senden? Der laufende Formatierer wird dabei beendet und ersetzt.",
                     ps5=ps5 or _("in einer älteren Fassung"), app=app), _("Senden")):
            self._send_elf(ask=False)

    def _scan_finished(self):
        """Suche/Einlesen fertig: Balken kurz voll, dann leer - solange kein Auftrag läuft"""
        self.pbar.stop()
        self.pbar.configure(mode="determinate")
        self.pbar["value"] = 100
        if self._bar_reset:
            self.after_cancel(self._bar_reset)
        self._bar_reset = self.after(800, self._clear_bar)

    def _clear_bar(self):
        self._bar_reset = None
        if not self.busy and not self.formatting:
            self.pbar.configure(value=0)

    def _library_progress(self, prog):
        """Spielesuche der PS5 (ab 2.5.6: durchsuchte Laufwerke von allen) im Balken - nur, wenn sonst nichts läuft"""
        if not isinstance(prog, dict) or self.busy or self.formatting or self.mode_var.get() != "ps5":
            return
        total, done = int(prog.get("total") or 0), int(prog.get("done") or 0)
        if total <= 0:
            return
        root = str(prog.get("root") or "")
        text = _("Spiele auf der PS5 werden gesucht: {where} …", where=root) if root else \
            _("Spiele auf der PS5 werden gesucht …")
        self.lib_bar = True
        show_scan(self.pbar, self.status_var, min(99, 100 * done // total), text)

    def _on_close(self):
        if self.elf_sending and not messagebox.askyesno(
                self.title(), _("Die ELF wird gerade an die PS5 gesendet. Wird das Fenster jetzt geschlossen, läuft dort "
                                "danach womöglich kein Formatierer. Fenster trotzdem schließen?"),
                icon="warning", parent=self):
            return
        transfer = self.transfer is not None and self.transfer.is_alive()
        if transfer and not messagebox.askyesno(
                self.title(), _("Die Übertragung zwischen diesem PC und der PS5 läuft noch und bricht ab, wenn das "
                                "Fenster geschlossen wird. Beim Wiederherstellen bleibt der Datenträger dann "
                                "unvollständig beschrieben. Fenster trotzdem schließen?"),
                icon="warning", parent=self):
            return
        if self.formatting and not transfer and not messagebox.askyesno(
                self.title(), (_("Der Auftrag läuft am PC im Hintergrund weiter.") if self.pc_job else
                               _("Die Formatierung läuft auf der PS5 weiter.")) + " " +
                _("Fenster trotzdem schließen?"),
                icon="warning", parent=self):
            return
        if not self.std_panel.may_close():   # Auftrag in "Am PC – frei" (fragt selbst nach)
            return
        if self.pc_job and getattr(self.pc_job, "cancellable", False):
            self.pc_job.cancel()   # Prüfung/Tempo am PC nicht ohne Fenster weiterlaufen lassen
        self.after_cancel(self._poll_id)
        self.after_cancel(self._watch_id)
        self.after_cancel(self._sys_id)
        for i in self.settle_ids:
            self.after_cancel(i)
        if self._bar_reset:
            self.after_cancel(self._bar_reset)
        self.destroy()

    def _on_exception(self, exc, val, tb):
        messagebox.showerror(self.title(), _("Unerwarteter Fehler: {e}", e=val), parent=self)


if __name__ == "__main__":
    enable_dpi_awareness()
    enable_history(APP_NAME)
    App().mainloop()
