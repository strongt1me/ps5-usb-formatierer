#!/usr/bin/env python3
"""Spielstand-API gegen den Host-Formatierer: Liste, ZIP-Sicherung, Wiederherstellen (auch mit bösartigen/kaputten ZIPs),
Schutzregeln.
Aufruf: saves_test.py <host-binary> [port]   (das Binary mit -DSERVER_PORT=<port> gebaut, Standard 18115)"""
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

BIN = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18115
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import saves_fixture as fx  # noqa: E402

fails = 0


def ok(name, extra=""):
    print("OK      " + name + (": " + extra if extra else ""))


def bad(name, extra=""):
    global fails
    fails += 1
    print("FEHLER  " + name + (": " + extra if extra else ""))


def check(name, cond, extra=""):
    (ok if cond else bad)(name, extra if not cond else "")


W = tempfile.mkdtemp(prefix="savestest")
ROOT = os.path.join(W, "fx")
HOME = os.path.join(ROOT, "home")
DATA = os.path.join(W, "data")
SAND = os.path.join(W, "sandbox")
os.makedirs(DATA)
os.makedirs(SAND)
sums = json.loads(subprocess.check_output([sys.executable, os.path.join(HERE, "saves_fixture.py"), ROOT]))
env = dict(os.environ, USBFMT_SAVES=HOME, USBFMT_SLOW="1", USBFMT_DATA=DATA, USBFMT_IMAGES="", USBFMT_SANDBOX=SAND)
srv = subprocess.Popen([BIN], env=env, stdout=open(os.path.join(W, "srv.log"), "w"), stderr=subprocess.STDOUT)


def cleanup():
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(5)
    except subprocess.TimeoutExpired:
        srv.kill()
    shutil.rmtree(W, ignore_errors=True)


def req(method, path, body=None, headers=None, raw=False):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=60)
    h = dict(headers or {})
    if isinstance(body, (dict, list)):
        body = json.dumps(body).encode()
        h.setdefault("Content-Type", "application/json")
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    out = (r.status, dict(r.getheaders()), data)
    c.close()
    return out


def jreq(method, path, body=None, headers=None):
    st, hd, data = req(method, path, body, headers)
    try:
        return st, json.loads(data)
    except ValueError:
        return st, {"raw": data[:200]}


def wait_job(timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        st, j = jreq("GET", "/api/status")
        if st == 200 and not j.get("busy"):
            return j
        time.sleep(0.05)
    return None


for _ in range(100):
    try:
        if req("GET", "/api/info")[0] == 200:
            break
    except OSError:
        time.sleep(0.1)
else:
    print("FEHLER  Server startet nicht")
    sys.exit(1)


def tree():
    """alle Dateien unter dem Fixture-Ordner (Pfad -> Größe, Rechte) und unter dem Arbeitsordner"""
    out = {}
    for base in (ROOT, DATA):
        for d, _, fs in os.walk(base):
            for f in fs:
                if f == "history.jsonl":   # der Verlauf wächst mit jedem Auftrag
                    continue
                p = os.path.join(d, f)
                st = os.stat(p)
                out[p] = (st.st_size, st.st_mode & 0o777)
    return out


def file_sha(uid, tid, name):
    return hashlib.sha256(open(os.path.join(HOME, uid, "savedata_prospero", tid, name), "rb").read()).hexdigest()


K1 = "1c3f8a0e/PPSA01234/sdimg_SAVEDATA00"
K2 = "2d4e9b1f/PPSA07890/sdimg_SAVEDATA01"

try:
    # ------------------------------------------------------------------ Liste
    print("== Liste ==")
    st, j = jreq("GET", "/api/saves")
    keys = [s["key"] for s in j.get("saves", [])]
    check("6 Spielstände (5 PS5, 1 PS4), sortiert, ohne .bin/Systemkopien (sce_bu)/zu kleine/leere Ordner/PS4 ohne Schlüssel", st == 200 and keys == sorted(keys) and len(keys) == 6,
          str(keys))
    s1 = next(s for s in j["saves"] if s["key"] == K1)
    check("Spielname aus param.sfo des Spiels", s1["title_name"] == 'Beispiel-Spiel: Die "Rückkehr"', s1["title_name"])
    check("Benutzername und Größe", s1["username"] == "Alex" and s1["size"] == 2 << 20, str(s1))
    check("Konsole: 5 mal PS5, 1 mal PS4 (mit Schlüsseldatei)", sorted(x["platform"] for x in j["saves"]) == ["PS4"] + ["PS5"] * 5, str([x["platform"] for x in j["saves"]]))
    check("PS4-Eintrag: Schlüssel mit #PS4, Spielname aus param.sfo", any(x["key"] == "1c3f8a0e/CUSA00001/sdimg_PS4DATA#PS4" and x["title_name"] == "PS4-Beispiel" for x in j["saves"]), "")
    check("Systemkopien (sce_bu) werden nicht gelistet", not [x for x in j["saves"] if "sce_bu_" in x["save_dir"]], "")
    st, j = jreq("GET", "/api/saves/users")
    users = {u["uid"]: u for u in j["users"]}
    check("3 Benutzer mit Namen", len(users) == 3 and users["2d4e9b1f"]["username"] == "Sam", str(j))

    # ------------------------------------------------------------------ Sichern
    print("== Sichern (ZIP) ==")
    before = tree()
    st, hd, data = req("GET", "/api/saves/download?key=" + K1.replace("/", "%2F"))
    check("Download 200 application/zip", st == 200 and hd.get("Content-Type") == "application/zip", str(st))
    check("Dateiname im Kopf", 'filename="PPSA01234_sdimg_SAVEDATA00.zip"' in hd.get("Content-Disposition", ""),
          hd.get("Content-Disposition", ""))
    z = zipfile.ZipFile(io.BytesIO(data))
    check("ZIP intakt (testzip)", z.testzip() is None)
    names = z.namelist()
    check("Einträge metadata.json + Container", names == ["metadata.json", "PPSA01234_sdimg_SAVEDATA00.img"], str(names))
    meta = json.loads(z.read("metadata.json"))
    check("Metadaten", meta["title_id"] == "PPSA01234" and meta["uid"] == "1c3f8a0e" and meta["username"] == "Alex"
          and meta["save_dir"] == "sdimg_SAVEDATA00" and meta["title_name"] == s1["title_name"] and meta["size"] == 2 << 20, str(meta))
    check("metadata.json nennt die Konsole", meta.get("platform") == "PS5", str(meta))
    check("Container unverändert (SHA-256)", hashlib.sha256(z.read(names[1])).hexdigest() == sums[K1])
    check("Download ändert nichts", tree() == before)
    check("fehlender Schlüssel 400", req("GET", "/api/saves/download")[0] == 400)
    check("unbekannter Schlüssel 404", req("GET", "/api/saves/download?key=nope")[0] == 404)
    check("Pfad-Ausbruch im Schlüssel 404", req("GET", "/api/saves/download?key=..%2F..%2F..%2Fetc%2Fpasswd")[0] == 404
          and req("GET", "/api/saves/download?key=1c3f8a0e%2F..%2F..%2F..%2Fx")[0] == 404)
    # drei Downloads gleichzeitig: der Vorgang ist einfach gesperrt, keiner liefert Müll
    import threading
    res = []

    def dl():
        s, h, d = req("GET", "/api/saves/download?key=" + K2.replace("/", "%2F"))
        res.append((s, len(d)))
    ts = [threading.Thread(target=dl) for _ in range(3)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    check("parallele Downloads: Antworten 200 oder 500 (gesperrt), nie kaputt", all(s in (200, 500) for s, _ in res) and
          any(s == 200 for s, _ in res), str(res))

    # ------------------------------------------------------------------ Wiederherstellen
    print("== Wiederherstellen ==")
    path1 = os.path.join(HOME, "1c3f8a0e", "savedata_prospero", "PPSA01234", "sdimg_SAVEDATA00")
    orig = open(path1, "rb").read()
    os.chmod(path1, 0o640)
    open(path1, "wb").write(b"\x02" + b"\0" * 5000)
    st, hd, body = req("POST", "/api/saves/restore/upload?lang=en", data, {"Content-Type": "application/zip"})
    j = wait_job()
    check("Wiederherstellen 200", st == 200, body.decode()[:200])
    check("Job save_restore ok", j and j["kind"] == "save_restore" and j["ok"], str(j))
    check("Inhalt wieder wie vorher", open(path1, "rb").read() == orig)
    check("Rechte der vorhandenen Datei bleiben", os.stat(path1).st_mode & 0o777 == 0o640, oct(os.stat(path1).st_mode & 0o777))
    leftovers = [p for p in tree() if p.endswith((".tmp", "upload.zip"))]
    check("keine Zwischendateien übrig", not leftovers, str(leftovers))
    # neuer Titel-Ordner wird angelegt
    z2 = io.BytesIO()
    with zipfile.ZipFile(z2, "w", zipfile.ZIP_STORED) as zz:
        zz.writestr("metadata.json", json.dumps({"title_id": "PPSA55555", "save_dir": "NEUER_STAND", "uid": "3a5b7c9d"}))
        zz.writestr("PPSA55555_NEUER_STAND.img", fx.container("neu", 1 << 20))
    st, _, body = req("POST", "/api/saves/restore/upload", z2.getvalue(), {"Content-Type": "application/zip"})
    wait_job()
    p = os.path.join(HOME, "3a5b7c9d", "savedata_prospero", "PPSA55555", "NEUER_STAND")
    check("Wiederherstellen in neuen Titel-Ordner (Python-ZIP, Datendeskriptor-frei)", st == 200 and os.path.getsize(p) == 1 << 20)
    st, j = jreq("GET", "/api/saves")
    check("neuer Spielstand erscheint in der Liste", any(s["key"] == "3a5b7c9d/PPSA55555/NEUER_STAND" for s in j["saves"]))
    shutil.rmtree(os.path.join(HOME, "3a5b7c9d", "savedata_prospero", "PPSA55555"))

    # ------------------------------------------------------------------ Titelname, Auswahl, Sicherungsordner
    print("== Spielname aus param.json ==")
    st, j = jreq("GET", "/api/saves")
    s9 = next(s for s in j["saves"] if s["title_id"] == "PPSA09999")
    check("Spielname aus param.json (\\u-Maskierung, Ersatzpaar, defaultLanguage)", s9["title_name"] == 'Café 🎮 "Quest"', s9["title_name"])

    print("== Mehrere Spielstände in einer ZIP ==")
    KA = "1c3f8a0e/PPSA07890/sdimg_SAVEDATA00"
    sel = [K1, KA, K2]
    q = ",".join(k.replace("/", "%2F") for k in sel)
    st, hd, bundle = req("GET", "/api/saves/download?keys=" + q)
    zb = zipfile.ZipFile(io.BytesIO(bundle)) if st == 200 else None
    check("Sammel-ZIP 200, intakt", st == 200 and zb.testzip() is None, str(st))
    nm = zb.namelist() if zb else []
    check("je Spielstand Ordner mit metadata.json und Container", len(nm) == 6 and all("/" in n for n in nm), str(nm))
    imgs_ok = zb and all(hashlib.sha256(zb.read(n)).hexdigest() == sums[k] for k in sel for n in nm
                         if n.endswith(".img") and n.split("/")[0] == k.split("/")[0] + "_" + k.split("/")[1] + "_" + k.split("/")[2])
    check("Container in der Sammel-ZIP unverändert", imgs_ok)
    check("unbekannter Schlüssel in der Auswahl 404", req("GET", "/api/saves/download?keys=" + q + ",nope")[0] == 404)
    many = ",".join(["x%d" % i for i in range(70)])
    check("mehr als 64 Schlüssel abgelehnt", req("GET", "/api/saves/download?keys=" + many)[0] in (400, 404))
    paths = {k: os.path.join(HOME, *k.split("/")[:1], "savedata_prospero", *k.split("/")[1:]) for k in sel}
    for p in paths.values():
        open(p, "wb").write(b"\x02" + bytes(5000))
    st, hd, rb = req("POST", "/api/saves/restore/upload?lang=en", bundle, {"Content-Type": "application/zip"})
    res = wait_job()
    check("Sammel-ZIP wiederhergestellt: alle drei, Meldung nennt 3", st == 200 and res and res["ok"] and "restored: 3" in res["message"], str(res and res["message"]))
    check("alle drei Container wieder wie vorher", all(file_sha(*k.split("/")[:1], *k.split("/")[1:]) == sums[k] for k in sel))
    # Sammel-ZIP mit einem fehlerhaften Spielstand: nichts wird geschrieben
    badz = bytearray(bundle)
    off = badz.find(zb.read([n for n in nm if n.endswith(".img")][2])[200:232])
    badz[off + 3] ^= 0xFF
    for p in paths.values():
        open(p, "wb").write(b"\x02" + bytes(5000))
    snap = {p: open(p, "rb").read() for p in paths.values()}
    st, hd, rb = req("POST", "/api/saves/restore/upload?lang=en", bytes(badz), {"Content-Type": "application/zip"})
    res = wait_job()
    check("Sammel-ZIP mit Prüfsummenfehler im dritten: Fehler, die ersten zwei sind geschrieben, Meldung nennt es",
          st == 500 and res and not res["ok"] and "2 of 3" in res["message"], str(res and res["message"]))
    sizes = {K1: 2 << 20, KA: 1 << 20, K2: 3 << 20}
    for k in sel:   # für die Folgetests wieder die Originale
        u, ti, na = k.split("/")
        open(paths[k], "wb").write(fx.container(u + ti + na, sizes[k], uid=u, title=ti, name=na))

    print("== Sicherungsordner (Orte, Sichern, Auflisten, Wiederherstellen) ==")
    st, j = jreq("GET", "/api/saves/locations")
    loc = j["locations"][0] if st == 200 and j.get("locations") else {}
    check("Orte: intern mit Ordner game_saves", loc.get("dir", "").endswith("/game_saves") and loc.get("free", 0) > 0, str(j))
    dest = loc["dir"]
    st, j = jreq("POST", "/api/saves/backup?lang=en", {"keys": "\n".join([K1, KA]), "dest": dest})
    res = wait_job()
    check("Sichern nach game_saves: Auftrag save_backup ok, 2 Spielstände", st == 202 and res and res["ok"] and res["kind"] == "save_backup"
          and "backed up: 2" in res["message"] and dest in res["message"], "%s %s" % (st, res))
    z1 = os.path.join(dest, "Alex_1c3f8a0e", "PPSA01234_sdimg_SAVEDATA00.zip")
    check("Sicherung liegt in <Benutzer>_<uid>/<Titel>_<Name>.zip und ist eine gültige ZIP",
          os.path.exists(z1) and zipfile.ZipFile(z1).testzip() is None and zipfile.ZipFile(z1).namelist() == ["metadata.json", "PPSA01234_sdimg_SAVEDATA00.img"])
    check("Sicherung: Konsole in metadata.json und in der Liste", json.loads(zipfile.ZipFile(z1).read("metadata.json"))["platform"] == "PS5", "")
    check("Container in der Sicherung unverändert", hashlib.sha256(zipfile.ZipFile(z1).read("PPSA01234_sdimg_SAVEDATA00.img")).hexdigest() == sums[K1])
    check("keine Zwischendateien", not [p for p in tree() if p.endswith(".tmp")] and not [f for d, _, fs in os.walk(dest) for f in fs if f.endswith(".tmp")])
    st, j = jreq("GET", "/api/saves/backups")
    bk = j.get("backups", [])
    check("Sicherungsliste nennt die Konsole", all(b["platform"] == "PS5" for b in jreq("GET", "/api/saves/backups")[1]["backups"]), "")
    check("Sicherungen aufgelistet mit Benutzer, Spielname, Größe", len(bk) == 2 and bk[0]["username"] == "Alex" and bk[0]["title_name"] and bk[0]["size"] > 1000
          and any(b["path"] == z1 and b["title_id"] == "PPSA01234" for b in bk), str(bk)[:300])
    # Original verändern, aus der Sicherung zurückschreiben (nur ein Pfad, dann zwei)
    open(paths[K1], "wb").write(b"\x02" + bytes(5000))
    open(paths[KA], "wb").write(b"\x02" + bytes(5000))
    st, j = jreq("POST", "/api/saves/restore?lang=en", {"paths": z1})
    res = wait_job()
    check("Wiederherstellen aus der Sicherung (ein Pfad)", st == 202 and res and res["ok"] and "restored: 1" in res["message"], str(res and res["message"]))
    check("... Container wieder wie vorher", file_sha("1c3f8a0e", "PPSA01234", "sdimg_SAVEDATA00") == sums[K1])
    st, j = jreq("POST", "/api/saves/restore?lang=en", {"paths": "\n".join(b["path"] for b in bk)})
    res = wait_job()
    check("Wiederherstellen aus der Sicherung (zwei Pfade)", st == 202 and res and res["ok"] and "restored: 2" in res["message"]
          and file_sha("1c3f8a0e", "PPSA07890", "sdimg_SAVEDATA00") == sums[KA], str(res and res["message"]))
    snap = tree()
    for name, body in (("Pfad außerhalb der Orte", {"paths": "/etc/passwd"}), ("Pfad mit ..", {"paths": dest + "/../data/x.zip"}),
                       ("keine ZIP", {"paths": os.path.join(dest, "Alex_1c3f8a0e", "gibtsnicht.zip")}),
                       ("ZIP direkt im Ordner (falsche Tiefe)", {"paths": z1.replace("/Alex_1c3f8a0e", "")}),
                       ("leer", {"paths": ""}), ("fehlt", {}), ("ein gültiger, ein ungültiger", {"paths": z1 + "\n/etc/passwd"})):
        st, j = jreq("POST", "/api/saves/restore", body)
        check("Wiederherstellen aus Pfad abgelehnt: " + name, st == 400, "%s %s" % (st, j))
    st, j = jreq("POST", "/api/saves/backup", {"keys": K1, "dest": "/etc"})
    check("Sichern in fremden Ordner abgelehnt", st == 400, "%s %s" % (st, j))
    st, j = jreq("POST", "/api/saves/backup", {"keys": "", "dest": dest})
    check("Sichern ohne Auswahl abgelehnt", st == 400, "%s %s" % (st, j))
    st, j = jreq("POST", "/api/saves/backup?lang=en", {"keys": "9/9/9", "dest": dest})
    res = wait_job()
    check("Sichern unbekannter Spielstand: Auftrag meldet Fehler", st == 202 and res and not res["ok"] and "not found" in res["message"], str(res and res["message"]))
    check("abgelehnte Anfragen ändern nichts", tree() == snap)
    os.makedirs(os.path.join(SAND, "PPSA11111_000"))
    st, j = jreq("POST", "/api/saves/restore", {"paths": z1})
    check("Wiederherstellen aus Sicherung bei laufendem Spiel 409", st == 409, "%s %s" % (st, j))
    st, j = jreq("POST", "/api/saves/backup?lang=en", {"keys": K1, "dest": dest})
    res = wait_job()
    check("Sichern bei laufendem Spiel erlaubt (nur lesen)", st == 202 and res and res["ok"], str(res and res["message"]))
    shutil.rmtree(os.path.join(SAND, "PPSA11111_000"))
    # zwei gleichzeitige Downloads derselben Datei dürfen sich nicht stören (Browser fragen doppelt an)
    r2 = []
    def dl2():
        s2, h2, d2 = req("GET", "/api/saves/download?key=" + K2.replace("/", "%2F"))
        r2.append((s2, len(d2)))
    ts = [threading.Thread(target=dl2) for _ in range(4)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    check("vier gleichzeitige Downloads: alle 200", all(s2 == 200 and n > 1000 for s2, n in r2), str(r2))

    print("== Versionen der Sicherung ==")
    zdir = os.path.dirname(z1)
    for i in range(4):   # die bisherige Sicherung zeitlich verschieben: sonst tragen die Vorversionen denselben Zeitstempel
        os.utime(z1, (1700000000 + i * 100000,) * 2)
        st, j = jreq("POST", "/api/saves/backup?lang=en", {"keys": K1, "dest": dest})
        wait_job()
    vers = sorted(f for f in os.listdir(zdir) if f.startswith("PPSA01234_sdimg_SAVEDATA00.") and f.endswith(".zip") and f != os.path.basename(z1))
    check("Sichern behält die neueste und zwei Vorversionen", os.path.exists(z1) and len(vers) == 2, str(vers))
    check("Vorversionen heißen <Titel>_<Name>.<JJJJMMTT-HHMMSS>.zip", all(re.fullmatch(r"PPSA01234_sdimg_SAVEDATA00\.\d{8}-\d{6}\.zip", v) for v in vers), str(vers))
    check("Vorversionen sind gültige ZIPs", all(zipfile.ZipFile(os.path.join(zdir, v)).testzip() is None for v in vers))
    st, j = jreq("GET", "/api/saves/backups")
    mine = [b for b in j["backups"] if b["title_id"] == "PPSA01234" and b["save_dir"] == "sdimg_SAVEDATA00"]
    check("Liste: drei Einträge dieser Sicherung, neueste zuerst", len(mine) == 3 and mine[0]["path"] == z1 and mine[0]["time"] >= mine[1]["time"] >= mine[2]["time"], str(mine)[:300])
    check("Liste und Spielstände tragen die Änderungszeit des Spielstands", all(isinstance(b.get("save_mtime"), int) and b["save_mtime"] > 0 for b in mine)
          and isinstance(jreq("GET", "/api/saves")[1]["saves"][0].get("mtime"), int), str(mine)[:200])
    for v in vers:   # für die Folgetests wieder nur die eine Sicherung
        os.unlink(os.path.join(zdir, v))

    print("== Fortschritt beim Sichern ==")
    all5 = [s["key"] for s in jreq("GET", "/api/saves")[1]["saves"]]
    NALL = len(all5)
    samples = []
    st, j = jreq("POST", "/api/saves/backup?lang=en", {"keys": chr(10).join(all5), "dest": jreq("GET", "/api/saves/locations")[1]["locations"][0]["dir"]})
    t_end = time.time() + 60
    while time.time() < t_end:
        _, j = jreq("GET", "/api/status")
        samples.append((j.get("percent"), j.get("step"), j.get("busy")))
        if not j.get("busy"):
            break
        time.sleep(0.03)
    run = [x for x in samples if x[2] and x[1] not in ("Starting", "Startet")]
    pcts = [x[0] for x in run]
    check("Fortschritt: nie 0 % während des Sicherns (sonst nur ein laufender Balken)", run and all(p >= 1 for p in pcts), str(run[:5]))
    check("Fortschritt: steigt, endet nicht vor 100 %", pcts == sorted(pcts) and pcts[-1] <= 99 and pcts[-1] > pcts[0], str(pcts[:3]) + "…" + str(pcts[-3:]))
    check("Fortschritt: Schritt nennt Spielstand x von 5 und den Spielnamen",
          any("save game 1 of %d" % NALL in (x[1] or "") for x in run) and any("save game %d of %d" % (NALL, NALL) in (x[1] or "") for x in run) and any("Zweites Spiel" in (x[1] or "") for x in run),
          str(sorted(set(x[1] for x in run)))[:300])
    wait_job()

    print("== Abbrechen ==")
    cdest = os.path.join(W, "cancel_dest")
    os.makedirs(cdest)
    # die Orte kennt der Server selbst: ein eigener Ordner ist kein Ort -> der Abbruch-Test sichert nach game_saves
    dest2 = jreq("GET", "/api/saves/locations")[1]["locations"][0]["dir"]
    shutil.rmtree(dest2, ignore_errors=True)
    st, j = jreq("POST", "/api/saves/backup?lang=en", {"keys": chr(10).join(all5), "dest": dest2})
    seen_cancellable = False
    t_end = time.time() + 20
    while time.time() < t_end:
        _, j = jreq("GET", "/api/status")
        if j.get("busy") and j.get("percent", 0) >= 2:
            seen_cancellable = bool(j.get("cancellable"))
            break
        time.sleep(0.02)
    check("während der Sicherung ist Abbrechen angeboten", seen_cancellable, str(j))
    st, _, _b = req("POST", "/api/cancel", b"{}", {"Content-Type": "application/json"})
    res = wait_job()
    check("Abbrechen: Auftrag endet mit Meldung 'Cancelled', cancelled=true", res and not res["ok"] and res.get("cancelled") and "Cancelled" in res["message"], str(res))
    left = [os.path.join(d, f) for d, _, fs in os.walk(dest2) for f in fs]
    check("Abbrechen: keine halbe Datei und keine .tmp bleibt", not [f for f in left if f.endswith(".tmp")] and all(zipfile.ZipFile(f).testzip() is None for f in left), str(left))
    check("Abbrechen: weniger als alle gesichert", len(left) < NALL, str(len(left)))
    # Wiederherstellen aus einer Sicherung abbrechen: Original bleibt unverändert
    st, j = jreq("POST", "/api/saves/backup?lang=en", {"keys": K1, "dest": dest2})
    wait_job()
    bk1 = os.path.join(dest2, "Alex_1c3f8a0e", "PPSA01234_sdimg_SAVEDATA00.zip")
    before_r = open(paths[K1], "rb").read()
    st, j = jreq("POST", "/api/saves/restore?lang=en", {"paths": bk1})
    t_end = time.time() + 20
    while time.time() < t_end:
        _, j = jreq("GET", "/api/status")
        if j.get("busy") and j.get("percent", 0) >= 1 and j.get("kind") == "save_restore":
            break
        time.sleep(0.02)
    req("POST", "/api/cancel", b"{}", {"Content-Type": "application/json"})
    res = wait_job()
    after_r = open(paths[K1], "rb").read()
    check("Wiederherstellen abbrechen: entweder fertig oder 'Cancelled' mit unverändertem/ganzem Container und ohne .tmp",
          res and (res["ok"] or "Cancelled" in res["message"]) and hashlib.sha256(after_r).hexdigest() in (hashlib.sha256(before_r).hexdigest(), sums[K1])
          and not [p for p in tree() if p.endswith(".tmp")], str(res))

    print("== PS4-Spielstände ==")
    KP = "1c3f8a0e/CUSA00001/sdimg_PS4DATA#PS4"
    pdir = os.path.join(HOME, "1c3f8a0e", "savedata", "CUSA00001")
    enc = lambda k: k.replace("/", "%2F").replace("#", "%23")
    st, hd, zdata = req("GET", "/api/saves/download?key=" + enc(KP))
    zp = zipfile.ZipFile(io.BytesIO(zdata)) if st == 200 else None
    check("PS4-Sicherung: metadata.json, Container, Schlüsseldatei", zp and zp.namelist() == ["metadata.json", "CUSA00001_sdimg_PS4DATA.img", "CUSA00001_sdimg_PS4DATA.bin"], str(zp and zp.namelist()))
    check("PS4-Sicherung: Inhalte unverändert, metadata nennt PS4", zp and hashlib.sha256(zp.read("CUSA00001_sdimg_PS4DATA.img")).hexdigest() == sums[KP]
          and hashlib.sha256(zp.read("CUSA00001_sdimg_PS4DATA.bin")).hexdigest() == sums[KP + ".bin"] and json.loads(zp.read("metadata.json"))["platform"] == "PS4")
    open(os.path.join(pdir, "sdimg_PS4DATA"), "wb").write(b"\x01" + bytes(5000))
    open(os.path.join(pdir, "PS4DATA.bin"), "wb").write(bytes(96))
    os.chmod(os.path.join(pdir, "PS4DATA.bin"), 0o640)
    st, hd, rb = req("POST", "/api/saves/restore/upload?lang=en", zdata, {"Content-Type": "application/zip"})
    res = wait_job()
    check("PS4 wiederhergestellt (Container und Schlüssel)", st == 200 and res and res["ok"], str(res and res["message"]))
    check("PS4: Container und .bin wie vorher", hashlib.sha256(open(os.path.join(pdir, "sdimg_PS4DATA"), "rb").read()).hexdigest() == sums[KP]
          and hashlib.sha256(open(os.path.join(pdir, "PS4DATA.bin"), "rb").read()).hexdigest() == sums[KP + ".bin"])
    check("PS4: Rechte der vorhandenen .bin bleiben", os.stat(os.path.join(pdir, "PS4DATA.bin")).st_mode & 0o777 == 0o640)
    check("PS4: keine Zwischendateien", not [p for p in tree() if p.endswith(".tmp")])
    # PS4 und PS5 gemeinsam in einer Sammel-ZIP, danach beide beschädigt und zurückgeholt
    st, hd, mixed = req("GET", "/api/saves/download?keys=" + ",".join(enc(k) for k in (K1, KP)))
    open(os.path.join(pdir, "sdimg_PS4DATA"), "wb").write(b"\x01" + bytes(5000))
    open(paths[K1], "wb").write(b"\x02" + bytes(5000))
    st, hd, rb = req("POST", "/api/saves/restore/upload?lang=en", mixed, {"Content-Type": "application/zip"})
    res = wait_job()
    check("Sammel-ZIP mit PS5 und PS4: beide zurück", st == 200 and res and res["ok"] and "restored: 2" in res["message"]
          and hashlib.sha256(open(os.path.join(pdir, "sdimg_PS4DATA"), "rb").read()).hexdigest() == sums[KP] and file_sha("1c3f8a0e", "PPSA01234", "sdimg_SAVEDATA00") == sums[K1], str(res and res["message"]))
    # Abweisungen: Konsolenangabe passt nicht zum Container, Schlüssel fehlt oder hat falsche Größe
    def mk(meta, entries):
        b = io.BytesIO()
        with zipfile.ZipFile(b, "w", zipfile.ZIP_STORED) as zz:
            zz.writestr("metadata.json", json.dumps(meta))
            for n, d in entries:
                zz.writestr(n, d)
        return b.getvalue()
    ps4img = fx.container("p", 1 << 20, ps4=True, uid="1c3f8a0e", title="CUSA00001", name="sdimg_PS4DATA")
    snap = tree()
    base_meta = {"title_id": "CUSA00001", "save_dir": "sdimg_PS4DATA", "uid": "1c3f8a0e", "platform": "PS4"}
    for name, body in (("PS4-Angabe, aber PS5-Container", mk(base_meta, [("CUSA00001_sdimg_PS4DATA.img", fx.container("p", 1 << 20)), ("CUSA00001_sdimg_PS4DATA.bin", bytes(96))])),
                       ("PS5-Angabe, aber PS4-Container", mk({**base_meta, "platform": "PS5"}, [("CUSA00001_sdimg_PS4DATA.img", ps4img)])),
                       ("PS4 ohne Schlüsseldatei", mk(base_meta, [("CUSA00001_sdimg_PS4DATA.img", ps4img)])),
                       ("PS4 mit falsch großer Schlüsseldatei", mk(base_meta, [("CUSA00001_sdimg_PS4DATA.img", ps4img), ("CUSA00001_sdimg_PS4DATA.bin", bytes(95))])),
                       ("PS4 mit Namen ohne sdimg_", mk({**base_meta, "save_dir": "evil"}, [("CUSA00001_evil.img", ps4img), ("CUSA00001_evil.bin", bytes(96))]))):
        st, hd, rb = req("POST", "/api/saves/restore/upload", body, {"Content-Type": "application/zip"})
        wait_job()
        check("abgelehnt: " + name, st == 500, "%s %s" % (st, rb[:100]))
    check("... nichts verändert", tree() == snap)

    print("== Zeitstempel ==")
    KT = "1c3f8a0e/PPSA01234/sdimg_SAVEDATA00"
    pt = paths[KT] if KT in paths else os.path.join(HOME, "1c3f8a0e", "savedata_prospero", "PPSA01234", "sdimg_SAVEDATA00")
    st, hd, ztime = req("GET", "/api/saves/download?key=" + KT.replace("/", "%2F"))
    check("Sicherung enthält die Änderungszeit des Containers", json.loads(zipfile.ZipFile(io.BytesIO(ztime)).read("metadata.json")).get("mtime", "").isdigit())
    T_OLD = 1700000000
    os.utime(pt, (T_OLD, T_OLD))
    open(pt, "r+b").write(b"\x02" + bytes(5000))   # Inhalt ändern ...
    os.utime(pt, (T_OLD, T_OLD))                   # ... Zeit zurück auf den alten Wert
    st, hd, rb = req("POST", "/api/saves/restore/upload?lang=en", ztime, {"Content-Type": "application/zip"})
    wait_job()
    check("Wiederherstellen: Inhalt ersetzt, Änderungszeit der vorhandenen Datei bleibt", st == 200 and file_sha("1c3f8a0e", "PPSA01234", "sdimg_SAVEDATA00") == sums[KT]
          and int(os.stat(pt).st_mtime) == T_OLD, str(int(os.stat(pt).st_mtime)))
    # PS4: Container und Schlüsseldatei behalten ihre Zeiten, die .bin unabhängig vom Container
    pbin = os.path.join(pdir, "PS4DATA.bin")
    pcont = os.path.join(pdir, "sdimg_PS4DATA")
    os.utime(pcont, (1650000000, 1650000000))
    os.utime(pbin, (1640000000, 1640000000))
    st, hd, rb = req("POST", "/api/saves/restore/upload?lang=en", zdata, {"Content-Type": "application/zip"})
    wait_job()
    check("PS4: Container- und Schlüsselzeit bleiben getrennt erhalten", st == 200 and int(os.stat(pcont).st_mtime) == 1650000000 and int(os.stat(pbin).st_mtime) == 1640000000,
          "%s %s" % (os.stat(pcont).st_mtime, os.stat(pbin).st_mtime))
    # Neu angelegter Spielstand bekommt die Zeit aus der Sicherung
    newz = io.BytesIO()
    with zipfile.ZipFile(newz, "w", zipfile.ZIP_STORED) as zz:
        zz.writestr("metadata.json", json.dumps({"title_id": "PPSA66666", "save_dir": "sdimg_NEU", "uid": "3a5b7c9d", "mtime": "1690000000"}))
        zz.writestr("PPSA66666_sdimg_NEU.img", fx.container("t", 1 << 20))
    st, hd, rb = req("POST", "/api/saves/restore/upload", newz.getvalue(), {"Content-Type": "application/zip"})
    wait_job()
    pn = os.path.join(HOME, "3a5b7c9d", "savedata_prospero", "PPSA66666", "sdimg_NEU")
    check("Neuer Spielstand: Zeit aus metadata.json", st == 200 and int(os.stat(pn).st_mtime) == 1690000000, str(os.path.exists(pn) and os.stat(pn).st_mtime))
    shutil.rmtree(os.path.dirname(pn))
    # Reparatur-Aufruf
    st, j = jreq("POST", "/api/saves/times", {"key": KT, "mtime": 1600000000})
    check("times: Zeit des Containers gesetzt", st == 200 and int(os.stat(pt).st_mtime) == 1600000000, "%s %s" % (st, j))
    st, j = jreq("POST", "/api/saves/times", {"key": KP, "mtime": 1610000000, "key_mtime": 1620000000})
    check("times (PS4): Container und Schlüssel getrennt", st == 200 and int(os.stat(pcont).st_mtime) == 1610000000 and int(os.stat(pbin).st_mtime) == 1620000000, "%s %s" % (st, j))
    snap = tree()
    for name, body in (("unbekannter Schlüssel", {"key": "9/9/9", "mtime": 1600000000}), ("Zeit fehlt", {"key": KT}),
                       ("Zeit unsinnig (1970)", {"key": KT, "mtime": 5}), ("Zeit unsinnig (Zukunft)", {"key": KT, "mtime": 99999999999}),
                       ("Zeit keine Zahl", {"key": KT, "mtime": "abc"}), ("Pfad-Ausbruch im Schlüssel", {"key": "../../../etc/passwd", "mtime": 1600000000})):
        st, j = jreq("POST", "/api/saves/times", body)
        check("times abgelehnt: " + name, st in (400, 500), "%s %s" % (st, j))
    check("... nichts verändert (Zeiten wie vorher)", int(os.stat(pt).st_mtime) == 1600000000)
    os.makedirs(os.path.join(SAND, "PPSA11111_000"))
    check("times bei laufendem Spiel 409", jreq("POST", "/api/saves/times", {"key": KT, "mtime": 1600000001})[0] == 409)
    shutil.rmtree(os.path.join(SAND, "PPSA11111_000"))

    # ------------------------------------------------------------------ böse und kaputte ZIPs
    print("== abgelehnte ZIPs (nichts darf sich ändern) ==")
    snap = tree()

    def mkzip(meta, entries, method=zipfile.ZIP_STORED):
        b = io.BytesIO()
        with zipfile.ZipFile(b, "w", method) as zz:
            if meta is not None:
                zz.writestr("metadata.json", meta if isinstance(meta, str) else json.dumps(meta))
            for n, d in entries:
                zz.writestr(n, d)
        return b.getvalue()

    img = fx.container("x", 1 << 20)
    good_meta = {"title_id": "PPSA01234", "save_dir": "sdimg_SAVEDATA00", "uid": "1c3f8a0e"}
    cases = [
        ("Müll statt ZIP", b"hallo" * 100),
        ("zu kurz (21 Byte)", b"PK" * 10 + b"x"),
        ("abgeschnittenes ZIP", data[:len(data) // 2]),
        ("ohne metadata.json", mkzip(None, [("PPSA01234_sdimg_SAVEDATA00.img", img)])),
        ("Container fehlt", mkzip(good_meta, [])),
        ("Container zu klein", mkzip(good_meta, [("PPSA01234_sdimg_SAVEDATA00.img", b"\x02" * 100)])),
        ("PS4-Container (Byte 0 = 1)", mkzip(good_meta, [("PPSA01234_sdimg_SAVEDATA00.img", b"\x01" + img[1:])])),
        ("komprimiert (Deflate)", mkzip(good_meta, [("PPSA01234_sdimg_SAVEDATA00.img", img)], zipfile.ZIP_DEFLATED)),
        ("Pfad-Ausbruch title_id ../..", mkzip({**good_meta, "title_id": "../.."}, [("../.._sdimg_SAVEDATA00.img", img)])),
        ("Pfad-Ausbruch save_dir a/b", mkzip({**good_meta, "save_dir": "a/b"}, [("PPSA01234_a/b.img", img)])),
        ("Pfad-Ausbruch save_dir ..", mkzip({**good_meta, "save_dir": ".."}, [("PPSA01234_...img", img)])),
        ("Pfad-Ausbruch uid ..", mkzip({**good_meta, "uid": ".."}, [("PPSA01234_sdimg_SAVEDATA00.img", img)])),
        ("uid mit Schrägstrich", mkzip({**good_meta, "uid": "1c3f8a0e/x"}, [("PPSA01234_sdimg_SAVEDATA00.img", img)])),
        ("uid zu lang (wird nicht abgeschnitten)", mkzip({**good_meta, "uid": "1c3f8a0e1c3f8a0e1c3f8a0e"}, [("PPSA01234_sdimg_SAVEDATA00.img", img)])),
        ("Benutzer existiert nicht", mkzip({**good_meta, "uid": "ffffffff"}, [("PPSA01234_sdimg_SAVEDATA00.img", img)])),
        ("Name mit Punkt am Anfang", mkzip({**good_meta, "save_dir": ".versteckt"}, [("PPSA01234_.versteckt.img", img)])),
        ("metadata.json kein JSON", mkzip("kein json", [("PPSA01234_sdimg_SAVEDATA00.img", img)])),
        ("metadata.json: title_id Zahl", mkzip('{"title_id":5,"save_dir":"a","uid":"1c3f8a0e"}', [("5_a.img", img)])),
    ]
    for name, body in cases:
        st, hd, rb = req("POST", "/api/saves/restore/upload", body, {"Content-Type": "application/zip"})
        wait_job()
        try:
            msg = json.loads(rb).get("error", "")
        except ValueError:
            msg = rb[:80]
        check("abgelehnt: " + name, st in (413, 500) and msg, "%s %s" % (st, msg))
    # CRC-Fehler: ein Byte im Container ändern, CD-CRC bleibt
    bz = bytearray(data)
    off = bz.find(img[:0] + z.read(names[1])[100:132])
    bz[off + 5] ^= 0xFF
    st, hd, rb = req("POST", "/api/saves/restore/upload?lang=en", bytes(bz), {"Content-Type": "application/zip"})
    wait_job()
    check("abgelehnt: Prüfsummenfehler im Container", st == 500 and b"checksum" in rb.lower(), "%s %s" % (st, rb[:120]))
    check("alle abgelehnten ZIPs haben nichts verändert", tree() == snap, str(set(tree().items()) ^ set(snap.items())))
    check("Original-Container unverändert", open(path1, "rb").read() == orig)
    # Längenangaben
    check("Content-Length fehlt → 413", req("POST", "/api/saves/restore/upload", None, {"Content-Type": "application/zip"})[0] in (413, 411))
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    c.putrequest("POST", "/api/saves/restore/upload")
    c.putheader("Content-Length", str(5 << 30))
    c.endheaders()
    check("Content-Length 5 GiB → 413", c.getresponse().status == 413)
    c.close()
    # Verbindung bricht mitten im Hochladen ab: kein Müll, Server frei
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    c.putrequest("POST", "/api/saves/restore/upload")
    c.putheader("Content-Length", str(len(data)))
    c.putheader("Content-Type", "application/zip")
    c.endheaders(data[:len(data) // 3])
    c.sock.close()
    j = wait_job(70)
    check("abgebrochener Upload: Auftrag endet mit Fehler", j is not None and not j["ok"] and j["kind"] == "save_restore",
          str(j)[:200])
    check("abgebrochener Upload: keine Zwischendatei", not [p for p in tree() if p.endswith((".tmp", "upload.zip"))])
    check("Upload mit fremdem Origin → 403", req("POST", "/api/saves/restore/upload", data,
                                                  {"Content-Type": "application/zip", "Origin": "http://evil.example"})[0] == 403)
    try:   # der Server antwortet vor dem Lesen des Körpers; je nach Zeitpunkt kommt die Antwort oder ein Reset an
        st = req("POST", "/api/saves/restore/upload", data, {"Content-Type": "application/zip", "Host": "evil.example"})[0]
    except OSError:
        st = 403
    check("Upload mit falschem Host abgelehnt (403)", st == 403)

    # ------------------------------------------------------------------ laufendes Spiel
    print("== Schutz bei laufendem Spiel ==")
    os.makedirs(os.path.join(SAND, "PPSA11111_000"))
    snap = tree()
    st, hd, rb = req("POST", "/api/saves/restore/upload", data, {"Content-Type": "application/zip"})
    check("Wiederherstellen bei laufendem Spiel 409", st == 409 and b"PPSA11111" in rb, "%s %s" % (st, rb[:100]))
    check("Sichern bei laufendem Spiel erlaubt (nur lesen)", req("GET", "/api/saves/download?key=" + K2.replace("/", "%2F"))[0] == 200)
    check("nichts verändert", tree() == snap)
    shutil.rmtree(os.path.join(SAND, "PPSA11111_000"))

    # ------------------------------------------------------------------ Verlauf, Sprachen
    print("== Verlauf ==")
    st, j = jreq("GET", "/api/history")
    kinds = [e["kind"] for e in j["entries"]]
    check("Verlauf enthält save_restore", "save_restore" in kinds, str(kinds))
    for lang, want in (("de", "Keine gültige Spielstand-ZIP-Datei"), ("fr", "n'est pas un fichier ZIP")):
        st, hd, rb = req("POST", "/api/saves/restore/upload?lang=" + lang, b"kein zip" * 10, {"Content-Type": "application/zip"})
        wait_job()
        check("Fehlermeldung in " + lang, want in rb.decode(), rb.decode()[:150])
finally:
    # Sanitizer-Meldungen des Servers (Läufe mit HOST_CFLAGS="-fsanitize=address,undefined") zählen als Fehler
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(5)
    except subprocess.TimeoutExpired:
        srv.kill()
    log = open(os.path.join(W, "srv.log"), errors="replace").read()
    if "runtime error" in log or "AddressSanitizer" in log or "LeakSanitizer" in log:
        bad("Sanitizer-Meldung im Server", log[-1500:])
    cleanup()

print("\n%s" % ("ALLES OK" if not fails else "%d FEHLER" % fails))
sys.exit(1 if fails else 0)
