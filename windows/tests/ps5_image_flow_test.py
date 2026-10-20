"""PS5-USB-Formatierer, Modus "An der PS5": Image-Sicherung gegen den Testserver des Formatierers
(usb_format/test/pc_flow_test.sh startet ihn auf 127.0.0.1:8090 mit da0 = exFAT mit Daten, eingebunden; da1 eingebunden;
da2 frei). Echtes Programmfenster, nur Auswahl-, Datei- und Rückfragedialoge sind gestellt.
Geprüft: Sichern auf den PC (Download, .sha256), Sichern auf der PS5, Hochladen, Wiederherstellen aus einer
PS5-Sicherung, Sperre bei laufendem Spiel, Ablehnen einer Datei ohne Sicherung, Versionsabgleich mit der PS5.
Aufruf: python3 tests/ps5_image_flow_test.py <sandbox-ordner des testservers>"""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
SANDBOX = sys.argv[1]
W = tempfile.mkdtemp(prefix="ps5flow-")
os.environ["XDG_CONFIG_HOME"] = os.environ["APPDATA"] = W   # nie die echten Einstellungen
os.environ["USBFMT_NO_UPDATE_CHECK"] = "1"   # kein Abruf bei GitHub

import ps5_discovery  # noqa: E402
import usb_formatierer as uf  # noqa: E402

fails = 0
msgs = []


def check(name, cond, extra=""):
    global fails
    fails += not cond
    print(("OK      " if cond else "FEHLER  ") + name + ("" if cond else f"  [{extra}]"), flush=True)


uf.load_settings = lambda: {"lang": "de", "mode": "ps5", "host": "127.0.0.1"}
uf.save_settings = lambda data: None
ps5_discovery.discover = lambda port, progress=None: [{"ip": "127.0.0.1", "name": "PS5", "standby": False, "formatter": True}]
uf.messagebox.showinfo = lambda title, msg, **k: msgs.append(("info", msg))
uf.messagebox.showerror = lambda title, msg, **k: msgs.append(("error", msg))
pick = {"save": "", "open": ""}
uf.filedialog.asksaveasfilename = lambda **k: pick["save"]
uf.filedialog.askopenfilename = lambda **k: pick["open"]
uf.App._confirm = lambda self, title, text, ok, ack=None: (pick.__setitem__("confirm", text), True)[1]


def choose(last):
    """Auswahl: erster Eintrag (auf der PS5) oder letzter (dieser PC)"""
    def ask(root, title, prompt, options, ok):
        pick["options"] = options
        return len(options) - 1 if last else 0
    uf.ask_choice = ask


def labels():
    data = json.loads(urllib.request.urlopen("http://127.0.0.1:8090/api/devices", timeout=10).read())
    return {d["name"]: d["label"] for d in data["devices"]}


app = uf.App()


def pump(cond, timeout=180):
    end = time.time() + timeout
    while time.time() < end:
        app.update()
        if cond():
            return True
        time.sleep(0.03)
    return cond()


def select(name):
    i = next(k for k, d in enumerate(app.devices) if d["name"] == name)
    app.dev_combo.current(i)
    app._update()


def run(fn):
    n = len(msgs)
    fn()
    return msgs[-1] if pump(lambda: len(msgs) > n and not app.busy) else ("timeout", "")


check("mit dem Testserver verbunden", pump(lambda: app.api is not None and len(app.devices) == 3 and not app.busy, 60),
      len(app.devices))

# Sichern auf den PC (Download) - Dateiname mit Leerzeichen
select("da0")
check("Knöpfe der Image-Sicherung bereit", "disabled" not in app.backup_btn.state() and
      "disabled" not in app.restore_btn.state())
choose(last=True)
pick["save"] = os.path.join(W, "von ps5")
kind, text = run(app._ps5_backup)
path = os.path.join(W, "von ps5.img.zst")
check("Sichern auf den PC", kind == "info" and os.path.exists(path), (kind, text))
if os.path.exists(path):
    sha = hashlib.sha256(open(path, "rb").read()).hexdigest()
    check("PC: .sha256 daneben stimmt, keine .part übrig",
          open(path + ".sha256", encoding="utf-8").read() == f"{sha}  von ps5.img.zst\n"
          and not os.path.exists(path + ".part"))

# Sichern auf der PS5 (intern)
choose(last=False)
kind, text = run(app._ps5_backup)
check("Sichern intern auf der PS5", kind == "info" and "gesichert" in text, (kind, text))
check("Ziele: intern, das andere eingebundene Laufwerk, dieser PC - nicht das Laufwerk selbst",
      len(pick["options"]) == 3 and "da1" in pick["options"][1] and not any("da0" in o for o in pick["options"]),
      pick["options"])

# Hochladen und zurückschreiben auf da1 (eingebunden)
select("da1")
choose(last=True)
pick["open"] = path
kind, text = run(app._ps5_restore)
check("Hochladen und wiederherstellen", kind == "info" and "wiederhergestellt" in text, (kind, text))
check("da1 trägt danach die Bezeichnung der Quelle", labels().get("da1") == "QUELLE", labels())

# aus der Sicherung auf der PS5 auf da2 (frei)
select("da2")
choose(last=False)
kind, text = run(app._ps5_restore)
check("aus der PS5-Sicherung wiederherstellen", kind == "info" and "wiederhergestellt" in text, (kind, text))
check("Rückfrage nennt Herkunft der Sicherung", "SanDisk Ultra" in pick.get("confirm", ""), pick.get("confirm"))
check("da2 trägt danach die Bezeichnung der Quelle", labels().get("da2") == "QUELLE", labels())

# Seite "Image-Sicherung": Sicherungen auf der PS5 auflisten, prüfen und löschen
app._nav("ps5:images")
check("Sicherungsliste geladen", pump(lambda: len(app.ps5_images) >= 1, 30), len(app.ps5_images))
first = app.ps5_images[0]["path"] if app.ps5_images else ""
check("Knöpfe ohne Auswahl gesperrt", app.img_verify_btn.instate(["disabled"]) and app.img_del_btn.instate(["disabled"]))
check("Kurzinfo nennt den Grund", "Sicherung" in app._why(app.img_verify_btn, need_dev=False),
      app._why(app.img_verify_btn, need_dev=False))
if first:
    app.img_tree.selection_set(first)
    app._update()
    check("Prüfen/Löschen nach Auswahl bereit", not app.img_verify_btn.instate(["disabled"]))
    kind, text = run(app._img_verify)
    check("Sicherung auf der PS5 geprüft", kind == "info" and "Prüfsumme" in text, (kind, text))
    n0 = len(app.ps5_images)
    uf.messagebox.askokcancel = lambda *a, **k: True
    app.img_tree.selection_set(first)
    app._img_delete()
    check("Sicherung gelöscht, Liste neu", pump(lambda: len(app.ps5_images) == n0 - 1 and not app.busy, 30),
          [i["name"] for i in app.ps5_images])
    check(".img.zst und .sha256 auf der PS5 entfernt", not os.path.exists(first) and not os.path.exists(first + ".sha256"))
app._nav("ps5:format")

# laufendes Spiel: nichts abmelden
game = os.path.join(SANDBOX, "PPSA01325_000")
os.makedirs(game, exist_ok=True)
select("da0")
choose(last=False)
kind, text = run(app._ps5_backup)
check("Spiel läuft: Sichern abgelehnt", kind == "error" and "PPSA01325" in text, (kind, text))
shutil.rmtree(game)

# keine Sicherung: schon vor dem Hochladen abgelehnt
junk = os.path.join(W, "kaputt.img.zst")
with open(junk, "wb") as f:
    f.write(os.urandom(5000))
select("da1")
choose(last=True)
pick["open"] = junk
n = len(msgs)
app._ps5_restore()
pump(lambda: len(msgs) > n, 10)
check("keine Image-Datei: abgelehnt", msgs[-1:] and msgs[-1][0] == "error" and "keine Image-Sicherung" in msgs[-1][1],
      msgs[-1:])

# Versionsabgleich: der Testserver hat dieselbe Version wie das Programm - also keine Rückfrage
asked = []
uf.confirm = lambda root, title, text, ok, ack=None: (asked.append(text), False)[1]
srv_version = app.api.info().get("version")
app._version_notice(srv_version)
check("gleiche Version wie der Formatierer: keine Rückfrage", srv_version == uf.APP_VERSION and not asked,
      (srv_version, uf.APP_VERSION, asked))
app._version_notice("2.4.0")
app._version_notice("2.4.0")
check("ältere Fassung auf der PS5: einmal angeboten", len(asked) == 1 and "2.4.0" in asked[0], asked)
app._version_notice("")
check("ohne Versionsangabe (vor 2.5.3): angeboten", len(asked) == 2, asked)
app._version_notice("99.0.0")
check("neuere Fassung auf der PS5: nur Hinweis", len(asked) == 2 and "99.0.0" in app.status_var.get(),
      app.status_var.get())

app._on_close()
shutil.rmtree(W, ignore_errors=True)
print(f"{'alles bestanden' if not fails else str(fails) + ' fehlgeschlagen'}")
sys.exit(1 if fails else 0)
