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
check("PcJob ist der pkexec-Weg", pc_common.PcJob is pc_linux.LinuxJob)
os.environ["XDG_CONFIG_HOME"] = tempfile.mkdtemp()
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
import usb_formatierer as uf  # noqa: E402
import usb_standard  # noqa: E402


class NoJob:
    def __init__(self, *a, **k):
        raise RuntimeError("Test: keine Aufträge")


sp.pc_list = uf.pc_list = lambda: [dict(d, volumes=[dict(v) for v in d["volumes"]]) for d in DEVICES]
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
check("eigenständig: Stil clam", app.tk.call("ttk::style", "theme", "use") == "clam")
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
p.expert_var.set(True)
p._expert_changed()
pump(app, timeout=0.3)
shot(app, "linux_expert.png")
app.destroy()

app = uf.App()
pump(app, lambda: app.std_panel.devices and not app.std_panel.busy)
check("PS5-Programm startet unter Linux (Modus Standard)", app.std_panel.winfo_ismapped())
shot(app, "linux_ps5_std.png")
app.mode_var.set("pc")
app._mode_changed()
pump(app, lambda: app.devices and not app.busy)
check("Modus USB am PC (für PS5): Liste", len(app.devices) == 3, len(app.devices))
shot(app, "linux_ps5_pc.png")
app._on_close()
print(f"{ok_count} bestanden, {bad_count} fehlgeschlagen")
sys.exit(1 if bad_count else 0)
