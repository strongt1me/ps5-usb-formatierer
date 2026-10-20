"""PC-Programme ohne Oberfläche: Hinweis auf neue Versionen (Auswertung der GitHub-Antwort, Abruf über eine
file:-Adresse) und "Image prüfen" über den Helfer ohne Administratorrechte (PlainJob) - heile, beschädigte,
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
check("neuere Version erkannt", nr({"tag_name": "v" + newer, "html_url": url}) == (newer, url))
check("gleiche Version: nichts", nr({"tag_name": "v" + pc_common.APP_VERSION, "html_url": url}) is None)
check("ältere Version: nichts", nr({"tag_name": "v0.9.9", "html_url": url}) is None)
check("Vorabversion: nichts", nr({"tag_name": "v" + newer, "prerelease": True}) is None)
check("Unsinn: nichts", nr({"tag_name": "neu"}) is None and nr([]) is None and nr({}) is None)
check("fremder Link wird nicht geöffnet",
      nr({"tag_name": "v" + newer, "html_url": "https://boese.example/x"}) == (newer, pc_common.REPO_URL + "/releases/latest"))
check("zweistellig verglichen (2.10.0 > 2.9.9)", nr({"tag_name": "v2.10.0"}, current="2.9.9") is not None)

work = tempfile.mkdtemp(prefix="pctools-")
try:
    feed = os.path.join(work, "latest.json")
    with open(feed, "w", encoding="utf-8") as f:
        json.dump({"tag_name": "v" + newer, "html_url": url, "draft": False, "prerelease": False}, f)
    os.environ["USBFMT_UPDATE_URL"] = pathlib.Path(feed).as_uri()
    pc_common.start_update_check()
    for _ in range(100):
        with pc_common._update_lock:
            if pc_common._update["state"] == "done":
                break
        time.sleep(0.05)
    check("Abruf: neue Version gefunden", pc_common._update["found"] == (newer, url), pc_common._update)
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
