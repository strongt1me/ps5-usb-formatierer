#!/usr/bin/env python3
"""Spielstand-API gegen den Host-Formatierer - das Verfahren des PS5 Cooling Center: Auslesen über das Einhängen einer Kopie
(ZIP mit den Dateien und chives.json), Einspielen in ein neues Abbild mit Besitzer, PARAMS-Spender und Datenbankzeile.
Der Host ersetzt das Einhängen durch Ordner (src/savemnt.c, USBFMT_SAVEMNT_STORE).
Aufruf: saves_test.py <host-binary> [port]   (gebaut mit -DSERVER_PORT=<port>, Standard 18115)"""
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import signal
import sqlite3
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
SYS = os.path.join(ROOT, "sys")
STORE = os.path.join(ROOT, "store")
DATA = os.path.join(W, "data")
SAND = os.path.join(W, "sandbox")
os.makedirs(DATA)
os.makedirs(SAND)
ids = json.loads(subprocess.check_output([sys.executable, os.path.join(HERE, "saves_fixture.py"), ROOT]))
env = dict(os.environ, USBFMT_SAVES=HOME, USBFMT_SYSDATA=SYS, USBFMT_SAVEMNT_STORE=STORE, USBFMT_SLOW="1", USBFMT_DATA=DATA,
           USBFMT_IMAGES="", USBFMT_SANDBOX=SAND, USBFMT_ACCOUNTS="Alex=1122334455667788;Sam=aabbccdd00112233")
srv = subprocess.Popen([BIN], env=env, stdout=open(os.path.join(W, "srv.log"), "w"), stderr=subprocess.STDOUT)
ALEX, SAM, JO = "1c3f8a0e", "2d4e9b1f", "3a5b7c9d"


def cleanup():
    srv.send_signal(signal.SIGTERM)
    try:
        srv.wait(5)
    except subprocess.TimeoutExpired:
        srv.kill()
    shutil.rmtree(W, ignore_errors=True)


def req(method, path, body=None, headers=None):
    path += ("&" if "?" in path else "?") + "lang=de"
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=120)
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
        return st, {"raw": data[:300]}


def wait_job(timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        st, j = jreq("GET", "/api/status")
        if st == 200 and not j.get("busy"):
            return j
        time.sleep(0.05)
    return None


def last_result():
    st, j = jreq("GET", "/api/status")
    return j.get("last") or j.get("result") or j


def run_job(method, path, body):
    st, j = jreq(method, path, body)
    res = wait_job() if st in (200, 202) else None
    r = res.get("last") if res and res.get("last") else res
    return st, j, r


def upload(zbytes, uid=None, batch=None):
    q = "/api/saves/restore/upload" + ("?uid=" + uid if uid else "")
    if batch:
        q += ("&" if "?" in q else "?") + "batch=" + batch
    return req("POST", q, zbytes, {"Content-Type": "application/zip", "Content-Length": str(len(zbytes))})


for _ in range(100):
    try:
        if req("GET", "/api/info")[0] == 200:
            break
    except OSError:
        time.sleep(0.1)
else:
    print("FEHLER  Server startet nicht")
    sys.exit(1)


def img_path(uid, tid, slot, ps4=False):
    return os.path.join(HOME, uid, "savedata" if ps4 else "savedata_prospero", tid, "sdimg_" + slot)


def ident(path):
    with open(path, "rb") as f:
        f.seek(0x100)
        t = f.read(31).decode("ascii", "replace")
    return t[15:31] if t.startswith("usbfmt-host-id:") else None


def stored(path):
    """was ein Einhängen des Abbilds zeigt: rel -> Inhalt"""
    d = os.path.join(STORE, ident(path) or "-")
    out = {}
    for b, _, fs in os.walk(d):
        for f in fs:
            p = os.path.join(b, f)
            out[os.path.relpath(p, d).replace(os.sep, "/")] = open(p, "rb").read()
    return out


def sfo_get(buf, key):
    kt, dt, n = struct.unpack_from("<III", buf, 8)
    for i in range(n):
        ko, fmt, used, cap, do = struct.unpack_from("<HHIII", buf, 20 + i * 16)
        k = buf[kt + ko:buf.index(b"\0", kt + ko)].decode()
        if k == key:
            return buf[dt + do:dt + do + used]
    return None


def zero_acc(buf):
    """das param.sfo, wie es in die ZIP kommt: ACCOUNT_ID 0 (das Konto reist nicht mit), alles andere gleich"""
    b = bytearray(buf)
    kt, dt, n = struct.unpack_from("<III", b, 8)
    for i in range(n):
        ko, fmt, used, cap, do = struct.unpack_from("<HHIII", b, 20 + i * 16)
        if b[kt + ko:b.index(b"\0", kt + ko)] == b"ACCOUNT_ID":
            b[dt + do:dt + do + 8] = bytes(8)
    return bytes(b)


def db_rows(uid, ps4=False, tid=None):
    p = os.path.join(SYS, "savedata" if ps4 else "savedata_prospero", uid, "db", "user", "savedata.db")
    if not os.path.exists(p):
        return None
    db = sqlite3.connect(p)
    db.row_factory = sqlite3.Row
    rows = [dict(r) for r in db.execute("SELECT * FROM savedata" + (" WHERE title_id=?" if tid else ""), (tid,) if tid else ())]
    db.close()
    return rows


def tree():
    out = {}
    for base in (HOME, SYS):
        for d, _, fs in os.walk(base):
            for f in fs:
                p = os.path.join(d, f)
                out[p] = hashlib.sha256(open(p, "rb").read(4096)).hexdigest() + str(os.path.getsize(p))
    return out


def mkzip(entries, method=zipfile.ZIP_STORED):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", method) as z:
        for n, d in entries:
            z.writestr(n, d)
    return b.getvalue()


K1 = ALEX + "/PPSA01234/sdimg_SAVEDATA00"
KP = ALEX + "/CUSA00001/sdimg_PS4DATA#PS4"

try:
    # ------------------------------------------------------------------ Liste
    print("== Liste ==")
    st, j = jreq("GET", "/api/saves")
    keys = [s["key"] for s in j.get("saves", [])]
    check("7 Spielstände (6 PS5, 1 PS4), sortiert, ohne Systemkopien/Fremdes/PS4 ohne Schlüssel", st == 200 and keys == sorted(keys) and len(keys) == 7, str(keys))
    s1 = next((s for s in j.get("saves", []) if s["key"] == K1), {})
    check("Spielname, Benutzer, Konsole", s1.get("title_name") == 'Beispiel-Spiel: Die "Rückkehr"' and s1.get("username") == "Alex" and s1.get("platform") == "PS5", str(s1))
    check("PS4-Eintrag mit #PS4", any(x["key"] == KP and x["platform"] == "PS4" for x in j.get("saves", [])))
    st, j = jreq("GET", "/api/saves/users")
    check("3 Benutzer", st == 200 and len(j.get("users", [])) == 3, str(j))
    check("Zeitstempel-Reparatur gibt es nicht mehr", req("POST", "/api/saves/times", {"key": K1, "mtime": 1700000000})[0] not in (200, 202))

    # ------------------------------------------------------------------ Herunterladen
    print("== Herunterladen (Auslesen über das Einhängen) ==")
    before = tree()
    st, hd, data = req("GET", "/api/saves/download?key=" + K1)
    check("Download 200 application/zip", st == 200 and hd.get("Content-Type") == "application/zip", "%s %s" % (st, data[:200]))
    z = zipfile.ZipFile(io.BytesIO(data)) if st == 200 else None
    names = sorted(z.namelist()) if z else []
    check("ZIP: die Dateien des Spielstands im Wurzelordner und chives.json, kein metadata.json",
          names == sorted(["chives.json", "data/main.sav", "data/sub/extra.bin", "sce_sys/icon0.png", "sce_sys/param.sfo"]), str(names))
    want = fx.content(ALEX, "PPSA01234", "SAVEDATA00")
    check("Dateiinhalte wie im eingehängten Abbild", z and all(z.read(k) == v for k, v in want.items()))
    psfo = z.read("sce_sys/param.sfo") if z else b""
    check("param.sfo ist das echte aus dem Kopf (nicht der Block Nullen), Konto-ID 0, PARAMS bleibt", psfo == zero_acc(fx.save_sfo(ALEX, "PPSA01234", "SAVEDATA00", 64)), str(sfo_get(psfo, "ACCOUNT_ID")))
    ch = json.loads(z.read("chives.json")) if z else {}
    check("chives.json: Aufbau des Abbilds", ch.get("platform") == "PS5" and ch.get("title_id") == "PPSA01234" and ch.get("save_directory") == "SAVEDATA00"
          and ch.get("encrypted_name") == "sdimg_SAVEDATA00" and ch.get("pfs", {}).get("block_count") == 64 and ch.get("pfs", {}).get("block_size_bytes") == 32768
          and ch.get("pfs", {}).get("mode_raw") == 13 and ch.get("pfs", {}).get("fs_ext") == 0 and ch.get("original_image_size_bytes") == 64 * 32768, str(ch)[:300])
    check("chives.json: Besitzerliste mit Rechten", any(o["path"] == "data/main.sav" and o["mode"] == 0o644 for o in ch.get("ownership", [])), str(ch.get("ownership"))[:200])
    check("Download ändert nichts an der Konsole", tree() == before)
    check("keine Reste im Arbeitsordner", not [f for f in os.listdir(os.path.join(DATA, "save_tmp")) if f != "upload.zip"], str(os.listdir(os.path.join(DATA, "save_tmp"))))
    st, hd, data = req("GET", "/api/saves/download?key=" + SAM + "/PPSA01234/sdimg_SAVEDATA00")
    z = zipfile.ZipFile(io.BytesIO(data)) if st == 200 else None
    check("ohne Blob im Kopf: param.sfo aus dem Dateisystem", z and z.read("sce_sys/param.sfo") == zero_acc(fx.save_sfo(SAM, "PPSA01234", "SAVEDATA00", 64)), str(st))
    st, hd, data = req("GET", "/api/saves/download?key=" + KP.replace("#", "%23"))
    z = zipfile.ZipFile(io.BytesIO(data)) if st == 200 else None
    check("PS4: Dateien ohne chives.json (wie Garlic SaveMgr)", z and "chives.json" not in z.namelist() and z.read("data/main.sav") == fx.content(ALEX, "CUSA00001", "PS4DATA")["data/main.sav"],
          "%s %s" % (st, z and z.namelist()))
    check("PS4: Rechte der Dateien in der ZIP", z and (z.getinfo("data/main.sav").external_attr >> 16) & 0o7777 == 0o640)
    st, hd, data = req("GET", "/api/saves/download?keys=" + K1 + "," + ALEX + "/PPSA07890/sdimg_SAVEDATA00")
    z = zipfile.ZipFile(io.BytesIO(data)) if st == 200 else None
    tops = sorted({n.split("/")[0] for n in z.namelist()}) if z else []
    check("Sammel-ZIP: je Spielstand ein Ordner mit chives.json", tops == [ALEX + "_PPSA01234_sdimg_SAVEDATA00", ALEX + "_PPSA07890_sdimg_SAVEDATA00"]
          and all(t + "/chives.json" in z.namelist() for t in tops), str(tops))
    check("unbekannter Schlüssel 404", req("GET", "/api/saves/download?key=nope")[0] == 404)
    st, hd, data = req("GET", "/api/saves/download?key=" + K1 + "&check=1")
    check("Vorprüfung (check=1): 200 mit Zahl, ohne ZIP", st == 200 and json.loads(data).get("count") == 1, data[:120])
    longkey = ",".join([K1] * 60)
    st, hd, data = req("GET", "/api/saves/download?keys=" + longkey + "&check=1")
    check("lange Adresse (60 Schlüssel) wird nicht abgeschnitten", st == 200, "%s %s" % (st, data[:120]))
    st, hd, data = req("GET", "/api/saves/download?keys=" + ",".join([K1] * 120) + "&check=1")
    check("zu lange Adresse: 414, nie gekürzt", st == 414, "%s %s" % (st, data[:120]))
    os.makedirs(os.path.join(SAND, "PPSA11111_000"))
    check("Download bei laufendem Spiel 409", req("GET", "/api/saves/download?key=" + K1)[0] == 409)
    shutil.rmtree(os.path.join(SAND, "PPSA11111_000"))
    open(os.path.join(STORE, ".fail_open"), "w").close()
    st, hd, data = req("GET", "/api/saves/download?key=" + K1)
    check("Einhängen scheitert: 500 mit der Antwort der Konsole, nichts bleibt liegen", st == 500 and "eingehängt" in data.decode("utf-8", "replace")
          and not [f for f in os.listdir(os.path.join(DATA, "save_tmp")) if f.startswith("sdimg_")], data[:200].decode("utf-8", "replace"))

    # ------------------------------------------------------------------ Sichern und Liste der Sicherungen
    print("== Sichern ==")
    st, j = jreq("GET", "/api/saves/locations")
    dest = j["locations"][0]["dir"] if st == 200 and j.get("locations") else ""
    st, j, res = run_job("POST", "/api/saves/backup", {"keys": K1 + "\n" + KP, "dest": dest})
    check("Sichern: Auftrag ok", st == 202 and res and res.get("ok"), str(res)[:300])
    z1 = os.path.join(dest, "Alex_" + ALEX, "PPSA01234_sdimg_SAVEDATA00.zip")
    zp = os.path.join(dest, "Alex_" + ALEX, "CUSA00001_sdimg_PS4DATA.zip")
    check("Sicherungen liegen in <Benutzer>_<uid>/", os.path.exists(z1) and os.path.exists(zp))
    st, j, res = run_job("POST", "/api/saves/backup", {"keys": ALEX + "/PPSA09999/sdimg_EXTRA2", "dest": dest})
    check("Sichern eines Spielstands sichert alle Spielstände des Spiels, je eine ZIP",
          res and res.get("ok") and all(os.path.exists(os.path.join(dest, "Alex_" + ALEX, "PPSA09999_sdimg_%s.zip" % n)) for n in ("EXTRA", "EXTRA2")), str(res)[:300])
    check("Sicherung = dasselbe Format wie der Download", os.path.exists(z1) and "chives.json" in zipfile.ZipFile(z1).namelist() and "metadata.json" not in zipfile.ZipFile(z1).namelist())
    st, j = jreq("GET", "/api/saves/backups")
    bk = {b["save_dir"] + "/" + b["title_id"]: b for b in j.get("backups", [])}
    b1 = bk.get("sdimg_SAVEDATA00/PPSA01234", {})
    check("Sicherungsliste: Titel, Name, Benutzer, Konsole, Spielname", b1.get("uid") == ALEX and b1.get("username") == "Alex" and b1.get("platform") == "PS5"
          and b1.get("title_name") == 'Beispiel-Spiel: Die "Rückkehr"' and b1.get("size", 0) > 1000, str(b1))
    check("Sicherungsliste: Zeit des Spielstands = Zeit des Abbilds (Kommentar der ZIP)",
          b1.get("save_mtime") == int(os.path.getmtime(img_path(ALEX, "PPSA01234", "SAVEDATA00"))), "%s %s" % (b1.get("save_mtime"), os.path.getmtime(img_path(ALEX, "PPSA01234", "SAVEDATA00"))))
    check("Sicherungsliste: PS4", bk.get("sdimg_PS4DATA/CUSA00001", {}).get("platform") == "PS4", str(list(bk)))
    for _ in range(3):
        time.sleep(1.1)
        run_job("POST", "/api/saves/backup", {"keys": K1, "dest": dest})
    vers = [f for f in os.listdir(os.path.dirname(z1)) if re.fullmatch(r"PPSA01234_sdimg_SAVEDATA00\.\d{8}-\d{6}\.zip", f)]
    check("die neueste und zwei Vorversionen bleiben", len(vers) == 2, str(vers))
    alt = os.path.join(dest, "Alex_" + ALEX, "alt.zip")
    with zipfile.ZipFile(alt, "w") as z:
        z.writestr("metadata.json", "{}")
        z.writestr("PPSA01234_sdimg_SAVEDATA00.img", b"\2" * 8192)
    check("Sicherungen im alten Format (Container) werden nicht mehr angeboten", not any(b["path"] == alt for b in jreq("GET", "/api/saves/backups")[1].get("backups", [])))
    os.makedirs(os.path.join(SAND, "PPSA11111_000"))
    check("Sichern bei laufendem Spiel 409", jreq("POST", "/api/saves/backup", {"keys": K1, "dest": dest})[0] == 409)
    shutil.rmtree(os.path.join(SAND, "PPSA11111_000"))

    # ------------------------------------------------------------------ Einspielen (eigener Spielstand zurück)
    print("== Einspielen ==")
    p1 = img_path(ALEX, "PPSA01234", "SAVEDATA00")
    old_id = ident(p1)
    st, j, res = run_job("POST", "/api/saves/restore", {"paths": z1})
    check("Einspielen aus der Sicherung: Auftrag ok", st == 202 and res and res.get("ok"), str(res)[:400])
    vz = sorted(f for f in os.listdir(os.path.dirname(z1)) if re.fullmatch(r"PPSA01234_sdimg_SAVEDATA00\.\d{8}-\d{6}\.zip", f))
    snap_r = tree()
    st, j, rd = run_job("POST", "/api/saves/restore", {"paths": z1 + "\n" + os.path.join(os.path.dirname(z1), vz[0])}) if vz else (0, {}, None)
    st, j, rj = run_job("POST", "/api/saves/restore", {"paths": z1, "uid": "../x"})
    check("ungültige uid beim Einspielen aus Sicherungen: 400", st == 400, str(j)[:200])
    check("zwei Sicherungen desselben Spielstands auf einmal: abgelehnt, nichts verändert", rd and not rd.get("ok") and "denselben" in rd.get("message", "")
          and tree() == snap_r, str(rd)[:300])
    new_id = ident(p1)
    check("ein NEUES Abbild ersetzt das alte (andere Kennung)", new_id and new_id != old_id, "%s %s" % (old_id, new_id))
    undo = [f for f in os.listdir(os.path.dirname(z1)) if re.fullmatch(r"PPSA01234_sdimg_SAVEDATA00\.vorher-\d{8}-\d{6}\.zip", f)]
    check("Stand davor gesichert (….vorher-<Zeit>.zip), die Quelle der Wiederherstellung bleibt", len(undo) == 1 and os.path.exists(z1), str(undo))
    check("... die Sicherung des Stands davor enthält den alten Spielstand", undo and
          zipfile.ZipFile(os.path.join(os.path.dirname(z1), undo[0])).read("data/main.sav") == want["data/main.sav"])
    check("... Meldung nennt den gesicherten Stand", "Stand davor" in (res or {}).get("message", ""), (res or {}).get("message", "")[:300])
    check("... und er steht in der Liste der Sicherungen", any(b["path"].endswith(undo[0]) for b in jreq("GET", "/api/saves/backups")[1].get("backups", [])) if undo else False)
    got = stored(p1)
    check("neues Abbild enthält die Dateien", all(got.get(k) == v for k, v in want.items()), str(sorted(got)))
    ns = got.get("sce_sys/param.sfo", b"")
    check("param.sfo im neuen Abbild: Konto aus der Registry", sfo_get(ns, "ACCOUNT_ID") == struct.pack("<Q", 0x1122334455667788), str(sfo_get(ns, "ACCOUNT_ID")))
    pr = sfo_get(ns, "PARAMS") or b""
    check("PARAMS: Benutzer-ID und Besitzerwert des Benutzers", pr[4:8] == struct.pack("<I", int(ALEX, 16)) and pr[8:40] == fx.user_value(ALEX))
    check("... Spender ist der eigene Spielstand, sein param.sfo nur im Kopf des Abbilds (ganzes PARAMS, kein Hinweis)",
          pr[0x100:0x110] == hashlib.md5(b"PPSA01234SAVEDATA00").digest() and "anderen Spielstand" not in (res or {}).get("message", ""),
          (res or {}).get("message", "")[:300])
    rows = db_rows(ALEX, tid="PPSA01234")
    r0 = rows[0] if rows else {}
    check("Datenbank: Zeile aktualisiert (keine zweite)", rows and len(rows) == 1 and r0["main_title"] == 'Beispiel-Spiel: Die "Rückkehr"' and r0["sub_title"] == "Kapitel 3"
          and r0["account_id"] == 0x1122334455667788 and r0["user_id"] == int(ALEX, 16) and r0["blocks"] == 64 and r0["size_kib"] == 2048 and r0["user_param"] == 7
          and r0["is_broken"] == 0 and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.00Z", r0["mtime"]), str(r0))
    dbb = os.listdir(os.path.join(DATA, "save_db_backup")) if os.path.isdir(os.path.join(DATA, "save_db_backup")) else []
    check("Datenbank vorher gesichert", any(f.startswith("savedata-" + ALEX + "-ps5-") for f in dbb), str(dbb))
    icon = os.path.join(HOME, ALEX, "savedata_prospero_meta", "user", "PPSA01234", "SAVEDATA00_icon0.png")
    check("Bild des Spielstands abgelegt", os.path.exists(icon) and open(icon, "rb").read() == want["sce_sys/icon0.png"])
    left = [f for f in os.listdir(os.path.dirname(p1)) if "usbfmt-" in f]
    check("keine .usbfmt-neu/-alt übrig", not left, str(left))
    check("Arbeitsordner leer", not [f for f in os.listdir(os.path.join(DATA, "save_tmp")) if f != "upload.zip"], str(os.listdir(os.path.join(DATA, "save_tmp"))))

    # ------------------------------------------------------------------ für einen anderen Benutzer
    print("== für einen anderen Benutzer ==")
    st, j, res = run_job("POST", "/api/saves/restore", {"paths": z1, "uid": SAM})
    check("Alex' Spielstand für Sam: ok", st == 202 and res and res.get("ok"), str(res)[:400])
    ps = img_path(SAM, "PPSA01234", "SAVEDATA00")
    gs = stored(ps)
    ns = gs.get("sce_sys/param.sfo", b"")
    pr = sfo_get(ns, "PARAMS") or b""
    check("Sams Abbild: Konto und Benutzer von Sam", sfo_get(ns, "ACCOUNT_ID") == struct.pack("<Q", 0xAABBCCDD00112233) and pr[4:8] == struct.pack("<I", int(SAM, 16)))
    check("Besitzerwert aus Sams eigenem Spielstand (derselbe Titel und Name: ganzes PARAMS)", pr[8:40] == fx.user_value(SAM) and
          pr[0x100:0x110] == hashlib.md5(b"PPSA01234SAVEDATA00").digest())
    rows = db_rows(SAM, tid="PPSA01234")
    check("Sams Datenbank wird mit dem Aufbau der Konsole angelegt, Zeile neu", rows and len(rows) == 1 and rows[0]["game_title_id"] == "PPSA01234"
          and rows[0]["account_id_in_usb_path"] == "0000000000000000", str(rows))
    st, j, res = run_job("POST", "/api/saves/restore", {"paths": zp, "uid": SAM})
    check("PS4 für Sam ohne PS4-Datenbankordner: abgelehnt, nichts verändert", st == 202 and res and not res.get("ok") and "Datenbank" in res.get("message", "")
          and not os.path.exists(os.path.join(HOME, SAM, "savedata", "CUSA00001", "sdimg_PS4DATA")), str(res)[:300])
    # anderer Titel: Spender aus einem anderen Spiel desselben Benutzers (nur Bytes 4..40), Hinweis
    st, hd, data = req("GET", "/api/saves/download?key=" + ALEX + "/PPSA09999/sdimg_EXTRA")
    zx = zipfile.ZipFile(io.BytesIO(data)) if st == 200 else None
    check("ein Spielstand gewählt: alle Spielstände des Spiels kommen mit (wie im Cooling Center)",
          zx and sorted({n.split("/")[0] for n in zx.namelist()}) == [ALEX + "_PPSA09999_sdimg_EXTRA", ALEX + "_PPSA09999_sdimg_EXTRA2"], str(zx and zx.namelist()))
    st, hd, body = upload(data, SAM)
    check("Hochladen für Sam: 200", st == 200, body[:300].decode("utf-8", "replace"))
    msg = (wait_job() or {}).get("message", "")
    check("Hinweis: Besitzerangaben aus einem anderen Spielstand", "anderen Spielstand" in msg, msg[:300])
    check("... bei zwei Speicherplätzen nur einmal", msg.count("anderen Spielstand") == 1, msg[:600])
    gx = stored(img_path(SAM, "PPSA09999", "EXTRA"))
    px = sfo_get(gx.get("sce_sys/param.sfo", b""), "PARAMS") or b""
    check("... Besitzerwert von Sam, eigener Teil des Spielstands bleibt", px[8:40] == fx.user_value(SAM) and px[0x100:0x110] == hashlib.md5(b"PPSA09999EXTRA").digest())
    st, hd, body = upload(data, JO)
    check("Jo hat keinen Datenbank-Ordner: abgelehnt", st == 500 and "Datenbank" in body.decode("utf-8", "replace"), body[:200].decode("utf-8", "replace"))
    st, hd, body = upload(data)
    check("ohne Benutzer bei mehreren Benutzern: Rückfrage", st == 500 and "Benutzer" in body.decode("utf-8", "replace"), body[:200].decode("utf-8", "replace"))
    st, hd, body = upload(data, "../../x")
    check("ungültige uid 400", st == 400)

    # ------------------------------------------------------------------ PS4
    print("== Apps, Spiel ganz oder gar nicht ==")
    check("Apps (param.json, NPXS) werden nicht gelistet", not [s for s in jreq("GET", "/api/saves")[1].get("saves", []) if s["title_id"] in ("PPSA88888", "NPXS20001")])
    appzip = mkzip([("sce_sys/param.sfo", fx.save_sfo(ALEX, "PPSA88888", "APPDATA", 40)), ("data/x", b"1"),
                    ("chives.json", json.dumps({"schema_version": 1, "platform": "PS5", "title_id": "PPSA88888", "save_directory": "APPDATA",
                                                "encrypted_name": "sdimg_APPDATA", "original_image_size_bytes": 40 * 32768,
                                                "pfs": {"version": 2, "block_size_bytes": 32768, "block_count": 40, "mode_raw": 13,
                                                        "backup_header_count": 1, "fs_ext": 0}}))])
    st, hd, body = upload(appzip, ALEX)
    check("Spielstand einer App wird nicht eingespielt", st == 500 and "App" in body.decode("utf-8", "replace"), body[:200].decode("utf-8", "replace"))
    adir = os.path.join(dest, "Alex_" + ALEX)
    ze = os.path.join(adir, "PPSA09999_sdimg_EXTRA.zip")
    before_e = open(ze, "rb").read()
    vers_e = sorted(os.listdir(adir))
    id2 = ident(img_path(ALEX, "PPSA09999", "EXTRA2"))
    os.rename(os.path.join(STORE, id2 + ".param"), os.path.join(STORE, id2 + ".param.weg"))   # EXTRA2 lässt sich nicht auslesen
    os.rename(os.path.join(STORE, id2), os.path.join(STORE, id2 + ".weg"))
    time.sleep(1.1)
    st, j, res = run_job("POST", "/api/saves/backup", {"keys": ALEX + "/PPSA09999/sdimg_EXTRA", "dest": dest})
    check("ein Spielstand des Spiels scheitert: Auftrag scheitert, Meldung sagt es", res and not res.get("ok") and "entfernt" in res.get("message", ""), str(res)[:300])
    check("... die eben geschriebene Sicherung des anderen Spielstands ist zurückgenommen, die bisherige wieder da",
          open(ze, "rb").read() == before_e and sorted(os.listdir(adir)) == vers_e, str(sorted(set(os.listdir(adir)) ^ set(vers_e))))
    os.rename(os.path.join(STORE, id2 + ".param.weg"), os.path.join(STORE, id2 + ".param"))
    os.rename(os.path.join(STORE, id2 + ".weg"), os.path.join(STORE, id2))

    print("== mehrere Dateien eines Spiels (Reihe) ==")
    adir = os.path.join(dest, "Alex_" + ALEX)
    def undos(slot):
        return [f for f in os.listdir(adir) if re.fullmatch(r"PPSA09999_sdimg_%s\.vorher-\d{8}-\d{6}\.zip" % slot, f)]
    u0 = (len(undos("EXTRA")), len(undos("EXTRA2")))
    time.sleep(1.1)
    za, zb = (open(os.path.join(adir, "PPSA09999_sdimg_%s.zip" % n), "rb").read() for n in ("EXTRA", "EXTRA2"))
    ra = upload(za, ALEX, "reihe1")
    rb = upload(zb, ALEX, "reihe1")
    check("zwei Dateien einer Reihe: beide eingespielt", ra[0] == 200 and rb[0] == 200, "%s %s" % (ra[0], rb[0]))
    u1 = (len(undos("EXTRA")), len(undos("EXTRA2")))
    check("... der Stand davor nur einmal je Spiel gesichert (beide Spielstände, einmal)", u1 == (u0[0] + 1, u0[1] + 1), "%s -> %s" % (u0, u1))
    time.sleep(1.1)
    upload(za, ALEX, "reihe2")
    u2 = (len(undos("EXTRA")), len(undos("EXTRA2")))
    check("... eine neue Reihe sichert wieder", u2 == (min(u1[0] + 1, 3), min(u1[1] + 1, 3)), "%s -> %s" % (u1, u2))
    rd2 = upload(za, ALEX, "reihe2")
    check("derselbe Spielstand zweimal in einer Reihe: 409 (Konflikt, kein Serverfehler)", rd2[0] == 409 and "denselben" in rd2[2].decode("utf-8", "replace"),
          "%s %s" % (rd2[0], rd2[2][:200]))
    st, hd, body = req("POST", "/api/saves/restore/upload?uid=" + ALEX, za, {"Content-Type": "text/plain", "Content-Length": str(len(za))})
    check("Hochladen ohne passenden Content-Type: 415 (die Vorabfrage des Browsers schützt vor fremden Seiten)", st == 415, "%s %s" % (st, body[:200]))
    st, hd, body = upload(za, ALEX + "0123456789abc")
    check("zu lange uid beim Hochladen: abgelehnt, nicht still gekürzt", st == 400, "%s %s" % (st, body[:200]))

    print("== PS4 ==")
    pp = img_path(ALEX, "CUSA00001", "PS4DATA", ps4=True)
    oldbin = open(os.path.join(os.path.dirname(pp), "PS4DATA.bin"), "rb").read()
    st, j, res = run_job("POST", "/api/saves/restore", {"paths": zp})
    check("PS4 eingespielt", st == 202 and res and res.get("ok"), str(res)[:300])
    newbin = open(os.path.join(os.path.dirname(pp), "PS4DATA.bin"), "rb").read()
    check("PS4: neuer Schlüssel in PS4DATA.bin (96 Byte)", newbin != oldbin and len(newbin) == 96)
    with open(pp, "rb") as f:
        check("PS4: Abbild Version 1", struct.unpack("<Q", f.read(8))[0] == 1)
    check("PS4: Dateien im neuen Abbild", stored(pp).get("data/main.sav") == fx.content(ALEX, "CUSA00001", "PS4DATA")["data/main.sav"])
    rows = db_rows(ALEX, ps4=True, tid="CUSA00001")
    check("PS4: Zeile in savedata/<uid>/db", rows and len(rows) == 1 and rows[0]["account_id"] == 0x1122334455667788, str(rows))

    # ------------------------------------------------------------------ fremde ZIP-Dateien (Cooling Center / Garlic)
    print("== fremde und kaputte ZIP-Dateien ==")
    st, hd, data = req("GET", "/api/saves/download?key=" + ALEX + "/PPSA07890/sdimg_SAVEDATA00")
    zsrc = zipfile.ZipFile(io.BytesIO(data))
    defl = mkzip([("PPSA07890_SAVEDATA00/" + n, zsrc.read(n)) for n in zsrc.namelist()], zipfile.ZIP_DEFLATED)
    st, hd, body = upload(defl, ALEX)
    check("Deflate-ZIP mit Ordner (wie Garlic SaveMgr): eingespielt", st == 200, body[:300].decode("utf-8", "replace"))
    snap = tree()
    cases = [
        ("ohne param.sfo", mkzip([("data/x", b"1")])),
        ("PS5 ohne chives.json", mkzip([(n, zsrc.read(n)) for n in zsrc.namelist() if n != "chives.json"])),
        ("Datei außerhalb des Spielstands", mkzip([("a/" + n, zsrc.read(n)) for n in zsrc.namelist()] + [("fremd.txt", b"x")])),
        ("ineinander liegende Spielstände", mkzip([(n, zsrc.read(n)) for n in zsrc.namelist()] + [("x/sce_sys/param.sfo", zsrc.read("sce_sys/param.sfo"))])),
        ("Blockzahl passt nicht", mkzip([(n, zsrc.read(n) if n != "chives.json" else zsrc.read(n).replace(b'"block_count": 40', b'"block_count": 41')) for n in zsrc.namelist()])),
        ("Pfad-Ausbruch", mkzip([("../" + n, zsrc.read(n)) for n in zsrc.namelist()])),
        ("derselbe Spielstand zweimal", mkzip([(d + "/" + n, zsrc.read(n)) for d in ("a", "b") for n in zsrc.namelist()])),
        ("keine ZIP", b"PK\3\4" + b"x" * 100),
    ]
    for name, zb in cases:
        st, hd, body = upload(zb, ALEX)
        check("abgelehnt: " + name, st in (400, 409, 500), "%s %s" % (st, body[:200]))
    bz = bytearray(defl)
    i = bz.find(b"PPSA07890_SAVEDATA00/data/main.sav")
    bz2 = bytearray(mkzip([(n, zsrc.read(n)) for n in zsrc.namelist()]))
    k = bz2.find(fx.content(ALEX, "PPSA07890", "SAVEDATA00")["data/main.sav"][:64])
    bz2[k + 10] ^= 0xFF
    st, hd, body = upload(bytes(bz2), ALEX)
    check("abgelehnt: Prüfsummenfehler", st == 500 and "Prüfsumme" in body.decode("utf-8", "replace"), body[:200].decode("utf-8", "replace"))
    check("abgelehnte ZIP-Dateien ändern nichts an der Konsole", tree() == snap)
    # Einhängen/Anlegen scheitert: nichts verändert
    open(os.path.join(STORE, ".fail_create"), "w").close()
    st, hd, body = upload(data, ALEX)
    check("Anlegen des Abbilds scheitert: Fehler, nichts verändert", st == 500 and tree() == snap, body[:200].decode("utf-8", "replace"))
    # Datenbank kaputt -> nichts verändert, alles zurück
    dbp = os.path.join(SYS, "savedata_prospero", ALEX, "db", "user", "savedata.db")
    good = open(dbp, "rb").read()
    db = sqlite3.connect(dbp)
    db.executescript("DROP TABLE savedata; CREATE TABLE savedata (id INTEGER PRIMARY KEY, title_id, dir_name);")
    db.close()
    snap = tree()
    st, hd, body = upload(data, ALEX)
    check("Datenbank mit fremdem Aufbau: abgelehnt, Abbild und Datenbank wie vorher", st == 500 and tree() == snap, body[:300].decode("utf-8", "replace"))
    open(dbp, "wb").write(good)
    # laufendes Spiel
    os.makedirs(os.path.join(SAND, "PPSA11111_000"))
    check("Einspielen bei laufendem Spiel 409", upload(data, ALEX)[0] == 409 and jreq("POST", "/api/saves/restore", {"paths": z1})[0] == 409)
    shutil.rmtree(os.path.join(SAND, "PPSA11111_000"))
    # Rest eines abgebrochenen Laufs im Arbeitsordner
    mnt = os.path.join(DATA, "save_tmp", "mnt")
    os.makedirs(os.path.join(mnt, "rest"), exist_ok=True)
    open(os.path.join(mnt, "rest", "x"), "w").close()
    st, hd, body = upload(data, ALEX)
    check("Rest eines früheren Einhängens wird gelöst, dann eingespielt", st == 200 and not os.path.exists(mnt), body[:200].decode("utf-8", "replace"))
    # Sammel-ZIP: zwei Spielstände
    st, hd, multi = req("GET", "/api/saves/download?keys=" + K1 + "," + ALEX + "/PPSA07890/sdimg_SAVEDATA00")
    st, hd, body = upload(multi, ALEX)
    msg = (wait_job() or {}).get("message", "")
    check("Sammel-ZIP: beide eingespielt", st == 200 and "2" in msg, msg[:200])
    # Aufzeichnung eines abgebrochenen Tauschs: "swap" -> zurücknehmen, "db" -> zu Ende führen (beim nächsten Einspielen)
    pf = img_path(ALEX, "PPSA07890", "SAVEDATA00")
    old_bytes = open(pf, "rb").read()
    os.rename(pf, pf + ".usbfmt-alt")
    open(pf, "wb").write(b"NEU" * 4096)
    jdir = DATA
    ifin = os.path.join(HOME, ALEX, "savedata_prospero_meta", "user", "PPSA07890", "SAVEDATA00_icon0.png")
    dbp7 = os.path.join(SYS, "savedata_prospero", ALEX, "db", "user", "savedata.db")
    jr = {"v": 1, "phase": "swap", "fin": pf, "ifin": ifin, "dbp": dbp7, "ps4": 0, "replaced": 1, "bin_replaced": 0, "icon_existed": 1,
          "title_id": "PPSA07890", "dir_name": "SAVEDATA00", "main_title": "x", "sub_title": "", "detail": "", "mtime": "2026-01-01T00:00:00.00Z",
          "account_id": "1122334455667788", "user_id": "1c3f8a0e", "user_param": "0", "blocks": "28", "free_blocks": "0", "size_kib": "500", "system_blocks": "0"}
    open(os.path.join(jdir, "save_import.journal"), "w").write(json.dumps(jr))
    st, hd, body = upload(appzip, ALEX)   # irgendein Einspielen: zuerst wird das Offene erledigt (die App-ZIP wird danach abgelehnt)
    check("Aufzeichnung 'swap': der alte Spielstand liegt wieder an seinem Platz, die Aufzeichnung ist weg",
          open(pf, "rb").read() == old_bytes and not os.path.exists(pf + ".usbfmt-alt") and not os.path.exists(os.path.join(jdir, "save_import.journal")),
          body[:200].decode("utf-8", "replace"))
    os.rename(pf, pf + ".usbfmt-alt")
    shutil.copy(pf + ".usbfmt-alt", pf)
    jr["phase"] = "db"
    jr["main_title"] = "Fertig geschrieben"
    open(os.path.join(jdir, "save_import.journal"), "w").write(json.dumps(jr))
    upload(appzip, ALEX)
    r7 = db_rows(ALEX, tid="PPSA07890")
    check("Aufzeichnung 'db': die Zeile wird geschrieben, die alte Datei entfernt", r7 and r7[0]["main_title"] == "Fertig geschrieben"
          and not os.path.exists(pf + ".usbfmt-alt") and not os.path.exists(os.path.join(jdir, "save_import.journal")), str(r7)[:200])
    # "db", aber das neue Abbild liegt noch daneben (das Umbenennen kam nach einem Stromausfall nicht an): zurücknehmen, nicht "fertig"
    cur = open(pf, "rb").read()
    os.rename(pf, pf + ".usbfmt-alt")
    open(pf, "wb").write(b"ALT?" * 1024)
    open(pf + ".usbfmt-neu", "wb").write(b"NEU" * 1024)
    open(os.path.join(jdir, "save_import.journal"), "w").write(json.dumps(jr))
    upload(appzip, ALEX)
    check("Aufzeichnung 'db' mit liegengebliebenem neuem Abbild: zurückgenommen", open(pf, "rb").read() == cur and not os.path.exists(pf + ".usbfmt-neu")
          and not os.path.exists(pf + ".usbfmt-alt"))
    # "swap", das neue Abbild liegt schon an seinem Platz und ein Spiel hat es seitdem benutzt (jünger als die Aufzeichnung): zu Ende führen
    cur = open(pf, "rb").read()
    os.rename(pf, pf + ".usbfmt-alt")
    open(pf, "wb").write(b"GESPIELT" * 512)
    jr2 = dict(jr, phase="swap", main_title="Nach dem Abbruch gespielt")
    jp = os.path.join(jdir, "save_import.journal")
    open(jp, "w").write(json.dumps(jr2))
    os.utime(jp, (time.time() - 120, time.time() - 120))
    upload(appzip, ALEX)
    r8 = db_rows(ALEX, tid="PPSA07890")
    check("Aufzeichnung 'swap', Abbild seitdem benutzt: nicht zurückgenommen, sondern zu Ende geführt",
          open(pf, "rb").read() == b"GESPIELT" * 512 and not os.path.exists(pf + ".usbfmt-alt") and not os.path.exists(jp)
          and r8 and r8[0]["main_title"] == "Nach dem Abbruch gespielt", str(r8)[:200])
    open(pf, "wb").write(cur)
    # "rollback" nach geschriebener Zeile: die Kopie der Datenbank kommt zurück (im Lauf abgebrochen, bevor sie zurücklag)
    bkd0 = os.path.join(DATA, "save_db_backup")
    os.makedirs(bkd0, exist_ok=True)
    dbb = os.path.join(bkd0, "savedata-%s-ps5-29990101-000000-RB.db" % ALEX)
    shutil.copy(dbp7, dbb)
    before = open(dbb, "rb").read()
    db_rows_before = db_rows(ALEX, tid="PPSA07890")
    con = sqlite3.connect(dbp7)
    con.execute("UPDATE savedata SET main_title='halb' WHERE title_id='PPSA07890'")
    con.commit()
    con.close()
    os.rename(pf, pf + ".usbfmt-alt")
    open(pf, "wb").write(b"NEU" * 1024)
    open(jp, "w").write(json.dumps(dict(jr, phase="rollback", dbb=dbb, db_new=0)))
    upload(appzip, ALEX)
    check("Aufzeichnung 'rollback': Abbild und Datenbank wie vorher", open(pf, "rb").read() == cur and not os.path.exists(jp)
          and db_rows(ALEX, tid="PPSA07890") == db_rows_before and open(dbp7, "rb").read() == before, str(db_rows(ALEX, tid="PPSA07890"))[:200])
    os.unlink(dbb)
    open(os.path.join(jdir, "save_import.journal"), "w").write('{"v":1,"phase":"swap","fin":"/etc/passwd"}')
    upload(appzip, ALEX)
    check("eine Aufzeichnung, die Fremdes nennt, wird entfernt und rührt nichts an", not os.path.exists(os.path.join(jdir, "save_import.journal")))
    # Aufräumen: je Benutzer bleiben die zehn neuesten Kopien der Spielstand-Datenbank
    bkd = os.path.join(DATA, "save_db_backup")
    for k in range(14):
        fp = os.path.join(bkd, "savedata-%s-ps5-20200101-0000%02d-X.db" % (SAM, k))
        open(fp, "wb").write(b"x")
        os.utime(fp, (1600000000 + k, 1600000000 + k))
    upload(appzip, ALEX)
    left_sam = [f for f in os.listdir(bkd) if f.startswith("savedata-" + SAM)]
    check("Aufräumen: von Sams Datenbank-Kopien bleiben die zehn neuesten", len(left_sam) == 10, str(len(left_sam)))

    # Abbrechen während des Einspielens
    st, j = jreq("POST", "/api/saves/restore", {"paths": z1})
    time.sleep(0.05)
    jreq("POST", "/api/cancel", {})
    res = wait_job()
    r = res.get("last") if res and res.get("last") else res
    check("Abbrechen: endet sauber, keine Reste", r is not None and not [f for f in os.listdir(os.path.dirname(p1)) if "usbfmt-" in f]
          and not [f for f in os.listdir(os.path.join(DATA, "save_tmp")) if f.startswith("sdimg_") or f == "mnt"], str(r)[:200])
finally:
    cleanup()

print("== Ergebnis: %d fehlgeschlagen" % fails)
sys.exit(1 if fails else 0)
