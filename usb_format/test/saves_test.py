#!/usr/bin/env python3
"""Spielstand-API gegen den Host-Formatierer: Liste, ZIP-Sicherung, Wiederherstellen (auch mit bösartigen/kaputten ZIPs),
Neusignieren (mit der Attrappe aus platform_host.c), Schutzregeln.
Aufruf: saves_test.py <host-binary> [port]   (das Binary mit -DSERVER_PORT=<port> gebaut, Standard 18115)"""
import hashlib
import http.client
import io
import json
import os
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
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
env = dict(os.environ, USBFMT_SAVES=HOME, USBFMT_SAVEDB=os.path.join(ROOT, "savedb"), USBFMT_DATA=DATA, USBFMT_IMAGES="", USBFMT_SANDBOX=SAND,
           USBFMT_SAVES_MOCK="Alex=0123456789abcdef,Sam=fedcba9876543210")
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
    check("5 Spielstände, sortiert, ohne .bin/zu kleine/leere Ordner", st == 200 and keys == sorted(keys) and len(keys) == 5,
          str(keys))
    s1 = next(s for s in j["saves"] if s["key"] == K1)
    check("Spielname aus param.sfo des Spiels", s1["title_name"] == 'Beispiel-Spiel: Die "Rückkehr"', s1["title_name"])
    check("Benutzername und Größe", s1["username"] == "Alex" and s1["size"] == 2 << 20, str(s1))
    st, j = jreq("GET", "/api/saves/users")
    users = {u["uid"]: u for u in j["users"]}
    check("3 Benutzer mit Namen, Konten aus der Registry", len(users) == 3 and users["2d4e9b1f"]["account_id"] == "fedcba9876543210"
          and users["3a5b7c9d"]["account_id"] == "", str(j))

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

    # ------------------------------------------------------------------ Neusignieren
    def sfo_pos(blob):
        sfo = blob[fx.SFO_OFF:fx.SFO_OFF + 0x1000]
        kt, dt, n = struct.unpack("<III", sfo[8:20])
        pos = {}
        for k in range(n):
            ko, fmt, ln, mx, do = struct.unpack("<HHIII", sfo[20 + 16 * k:36 + 16 * k])
            pos[sfo[kt + ko:sfo.index(b"\0", kt + ko)].decode()] = fx.SFO_OFF + dt + do
        return pos

    def db_rows(uid):
        c = sqlite3.connect(os.path.join(ROOT, "savedb", uid, "db", "user", "savedata.db"))
        c.row_factory = sqlite3.Row
        rows = {(r["title_id"], r["dir_name"]): dict(r) for r in c.execute("SELECT * FROM savedata")}
        c.close()
        return rows

    def dbfile(uid):
        return os.path.join(ROOT, "savedb", uid, "db", "user", "savedata.db")

    def signed(v):
        return v - (1 << 64) if v >= 1 << 63 else v

    print("== Neusignieren am Ort (Besitzer = Zielbenutzer) ==")
    before = open(path1, "rb").read()
    pos = sfo_pos(before)
    os.chmod(path1, 0o600)
    alex_db = open(dbfile("1c3f8a0e"), "rb").read()
    st, j = jreq("POST", "/api/saves/resign?lang=en", {"key": K1, "uid": "1c3f8a0e"})
    res = wait_job()
    check("Neusignieren angenommen", st == 202, str(j))
    check("Auftrag save_resign ok, Meldung ohne Kopie", res and res["ok"] and res["kind"] == "save_resign" and "re-signed" in res["message"]
          and "copy" not in res["message"], str(res))
    after = open(path1, "rb").read()
    diff = [i for i in range(len(before)) if before[i] != after[i]]
    exp = set(range(pos["ACCOUNT_ID"], pos["ACCOUNT_ID"] + 8))
    check("nur ACCOUNT_ID geändert", diff and set(diff) <= exp, "geänderte Stellen: %s" % diff[:20])
    check("ACCOUNT_ID little-endian des Kontos", after[pos["ACCOUNT_ID"]:pos["ACCOUNT_ID"] + 8] == bytes.fromhex("0123456789abcdef")[::-1])
    check("Rechte bleiben", os.stat(path1).st_mode & 0o777 == 0o600)
    rows = db_rows("1c3f8a0e")
    r = rows[("PPSA01234", "SAVEDATA00")]
    check("Datenbank: Zeile bekommt die Account-ID", r["account_id"] == 0x0123456789abcdef and r["user_id"] == 0x1c3f8a0e, str(r))
    check("Datenbank: Sicherungskopie davor", open(dbfile("1c3f8a0e") + ".usbfmt.bak", "rb").read() == alex_db)
    check("keine Zwischendateien übrig", not [p for p in tree() if p.endswith(".tmp") or "/save_tmp/resign_" in p],
          str([p for p in tree() if "save_tmp" in p or p.endswith(".tmp")]))

    print("== Kopie für einen anderen Benutzer ==")
    K3 = "1c3f8a0e/PPSA07890/sdimg_SAVEDATA00"
    src = os.path.join(HOME, "1c3f8a0e", "savedata_prospero", "PPSA07890", "sdimg_SAVEDATA00")
    orig = open(src, "rb").read()
    os.chmod(src, 0o640)
    sam_db = open(dbfile("2d4e9b1f"), "rb").read()
    alex_db = open(dbfile("1c3f8a0e"), "rb").read()
    st, j = jreq("POST", "/api/saves/resign?lang=en", {"key": K3, "uid": "2d4e9b1f"})
    res = wait_job()
    check("Kopie angenommen und ok, Meldung nennt die Kopie", st == 202 and res and res["ok"] and "copy" in res["message"], str(res))
    check("Original unverändert", open(src, "rb").read() == orig)
    dest = os.path.join(HOME, "2d4e9b1f", "savedata_prospero", "PPSA07890", "sdimg_SAVEDATA00")
    check("Kopie liegt beim Zielbenutzer", os.path.exists(dest) and len(open(dest, "rb").read()) == len(orig))
    cp = open(dest, "rb").read() if os.path.exists(dest) else b""
    pos = sfo_pos(orig)
    diff = [i for i in range(len(orig)) if i < len(cp) and orig[i] != cp[i]]
    exp = set(range(pos["ACCOUNT_ID"], pos["ACCOUNT_ID"] + 8)) | set(range(pos["PARAMS"] + 4, pos["PARAMS"] + 40))
    check("Kopie unterscheidet sich nur in ACCOUNT_ID, Benutzer-ID und HMAC", diff and set(diff) <= exp, str(diff[:20]))
    check("ACCOUNT_ID des Zielkontos", cp[pos["ACCOUNT_ID"]:pos["ACCOUNT_ID"] + 8] == bytes.fromhex("fedcba9876543210")[::-1])
    check("Benutzer-ID des Zielbenutzers", struct.unpack("<I", cp[pos["PARAMS"] + 4:pos["PARAMS"] + 8])[0] == 0x2d4e9b1f)
    check("PARAMS-HMAC aus einem Spielstand des Zielbenutzers", cp[pos["PARAMS"] + 8:pos["PARAMS"] + 40] == fx.user_hmac("2d4e9b1f"))
    check("Rechte wie beim Original", os.path.exists(dest) and os.stat(dest).st_mode & 0o777 == 0o640)
    icon = os.path.join(HOME, "2d4e9b1f", "savedata_prospero_meta", "user", "PPSA07890", "SAVEDATA00_icon0.png")
    check("Symbolbild beim Zielbenutzer", os.path.exists(icon) and open(icon, "rb").read() == orig[fx.ICON_OFF:fx.ICON_OFF + fx.ICON_LEN])
    rows = db_rows("2d4e9b1f")
    r = rows.get(("PPSA07890", "SAVEDATA00"), {})
    check("Datenbank des Zielbenutzers: neue Zeile", r and signed(0xfedcba9876543210) == r["account_id"] and r["user_id"] == 0x2d4e9b1f
          and r["main_title"] == "Zweites Spiel" and r["sub_title"] == "Kapitel 3" and r["blocks"] == 96 and r["size_kib"] == 96 * 64
          and r["system_blocks"] == 2 and r["game_title_id"] == "PPSA07890" and r["user_param"] == 7, str(r))
    check("Datenbank: übrige Zeilen bleiben, 3 Zeilen", len(rows) == 3 and ("PPSA01234", "SAVEDATA00") in rows)
    check("Datenbank: Sicherungskopie des Zielbenutzers", open(dbfile("2d4e9b1f") + ".usbfmt.bak", "rb").read() == sam_db)
    check("Datenbank des Besitzers unberührt", open(dbfile("1c3f8a0e"), "rb").read() == alex_db)
    st, j = jreq("GET", "/api/saves")
    check("Kopie erscheint in der Liste", any(s["key"] == "2d4e9b1f/PPSA07890/sdimg_SAVEDATA00" for s in j["saves"]))
    check("keine Zwischendateien übrig", not [p for p in tree() if p.endswith(".tmp") or "/save_tmp/resign_" in p])

    print("== Kopie abgelehnt / zurückgenommen ==")
    snap = tree()
    snap_bytes = {p: open(p, "rb").read() for p in snap if p.endswith(".db")}
    st, j = jreq("POST", "/api/saves/resign?lang=en", {"key": K1, "uid": "2d4e9b1f"})
    r = wait_job()
    check("Zielbenutzer hat schon einen Spielstand gleichen Namens", st == 400 or (r and not r["ok"] and "already has" in r["message"]),
          "%s %s %s" % (st, j, r and r["message"]))
    check("... nichts verändert", tree() == snap)
    # Datenbank des Zielbenutzers kaputt: die Kopie darf nicht stehen bleiben
    K4 = "1c3f8a0e/PPSA09999/sdimg_EXTRA"
    good = open(dbfile("2d4e9b1f"), "rb").read()
    open(dbfile("2d4e9b1f"), "wb").write(b"kein sqlite " * 200)
    st, j = jreq("POST", "/api/saves/resign?lang=en", {"key": K4, "uid": "2d4e9b1f"})
    r = wait_job()
    check("kaputte Datenbank: Fehler gemeldet", r and not r["ok"] and "database" in r["message"].lower(), str(r and r["message"]))
    check("kaputte Datenbank: Kopie und Symbolbild zurückgenommen",
          not os.path.exists(os.path.join(HOME, "2d4e9b1f", "savedata_prospero", "PPSA09999", "sdimg_EXTRA"))
          and not os.path.exists(os.path.join(HOME, "2d4e9b1f", "savedata_prospero_meta", "user", "PPSA09999", "EXTRA_icon0.png")))
    check("kaputte Datenbank: Original unverändert", open(os.path.join(HOME, "1c3f8a0e", "savedata_prospero", "PPSA09999", "sdimg_EXTRA"), "rb").read()
          == fx.container("1c3f8a0e" + "PPSA09999" + "sdimg_EXTRA", 1 << 20, uid="1c3f8a0e", title="PPSA09999", name="sdimg_EXTRA"))
    open(dbfile("2d4e9b1f"), "wb").write(good)
    os.remove(dbfile("2d4e9b1f") + ".usbfmt.bak")

    # Fehlerfälle
    def resign_fail(name, body, want):
        lang = body.pop("lang", "de") if isinstance(body, dict) else "de"
        st, j = jreq("POST", "/api/saves/resign?lang=" + lang, body)
        if st == 202:
            r = wait_job()
            check(name, r and not r["ok"] and want in r["message"], str(r and r["message"]))
        else:
            check(name, st == want if isinstance(want, int) else False, "%s %s" % (st, j))
    snap = tree()
    resign_fail("Zielbenutzer ohne Konto (Jo)", {"key": K1, "uid": "3a5b7c9d", "lang": "en"}, "No account ID")
    resign_fail("Zielbenutzer existiert nicht", {"key": K1, "uid": "ffffffff", "lang": "en"}, "does not exist")
    resign_fail("unbekannter Spielstand", {"key": "1c3f8a0e/PPSA00000/x", "uid": "2d4e9b1f", "lang": "en"}, "not found")
    resign_fail("uid mit Schrägstrich", {"key": K1, "uid": "../x"}, 400)
    resign_fail("uid fehlt", {"key": K1}, 400)
    resign_fail("key fehlt", {"uid": "2d4e9b1f"}, 400)
    resign_fail("kein JSON (Content-Type fehlt)", None, 415)
    resign_fail("Schlüssel zu lang", {"key": "a" * 400, "uid": "2d4e9b1f"}, 400)
    check("Fehlerfälle ändern nichts", tree() == snap)
    # PS4-Container
    ps4 = os.path.join(HOME, "2d4e9b1f", "savedata_prospero", "PPSA07890", "sdimg_SAVEDATA01")
    d = open(ps4, "rb").read()
    open(ps4, "wb").write(b"\x01" + d[1:])
    resign_fail("PS4-Container", {"key": K2, "uid": "2d4e9b1f", "lang": "en"}, "PS4")
    open(ps4, "wb").write(d)
    # Container ohne ACCOUNT_ID in der param.sfo
    d2 = bytearray(d)
    d2[fx.SFO_OFF:fx.SFO_OFF + 0x1000] = fx.sfo([("TITLE_ID", b"PPSA07890\0")]).ljust(0x1000, b"\0")
    open(ps4, "wb").write(bytes(d2))
    snap = open(ps4, "rb").read()
    resign_fail("param.sfo ohne ACCOUNT_ID", {"key": K2, "uid": "2d4e9b1f", "lang": "en"}, "ACCOUNT_ID")
    check("... Container danach unverändert", open(ps4, "rb").read() == snap)
    open(ps4, "wb").write(d)

    # ------------------------------------------------------------------ laufendes Spiel
    print("== Schutz bei laufendem Spiel ==")
    os.makedirs(os.path.join(SAND, "PPSA11111_000"))
    snap = tree()
    st, j = jreq("POST", "/api/saves/resign", {"key": K1, "uid": "2d4e9b1f"})
    check("Neusignieren bei laufendem Spiel 409", st == 409 and "PPSA11111" in j.get("error", ""), "%s %s" % (st, j))
    st, hd, rb = req("POST", "/api/saves/restore/upload", data, {"Content-Type": "application/zip"})
    check("Wiederherstellen bei laufendem Spiel 409", st == 409, str(st))
    check("Sichern bei laufendem Spiel erlaubt (nur lesen)", req("GET", "/api/saves/download?key=" + K2.replace("/", "%2F"))[0] == 200)
    check("nichts verändert", tree() == snap)
    shutil.rmtree(os.path.join(SAND, "PPSA11111_000"))

    # ------------------------------------------------------------------ Verlauf, Sprachen
    print("== Verlauf ==")
    st, j = jreq("GET", "/api/history")
    kinds = [e["kind"] for e in j["entries"]]
    check("Verlauf enthält save_resign und save_restore", "save_resign" in kinds and "save_restore" in kinds, str(kinds))
    st, j = jreq("POST", "/api/saves/resign?lang=de", {"key": K1, "uid": "ffffffff"})
    r = wait_job()
    check("deutsche Fehlermeldung", r and "nicht vorhanden" in r["message"], str(r and r["message"]))
    st, j = jreq("POST", "/api/saves/resign?lang=fr", {"key": K1, "uid": "ffffffff"})
    r = wait_job()
    check("französische Fehlermeldung", r and "n'existe pas" in r["message"], str(r and r["message"]))
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
