"""Linux-Fassung der Programme ohne echte Datenträger und ohne Root: Oberfläche mit gestellter Laufwerksliste (Texte,
/dev-Namen, keine Windows-Hinweise), Einstellungsordner, und der echte Startweg über "pkexec" - ein Ersatz-pkexec
führt den Befehl als normaler Benutzer aus, der Helfer (Testfassung) bearbeitet eine Abbild-Datei. Geprüft werden
Kopieren und Prüfsumme im Startskript, Ausgabedatei, Fortschritt und Ergebnis, Abbrechen und ein veränderter Helfer.
Aufruf (Linux, mit Anzeige oder xvfb-run): python3 tests/linux_ui_test.py <helfer-testfassung> [bildordner]"""
import hashlib
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
HELPER = os.path.abspath(sys.argv[1])
SHOTS = sys.argv[2] if len(sys.argv) > 2 else None

import i18n  # noqa: E402
import pc_common  # noqa: E402
import pc_linux  # noqa: E402

ok_count = bad_count = 0


def check(name, cond, extra=""):
    global ok_count, bad_count
    ok_count += bool(cond)
    bad_count += not cond
    print(("OK      " if cond else "FEHLER  ") + name + ("" if cond else f"  [{extra}]"))


check("Plattform: Linux erkannt", not pc_common.IS_WIN and pc_common.HELPER_NAME == "usbfmt_linux")
# PS5-Suche hinter einem NAT (WSL, VM): Stufe 3 fragt die üblichen Heimnetze, aber nicht das schon abgesuchte eigene
import ipaddress  # noqa: E402
import ps5_discovery  # noqa: E402
nat = ps5_discovery.home_hosts([ipaddress.IPv4Interface("172.29.158.207/20")])
lan = ps5_discovery.home_hosts([ipaddress.IPv4Interface("192.168.178.20/24")])
check("PS5-Suche hinter NAT: Heimnetze werden gefragt", "192.168.178.40" in nat and "192.168.1.94" in nat, len(nat))
check("PS5-Suche: eigenes Netz nicht doppelt", "192.168.178.40" not in lan and "192.168.1.94" in lan, len(lan))
check("PcJob ist der pkexec-Weg", pc_common.PcJob is pc_linux.LinuxJob)
os.environ["XDG_CONFIG_HOME"] = tempfile.mkdtemp()
os.environ["USBFMT_NO_UPDATE_CHECK"] = "1"   # kein Abruf bei GitHub (Bilder und Ergebnis unabhängig vom Netz)
check("Einstellungen unter XDG_CONFIG_HOME", pc_common.settings_file("X").startswith(os.environ["XDG_CONFIG_HOME"]))
i18n.set_lang("de")
check("Texte zur Rechte-Abfrage ohne Windows", "Windows" not in pc_common.admin_wait_text() + pc_common.irreversible_admin_text()
      + pc_common.data_kept_admin_text())

GB = 1024 ** 3
DEVICES = [
    {"disk": "sdb", "size": 61530439680, "ss": 512, "removable": True, "model": "SanDisk Ultra",
     "volumes": [{"letters": "/media/user/SANDISK", "device": "/dev/sdb1", "fs": "exFAT", "label": "SANDISK",
                  "total": 61 * 10 ** 9, "free": 48 * 10 ** 9}]},
    {"disk": "sdc", "size": 1000204886016, "ss": 512, "removable": False, "model": "Samsung PSSD T7",
     "volumes": [{"letters": "", "device": "/dev/sdc1", "fs": "ext4", "label": "daten", "total": 0, "free": 0}]},
    {"disk": "sdd", "size": 16 * GB, "ss": 512, "removable": True, "model": "", "volumes": []},
]
t0 = pc_common.pc_dev_text(DEVICES[0])
check("Laufwerkstext mit /dev/sdb und Einhängepunkt", "/dev/sdb" in t0 and "/media/user/SANDISK" in t0, t0)
t1 = pc_common.pc_dev_text(DEVICES[1])
check("nicht eingehängt: Gerät statt Buchstabe", "/dev/sdc1" in t1, t1)
t2 = pc_common.pc_dev_text(DEVICES[2])
check("leer: kein Windows-Hinweis", "Windows" not in t2 and "leer" in t2, t2)

# ------------------------------------------------------------------ Startweg über (Ersatz-)pkexec
work = tempfile.mkdtemp()
fake = os.path.join(work, "pkexec")
with open(fake, "w") as f:
    f.write('#!/bin/sh\nexec "$@"\n')   # "Abfrage bestanden": Befehl als Benutzer ausführen
os.chmod(fake, 0o755)
pc_linux.pkexec_path = lambda: fake
digest = hashlib.sha256(open(HELPER, "rb").read()).hexdigest()
img = os.path.join(work, "stick.img")
size = 512 * 1024 * 1024
with open(img, "wb") as f:
    f.truncate(size)
steps = []
job = pc_linux.LinuxJob(["--image", img, "format", "image", str(size), "exfat", "LINUX", "plain"], helper=HELPER,
                        digest=digest)
res = job.follow(lambda pct, step: steps.append((pct, step)))
check("Formatieren über den Startweg: Erfolg", res[0], res)
check("Fortschritt kam an", len(steps) >= 3 and steps[-1][0] == 100, steps[-3:])
check("Arbeitsordner aufgeräumt", not os.path.exists(job.tmpdir))
data = open(img, "rb").read(2 * 1024 * 1024)
check("Abbild hat jetzt eine Partitionstabelle", data[510:512] == b"\x55\xaa")

# Image sichern und zurückschreiben über denselben Weg (Dateien mit den Rechten des Aufrufers)
bak = os.path.join(work, "Sicherung Jürgen.img.zst")
res = pc_linux.LinuxJob(["--image", img, "backup", "image", str(size), bak], helper=HELPER, digest=digest).follow(
    lambda pct, step: None)
check("Image sichern über den Startweg", res[0] and os.path.getsize(bak) > 0 and os.path.exists(bak + ".sha256"), res)
info = pc_common.image_info(bak)
check("image_info liest den Kopf", info and info.get("size") == size and info.get("label") == "LINUX", info)
img2 = os.path.join(work, "ziel.img")
with open(img2, "wb") as f:
    f.write(os.urandom(1024 * 1024))
    f.truncate(size)
res = pc_linux.LinuxJob(["--image", img2, "restore", "image", str(size), bak], helper=HELPER, digest=digest).follow(
    lambda pct, step: None)
check("Image wiederherstellen über den Startweg", res[0] and "geprüft" in res[1], res)
check("wiederhergestellt: Partitionstabelle wie die Quelle", open(img2, "rb").read(512) == open(img, "rb").read(512))
check("image_info: keine zstd-Datei", pc_common.image_info(img) is None)

job = pc_linux.LinuxJob(["--image", img, "speed", "image", str(size), "plain"], helper=HELPER, digest="0" * 64)
res = job.follow(lambda pct, step: None)
check("veränderter Helfer (falsche Prüfsumme): nicht gestartet", not res[0] and "verändert" in res[1], res)

i18n.set_lang("en")
job = pc_linux.LinuxJob(["--image", img, "format", "image", str(size - 1), "exfat", "X"], helper=HELPER, digest=digest)
res = job.follow(lambda pct, step: None)
check("Meldung des Helfers in der Sprache des Programms", not res[0] and "image" in res[1].lower(), res)
i18n.set_lang("de")

job = pc_linux.LinuxJob(["--image", img, "format", "image", str(size), "exfat", "X", "full", "plain"], helper=HELPER,
                        digest=digest)
job.cancel()   # sofort, noch bevor der Helfer etwas schreibt: dann bleibt der Datenträger unverändert
t_start = time.time()
res = job.follow(lambda pct, step: None)
check("Abbrechen vor dem ersten Schreiben: nichts verändert", not res[0] and "nicht verändert" in res[1]
      and time.time() - t_start < 60, res)

denied = os.path.join(work, "pkexec-nein")
with open(denied, "w") as f:
    f.write("#!/bin/sh\nexit 126\n")   # Abfrage abgebrochen
os.chmod(denied, 0o755)
pc_linux.pkexec_path = lambda: denied
job = pc_linux.LinuxJob(["list"], helper=HELPER, digest=digest)
res = job.follow(lambda pct, step: None)
check("Abfrage abgebrochen: verständliche Meldung", not res[0] and "Administratorrechte" in res[1], res)
pc_linux.pkexec_path = lambda: None
try:
    pc_linux.LinuxJob(["list"], helper=HELPER, digest=digest)
    check("ohne pkexec: Meldung", False)
except PermissionError as e:
    check("ohne pkexec: Meldung mit Paketname", "polkit" in str(e), e)

# ------------------------------------------------------------------ Oberfläche
import standard_panel as sp  # noqa: E402
import ui_msgbox  # noqa: E402
import usb_formatierer as uf  # noqa: E402
import usb_standard  # noqa: E402

# Dialoge dürfen den Test nie anhalten (in der CI klickt niemand): unerwartete Meldungen werden notiert und am Ende
# als Fehler gezählt; die Tests, die eine Meldung erwarten, ersetzen die Funktion selbst
unexpected = []
for _name in ("showinfo", "showwarning", "showerror"):
    setattr(ui_msgbox, _name, lambda title, msg, *a, _n=_name, **k: unexpected.append((_n, title, msg)))
ui_msgbox.askyesno = lambda title, msg, *a, **k: unexpected.append(("askyesno", title, msg)) or False
ui_msgbox.askokcancel = lambda title, msg, *a, **k: unexpected.append(("askokcancel", title, msg)) or False


class NoJob:
    def __init__(self, *a, **k):
        raise RuntimeError("Test: keine Aufträge")


sp.pc_list = uf.pc_list = lambda progress=None: [dict(d, volumes=[dict(v) for v in d["volumes"]]) for d in DEVICES]
sp.PcJob = uf.PcJob = NoJob
usb_standard.load_settings = lambda name: {"lang": "de"}
usb_standard.save_settings = lambda name, data: None
uf.load_settings = lambda: {"lang": "de", "mode": "std"}
uf.save_settings = lambda data: None


def pump(app, cond=lambda: False, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        app.update()
        if cond():
            return True
        time.sleep(0.02)
    return cond()


def shot(app, name):
    if not SHOTS:
        return
    try:
        from PIL import ImageGrab
        app.update()
        x, y = app.winfo_rootx(), app.winfo_rooty()
        ImageGrab.grab(bbox=(x, y, x + app.winfo_width(), y + app.winfo_height()),
                       xdisplay=os.environ.get("DISPLAY")).save(os.path.join(SHOTS, name))
    except Exception as e:   # Bild ist Beiwerk
        print("        (kein Bild:", e, ")")


app = usb_standard.StandardApp()
p = app.panel
pump(app, lambda: len(p.devices) == len(DEVICES) and not p.busy)
check("eigenständig: Liste geladen, nichts vorausgewählt", len(p.devices) == 3 and p.dev_combo.current() == -1)
check("eigenständig: eigenes Theme (hell/dunkel)", str(app.tk.call("ttk::style", "theme", "use")) in
      ("usbfmt-light", "usbfmt-dark"), app.tk.call("ttk::style", "theme", "use"))
vals = list(p.dev_combo.cget("values"))
check("Liste zeigt /dev-Namen", all("/dev/sd" in v for v in vals), vals)
p.dev_combo.current(0)
p._device_changed()
p.fs_var.set("ext4 (Linux)")
p._refresh_options()
p._update()
pump(app, timeout=0.3)
hint = p.label_hint.cget("text") if hasattr(p, "label_hint") else ""
check("ext4: kein Windows-Hinweis zum Laufwerksbuchstaben", "Windows" not in hint, hint)
shot(app, "linux_std.png")
check("Sichern/Wiederherstellen bereit", "disabled" not in p.backup_btn.state() and "disabled" not in p.restore_btn.state())
check("Datei auf dem Stick selbst erkannt", pc_common.on_device("/media/user/SANDISK/x.img.zst", DEVICES[0])
      and not pc_common.on_device(bak, DEVICES[0]))
name = pc_common.image_file_name(DEVICES[0])
check("Vorschlag für den Dateinamen", name.startswith("SanDisk_Ultra_SANDISK_") and name.endswith(".img.zst"), name)
# Abläufe mit gestellten Dialogen: welcher Auftrag geht an den Helfer?
jobs, errors = [], []
p._job = lambda args, cancellable=False, after=None: jobs.append(args)
sp.messagebox.showerror = lambda title, msg, parent=None: errors.append(msg)
sp.confirm = lambda *a, **k: True
p._ask = lambda dev, what, ok_text=None: True
sp.filedialog.asksaveasfilename = lambda **k: "/media/user/SANDISK/sicher"
p._backup()
check("Sichern auf den Stick selbst abgelehnt", not jobs and errors and "nicht auf dem Datenträger" in errors[-1], errors)
sp.filedialog.asksaveasfilename = lambda **k: os.path.join(work, "neu")
p._backup()
check("Sichern: Auftrag mit .img.zst", jobs and jobs[-1][:3] == ["backup", "sdb", str(DEVICES[0]["size"])]
      and jobs[-1][3] == os.path.join(work, "neu.img.zst"), jobs)
sp.filedialog.askopenfilename = lambda **k: bak
p._restore()
check("Wiederherstellen: Auftrag mit der Datei", jobs[-1][:4] == ["restore", "sdb", str(DEVICES[0]["size"]), bak], jobs)
sp.filedialog.askopenfilename = lambda **k: img
n = len(jobs)
p._restore()
check("Wiederherstellen: keine Image-Datei abgelehnt", len(jobs) == n and "keine Image-Sicherung" in errors[-1], errors[-1:])
p.expert_var.set(True)
p._expert_changed()
pump(app, timeout=0.3)
shot(app, "linux_expert.png")
# Arbeitsbereich mit mehreren Monitoren (xrandr gestellt - Xvfb hat keins): Fenster auf einem Monitor bzw. auf keinem
real_monitors = pc_common._x11_monitors
try:
    pc_common._x11_monitors = lambda: [(-5000, 0, 1000, 800, False), (0, 0, 100000, 100000, False)]
    wa = pc_common.work_area(app)
    check("Arbeitsbereich: Monitor unter dem Fenster", wa == (0, 32, 100000, 100000 - 80), wa)
    pc_common._x11_monitors = lambda: [(200000, 0, 1920, 1080, False), (300000, 0, 2560, 1440, True)]
    wa = pc_common.work_area(app)
    check("Arbeitsbereich: sonst der Hauptmonitor", wa == (300000, 32, 2560, 1440 - 80), wa)
except Exception as e:
    check("Arbeitsbereich mit xrandr-Monitoren", False, repr(e))
finally:
    pc_common._x11_monitors = real_monitors
app._on_close()   # wie das Schließen-Kreuz: Zeitgeber und Abfragen beenden

app = uf.App()
pump(app, lambda: app.std_panel.devices and not app.std_panel.busy)
check("PS5-Programm startet unter Linux (Modus Standard)", app.std_panel.winfo_ismapped())
shot(app, "linux_ps5_std.png")
app.mode_var.set("pc")
app._mode_changed()
pump(app, lambda: app.devices and not app.busy)
check("Modus Am PC – für PS5: Liste", len(app.devices) == 3, len(app.devices))
shot(app, "linux_ps5_pc.png")
# Seitenleiste: Seiten der PS5 (ohne Verbindung) und zurück; dabei Laufwerke als Karten
app._nav("ps5:games")
pump(app, timeout=0.5)
check("Seitenleiste: Spiele zeigt die Spieleliste", app.mode_var.get() == "ps5" and app.games_box.winfo_ismapped()
      and not app.format_card.winfo_ismapped())
app._nav("ps5:images")
pump(app, timeout=0.3)
check("Seitenleiste: Image-Sicherung", app.img_row.winfo_ismapped() and not app.games_box.winfo_ismapped())
app._nav("pc")
pump(app, lambda: app.devices and not app.busy)
check("Seitenleiste: zurück zu Am PC", app.mode_var.get() == "pc" and app.format_card.winfo_ismapped()
      and app.tools.winfo_ismapped())
app.dev_combo.current(0)
app._nav("std")   # Wechsel mit gewähltem PC-Laufwerk: früher Fehler in formattable()
pump(app, timeout=0.5)
check("Wechsel in Am PC – frei mit gewähltem PC-Laufwerk", app.std_panel.winfo_ismapped())
# Design: Automatisch -> Hell -> Dunkel -> Automatisch, gemerkt, Oberfläche jedes Mal neu aufgebaut
seen = []
idle = lambda: not app.busy and not app.std_busy and not app.formatting  # noqa: E731
for _i in range(3):
    pump(app, idle, 60)   # während eines Auftrags lehnt das Programm den Wechsel (zu Recht) mit einer Meldung ab
    app._toggle_theme()
    pump(app, timeout=0.3)
    seen.append((app.settings.get("theme"), str(app.tk.call("ttk::style", "theme", "use"))))
check("Design Automatisch → Hell → Dunkel → Automatisch", [s for s, _t in seen] == ["light", "dark", "auto"]
      and seen[0][1] == "usbfmt-light" and seen[1][1] == "usbfmt-dark" and app.std_panel.winfo_ismapped(), seen)
pump(app, idle, 60)
app._toggle_theme()
pump(app, idle, 60)
app._toggle_theme()
pump(app, timeout=0.3)
shot(app, "linux_ps5_dark.png")
# Spieleliste: Suche, Sortierung, Summe der angehakten Spiele (gestellte Daten, ohne PS5)
fake = {"name": "da7", "model": "Teststick", "size": 64 * 10 ** 9, "locked": False, "mounts": [], "format": "exFAT",
        "label": "", "fake_real": 0, "ident": "x"}
app.mode_var.set("ps5")
app.page = "games"
app._show_page()
app.devices = app.ps5_devices = [fake]
app.games = {"da7": [{"key": f"k{i}", "name": n, "id": f"PPSA0{i}000", "kind": "Ordner", "size": s}
                     for i, (n, s) in enumerate((("Astro Bot", 54 * 10 ** 9), ("Returnal", 57 * 10 ** 9),
                                                 ("Arkanoid", 3 * 10 ** 9)))]}
app.dev_combo.set_items([app._ps5_drive_item(fake)])
app.dev_combo.current(0)
app._show_games()
check("Spieleliste: nach Name sortiert", [app.games_tree.item(k, "text").strip() for k in app.games_tree.get_children()]
      == ["Arkanoid", "Astro Bot", "Returnal"])
app.sort_combo.current(1)
app._show_games()
check("Spieleliste: nach Größe sortiert", [app.games_tree.item(k, "text").strip() for k in app.games_tree.get_children()]
      == ["Returnal", "Astro Bot", "Arkanoid"])
app.game_search.event_generate("<FocusIn>")
app.game_filter.set("astro")
pump(app, timeout=0.2)
check("Spieleliste: Suche filtert", app.games_tree.get_children() == ("k0",)
      and "1 von 3" in app.games_caption.cget("text"), (app.games_tree.get_children(), app.games_caption.cget("text")))
app._game_toggle("k0")
check("Summe der angehakten Spiele", "1" in app.games_sum.cget("text") and "GB" in app.games_sum.cget("text"),
      app.games_sum.cget("text"))
app.game_filter.set("")
app._game_toggle("k1")
check("Angehakte bleiben beim Filtern erhalten", app.games_checked == {"k0", "k1"}, app.games_checked)
# Laufwerksliste: bei vielen Laufwerken scrollbar statt endlos hoch
many = [dict(fake, name=f"da{i}", ident=str(i)) for i in range(9)]
app.dev_combo.set_items([app._ps5_drive_item(d) for d in many])
pump(app, timeout=0.5)
check("viele Laufwerke: Liste bleibt begrenzt und scrollt", app.dev_combo.canvas.winfo_height() <= app.dev_combo.max_h + 2
      and app.dev_combo.sbar.winfo_ismapped(), (app.dev_combo.canvas.winfo_height(), app.dev_combo.max_h))
app.dev_combo.current(8)
pump(app, timeout=0.3)
check("Auswahl unten wird sichtbar gescrollt", app.dev_combo.canvas.yview()[1] > 0.99, app.dev_combo.canvas.yview())
app.mode_var.set("std")
app._mode_changed()
app._on_close()
check("keine unerwarteten Dialoge", not unexpected, unexpected)
print(f"{ok_count} bestanden, {bad_count} fehlgeschlagen")
sys.exit(1 if bad_count else 0)
