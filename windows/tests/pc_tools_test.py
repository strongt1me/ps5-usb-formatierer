"""PC-Programme ohne Oberfläche: Hinweis auf neue Versionen (Auswertung der GitHub-Antwort, passende Datei je
System, Release-Hinweise als Text, Abruf über eine file:-Adresse, Fenster "Was ist neu" falls ein Bildschirm da ist) und "Image prüfen" über den Helfer ohne Administratorrechte (PlainJob) - heile, beschädigte,
abgeschnittene Datei und eine falsche .sha256 daneben.

    python windows/tests/pc_tools_test.py <test-helfer>   (usbfmt_win_test.exe bzw. usbfmt_linux mit -DUSBFMT_TEST)

Das Image legt der Test-Helfer selbst an (--image: Abbild-Datei statt Datenträger)."""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ["USBFMT_NO_UPDATE_CHECK"] = ""
import pc_common  # noqa: E402

HELPER = os.path.abspath(sys.argv[1])
fails = 0


def check(name, ok, extra=""):
    global fails
    print(("OK      " if ok else "FEHLER  ") + name + ("" if ok else f"  [{extra}]"))
    fails += 0 if ok else 1


# ---------------------------------------------------------------- neue Version?
cur = pc_common.version_tuple(pc_common.APP_VERSION)
check("eigene Version lesbar", cur is not None, pc_common.APP_VERSION)
newer = f"{cur[0]}.{cur[1]}.{cur[2] + 1}"
url = pc_common.REPO_URL + f"/releases/tag/v{newer}"
nr = pc_common.newer_release
dl = f"{pc_common.REPO_URL}/releases/download/v{newer}/"
names = ["PS5-USB-Formatierer.exe", "USB-Formatierer.exe", "PS5-USB-Formatierer-x86_64.AppImage",
         "PS5-USB-Formatierer-linux-x86_64.tar.gz", "USB-Formatierer-linux-aarch64.tar.gz", "usb_format.elf"]
rel = {"tag_name": "v" + newer, "html_url": url, "body": "## Neu\n\n- **schneller**\n- [Hilfe](x)",
       "assets": [{"name": n, "browser_download_url": dl + n, "digest": "sha256:" + hashlib.sha256(n.encode()).hexdigest()}
                  for n in names[:-1]]
       + [{"name": names[-1], "browser_download_url": dl + names[-1], "digest": "md5:1234"},
          {"name": "Boese.exe", "browser_download_url": "https://boese.example/Boese.exe"}, "kaputt"]}
found = nr(rel)
check("neuere Version erkannt", found and found["version"] == newer and found["url"] == url, found)
check("nur Dateien dieses Releases übernommen", found and sorted(found["assets"]) == sorted(names), found)
check("Prüfsummen von GitHub übernommen, ungültige weggelassen", found
      and found["digests"].get(names[0]) == hashlib.sha256(names[0].encode()).hexdigest()
      and names[-1] not in found["digests"] and len(found["digests"]) == len(names) - 1, found and found["digests"])
check("Release-Hinweise übernommen", found and found["notes"].startswith("## Neu"), found)
check("Hinweise als Text", pc_common.plain_notes(found["notes"]) == "Neu\n\n• schneller\n• Hilfe",
      pc_common.plain_notes(found["notes"]))
pick = pc_common.release_file
if pc_common.IS_WIN:
    check("Windows: passende .exe", pick(found, "USB-Formatierer") == ("USB-Formatierer.exe", dl + "USB-Formatierer.exe"))
else:
    arch = pc_common.machine()
    want = "PS5-USB-Formatierer-x86_64.AppImage" if arch == "x86_64" else None
    check("Linux: AppImage vor dem Archiv", (pick(found, "PS5-USB-Formatierer") or (None,))[0] == want,
          pick(found, "PS5-USB-Formatierer"))
    check("Linux: Archiv, wenn kein AppImage", (pick(found, "USB-Formatierer") or (None,))[0]
          == (None if arch != "aarch64" else "USB-Formatierer-linux-aarch64.tar.gz"), pick(found, "USB-Formatierer"))
check("keine passende Datei: None", pick({"assets": {}}, "USB-Formatierer") is None)
check("gleiche Version: nichts", nr({"tag_name": "v" + pc_common.APP_VERSION, "html_url": url}) is None)
check("ältere Version: nichts", nr({"tag_name": "v0.9.9", "html_url": url}) is None)
check("Vorabversion: nichts", nr({"tag_name": "v" + newer, "prerelease": True}) is None)
check("Unsinn: nichts", nr({"tag_name": "neu"}) is None and nr([]) is None and nr({}) is None)
check("fremder Link wird nicht geöffnet",
      nr({"tag_name": "v" + newer, "html_url": "https://boese.example/x"})["url"] == pc_common.REPO_URL + "/releases/latest")
bad = nr({"tag_name": "v" + newer, "body": 5, "assets": 7})
check("Unsinn in Hinweisen/Dateien: leer", bad and bad["notes"] == "" and bad["assets"] == {}, bad)
check("zweistellig verglichen (2.10.0 > 2.9.9)", nr({"tag_name": "v2.10.0"}, current="2.9.9") is not None)

work = tempfile.mkdtemp(prefix="pctools-")
try:
    feed = os.path.join(work, "latest.json")
    with open(feed, "w", encoding="utf-8") as f:
        json.dump(dict(rel, draft=False, prerelease=False), f)
    os.environ["USBFMT_UPDATE_URL"] = pathlib.Path(feed).as_uri()
    pc_common.start_update_check()
    for _ in range(100):
        with pc_common._update_lock:
            if pc_common._update["state"] == "done":
                break
        time.sleep(0.05)
    check("Abruf: neue Version gefunden", pc_common._update["found"] == found, pc_common._update)
    pc_common._update.update(state=None, found=None)
    os.environ["USBFMT_UPDATE_URL"] = pathlib.Path(os.path.join(work, "fehlt.json")).as_uri()
    pc_common.start_update_check()
    for _ in range(100):
        if pc_common._update["state"] == "done":
            break
        time.sleep(0.05)
    check("Abruf scheitert: still, kein Hinweis", pc_common._update == {"state": "done", "found": None}, pc_common._update)
    os.environ["USBFMT_NO_UPDATE_CHECK"] = "1"
    pc_common._update.update(state=None, found=None)
    pc_common.start_update_check()
    check("abschaltbar (USBFMT_NO_UPDATE_CHECK)", pc_common._update["state"] is None)

    # ---------------------------------------------------------------- Fenster "Was ist neu" (nur mit Bildschirm)
    try:
        import tkinter as tk
        root = tk.Tk()
    except Exception as e:   # kein Bildschirm (CI ohne Xvfb)
        print("        Fenster \"Was ist neu\" übersprungen:", e)
    else:
        pc_common.setup_window(root, "dark")
        win = pc_common.update_dialog(root, found, "PS5-USB-Formatierer")
        root.update()
        texts = []

        def walk(w):
            for ch in w.winfo_children():
                if ch.winfo_class() == "TEntry":
                    texts.append(ch.get())
                    continue
                try:
                    texts.append(str(ch.cget("text")))
                except tk.TclError:
                    if isinstance(ch, tk.Text):
                        texts.append(ch.get("1.0", "end"))
                walk(ch)
        walk(win)
        joined = "\n".join(texts)
        check("Fenster: Version, Hinweise, Knöpfe", newer in joined and "• schneller" in joined
              and "Release-Seite öffnen" in joined and "Schließen" in joined, texts)
        target = pc_common.release_file(found, "PS5-USB-Formatierer")
        check("Fenster: Knopf für die passende Datei", (target is None) or ("Herunterladen (" + target[0] + ")") in joined,
              texts)
        check("Fenster: Prüfsumme der angebotenen Datei", (target is None)
              or hashlib.sha256(target[0].encode()).hexdigest() in texts, texts)
        win.destroy()
        root.destroy()

    # ---------------------------------------------------------------- Auftragsverlauf am PC
    os.environ["APPDATA" if pc_common.IS_WIN else "XDG_CONFIG_HOME"] = os.path.join(work, "cfg")
    pc_common.record_job(["format", "4", "1000"], True, "vorher", None)
    check("ohne enable_history: nichts geschrieben", not os.path.exists(os.path.join(work, "cfg")))
    pc_common.enable_history("VerlaufTest")
    dev = {"disk": 4, "size": 64 * 10 ** 9, "model": "UDisk", "volumes": [{"label": "ALT"}]}
    t0 = time.time() - 3
    pc_common.record_job(["format", "4", str(dev["size"]), "exfat", "NEU", "serial=x"], True, "formatiert", dev, t0)
    pc_common.record_job(["speed", "4", str(dev["size"])], False, "Abgebrochen.", dev, t0, cancelled=True)
    pc_common.record_job(["verify", os.path.join(work, "a.img.zst")], False, "beschädigt", None, t0)
    h = pc_common.history_load()
    check("Verlauf: neueste zuerst", [e["kind"] for e in h] == ["verify", "speed", "format"], h)
    f = h[2] if len(h) == 3 else {}
    check("Formatieren: Dateisystem, neue Bezeichnung, Laufwerk, Dauer",
          (f.get("fs"), f.get("label"), f.get("model"), f.get("size"), f.get("ok"), f.get("secs", 0) >= 3)
          == ("exFAT", "NEU", "UDisk", dev["size"], True, True), f)
    check("abgebrochen / fehlgeschlagen unterschieden",
          len(h) == 3 and h[1]["cancelled"] and not h[0]["cancelled"] and h[0]["file"] == "a.img.zst", h)
    with open(pc_common._history["path"], "a", encoding="utf-8") as fh:
        fh.write("kaputt\n[1]\n")
    for i in range(120):
        pc_common.record_job(["speed", "4", "1"], True, f"lauf {i}", dev)
    h = pc_common.history_load()
    check("höchstens 100 Einträge, kaputte Zeilen fallen weg", len(h) == 100 and h[0]["msg"] == "lauf 119"
          and all(isinstance(e, dict) for e in h), (len(h), h[:1]))
    try:
        import tkinter as tk
        root = tk.Tk()
    except Exception as e:   # kein Bildschirm (CI ohne Xvfb)
        print("        Fenster \"Verlauf\" übersprungen:", e)
    else:
        pc_common.setup_window(root, "light")
        errors = []
        root.report_callback_exception = lambda *a: errors.append(a[1])
        cleared = []
        win = pc_common.history_dialog(root, [("PC", h[:3]), ("PS5", [{"t": int(time.time()) + 5, "kind": "tile",
                                                                       "ok": True, "secs": 4, "msg": "Kachel"}])],
                                       lambda: cleared.append(1) or [("PC", []), ("PS5", [])])
        root.update()
        tree = [w for w in win.winfo_children()[0].winfo_children() if w.winfo_class() == "Treeview"][0]
        first = tree.item(tree.get_children()[0], "values")
        check("Fenster: 4 Zeilen, PS5-Eintrag (neuester) oben", len(tree.get_children()) == 4 and first[1] == "PS5"
              and first[2] == "Kachel neu anmelden", (len(tree.get_children()), first))
        for iid in reversed(tree.get_children()):   # jede Zeile wählen, zuletzt die oberste (PS5, 4 s)
            tree.selection_set(iid)
            root.update()
        texts = [str(w.cget("text")) for w in win.winfo_children()[0].winfo_children() if w.winfo_class() == "TLabel"]
        check("Fenster: Meldung und Dauer des gewählten Eintrags", not errors and "Kachel\nDauer: 4 s" in texts,
              (errors, texts))
        win.destroy()
        root.destroy()
    pc_common.history_clear()
    check("Verlauf geleert", pc_common.history_load() == [])
    pc_common._history["path"] = None

    # ---------------------------------------------------------------- Image prüfen ohne Administratorrechte
    raw, img = os.path.join(work, "stick.img"), os.path.join(work, "stick.img.zst")
    size = 96 * 1024 * 1024
    with open(raw, "wb") as f:
        f.truncate(size)

    def helper(*args):
        r = subprocess.run([HELPER, "--lang", "de", "--image", raw, *args], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        return [line for line in r.stdout.splitlines() if line.startswith("R\t")][-1:]

    check("Testabbild formatiert", helper("format", "9", str(size), "exfat", "PRUEFEN")[0].startswith("R\t1"))
    check("Testabbild gesichert", helper("backup", "9", str(size), img)[0].startswith("R\t1") and os.path.isfile(img))
    pc_common.find_helper = lambda: HELPER

    def verify(path):
        job = pc_common.PlainJob(["verify", path])
        steps = []
        res = job.follow(lambda pct, step: steps.append((pct, step)))
        return res, steps, job

    (ok, msg, info), steps, job = verify(img)
    sha = hashlib.sha256(open(img, "rb").read()).hexdigest()
    check("heile Datei: in Ordnung", ok and info.get("checked") is True and info.get("side") == "ok"
          and info.get("sha256") == sha, (ok, msg, info))
    check("Meldung nennt die Prüfsumme", "Prüfsumme" in msg and ".sha256" in msg, msg)
    check("Fortschritt gemeldet", any(s == "Image prüfen" for _p, s in steps), steps[:3])
    check("Arbeitsordner aufgeräumt", not os.path.exists(job.tmpdir))

    bad = os.path.join(work, "kaputt.img.zst")
    data = bytearray(open(img, "rb").read())
    mid = len(data) // 2
    data[mid:mid + 4] = b"\xff\x00\xff\x00"
    open(bad, "wb").write(data)
    (ok, msg, info), _s, _j = verify(bad)
    check("beschädigte Datei erkannt", not ok and "fehlerhaft" in msg, msg)

    cut = os.path.join(work, "halb.img.zst")
    open(cut, "wb").write(open(img, "rb").read()[: os.path.getsize(img) // 2])
    (ok, msg, info), _s, _j = verify(cut)
    check("abgeschnittene Datei erkannt", not ok and "unvollständig" in msg, msg)

    other = os.path.join(work, "anders.img.zst")
    shutil.copy(img, other)
    with open(other + ".sha256", "w") as f:
        f.write("0" * 64 + "  anders.img.zst\n")
    (ok, msg, info), _s, _j = verify(other)
    check("falsche .sha256 erkannt", not ok and info.get("side") == "bad", (ok, msg, info))
    os.remove(other + ".sha256")
    (ok, msg, info), _s, _j = verify(other)
    check("ohne .sha256: in Ordnung", ok and info.get("side") == "none", (ok, msg, info))

    (ok, msg, info), _s, _j = verify(os.path.join(work, "fehlt.img.zst"))
    check("fehlende Datei: Fehler statt Absturz", not ok and msg, msg)
finally:
    shutil.rmtree(work, ignore_errors=True)

print(f"{fails} fehlgeschlagen" if fails else "PC-Werkzeuge: alle Prüfungen bestanden")
sys.exit(1 if fails else 0)
