"""Änderungszeiten von Spielständen mit der Datenbank der PS5 abgleichen und bei Abweichung zurücksetzen.

Die PS5 führt zu jedem Spielstand in savedata.db die Änderungszeit mit; sie ist gleich der Änderungszeit der Container-Datei.
Ein Spielstand, der von außen ersetzt wurde (auch mit identischem Inhalt), bekommt eine neue Zeit - Formatierer vor 2.6.12
haben das beim Wiederherstellen getan. Dieses Programm zeigt die Abweichungen und setzt die Zeit auf den Datenbankwert zurück.

    python tools/ps5_fix_save_times.py --host 192.168.1.94            nur lesen und anzeigen
    python tools/ps5_fix_save_times.py --host 192.168.1.94 --apply    Zeiten zurücksetzen (Formatierer ab 2.6.12 nötig)

Gebraucht werden der FTP-Server (Port 2121) und der Formatierer (Port 8090). Es werden nur Zeitstempel gesetzt, nie Inhalte.
Läuft ein Spiel, lehnt der Formatierer ab. Nur Python-Standardbibliothek."""
import argparse
import calendar
import ftplib
import io
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

PORT_API = 8090
PORT_FTP = 2121
TOLERANCE = 1   # Sekunden


def api(host, path, body=None):
    req = urllib.request.Request(f"http://{host}:{PORT_API}{path}", data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        try:
            return {"http": e.code, "error": json.load(e).get("error")}
        except ValueError:
            return {"http": e.code, "error": ""}
    except (urllib.error.URLError, OSError) as e:   # Formatierer läuft nicht: Meldung statt Traceback
        sys.exit(f"Formatierer auf {host}:{PORT_API} nicht erreichbar ({e})")


def mdtm(ftp, path):
    try:
        return calendar.timegm(time.strptime(ftp.sendcmd("MDTM " + path)[4:18], "%Y%m%d%H%M%S"))
    except ftplib.all_errors:
        return None


def db_times(ftp, platform, uid, tmp):
    """{(Titel, Ordner): Epoche} aus savedata.db des Benutzers (PS5: savedata_prospero, PS4: savedata)"""
    sub = "savedata_prospero" if platform == "PS5" else "savedata"
    buf = io.BytesIO()
    try:
        ftp.retrbinary(f"RETR /system_data/{sub}/{uid}/db/user/savedata.db", buf.write)
    except ftplib.all_errors as e:
        print(f"  Datenbank {sub}/{uid} nicht lesbar: {e}")
        return {}
    path = os.path.join(tmp, f"{sub}_{uid}.db")
    with open(path, "wb") as f:
        f.write(buf.getvalue())
    out = {}
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    for title, name, mt in con.execute("select title_id, dir_name, mtime from savedata"):
        try:
            out[(title, name)] = calendar.timegm(time.strptime(mt[:19], "%Y-%m-%dT%H:%M:%S"))
        except (ValueError, TypeError):
            pass
    con.close()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=os.environ.get("USBFMT_PS5_HOST", ""), help="Adresse der PS5")
    ap.add_argument("--apply", action="store_true", help="Zeiten zurücksetzen (sonst nur anzeigen)")
    a = ap.parse_args()
    if not a.host:
        sys.exit("Adresse der PS5 fehlt (--host oder USBFMT_PS5_HOST)")
    saves = api(a.host, "/api/saves").get("saves") or []
    if not saves:
        sys.exit("Keine Spielstände gemeldet (läuft der Formatierer?)")
    ftp = ftplib.FTP()
    ftp.connect(a.host, PORT_FTP, timeout=20)
    ftp.login()
    tmp = tempfile.mkdtemp(prefix="ps5-savedb-")
    dbs = {}
    todo = []
    for s in saves:
        plat, uid = s["platform"], s["uid"]
        if (plat, uid) not in dbs:
            dbs[(plat, uid)] = db_times(ftp, plat, uid, tmp)
        name = s["save_dir"].removeprefix("sdimg_")
        want = dbs[(plat, uid)].get((s["title_id"], name))
        sub = "savedata_prospero" if plat == "PS5" else "savedata"
        cpath = f"/user/home/{uid}/{sub}/{s['title_id']}/{s['save_dir']}"
        have = mdtm(ftp, cpath)
        label = f"{plat} {s['title_name']} / {name}"
        if want is None or have is None:
            print(f"  ?  {label}: kein Datenbankeintrag oder Datei nicht lesbar - übersprungen")
            continue
        if abs(have - want) <= TOLERANCE:
            print(f"  ok {label}")
            continue
        if have < want:   # Datei älter als die Datenbank: nicht durch ein Wiederherstellen entstanden, nicht anfassen
            print(f"  ?  {label}: Datei ist älter als die Datenbank - nicht angefasst")
            continue
        key_m = None
        if plat == "PS4":   # die .bin trägt (wie der Container) die Zeit des Wiederherstellens: auf die der Systemkopie setzen
            kpath = f"/user/home/{uid}/{sub}/{s['title_id']}/{name}.bin"
            bu = mdtm(ftp, f"/user/home/{uid}/{sub}/{s['title_id']}/sce_bu_{name}.bin")
            kcur = mdtm(ftp, kpath)
            if bu is not None and kcur is not None and abs(kcur - have) <= 5:
                key_m = bu
        fmt = lambda t: time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(t))
        print(f"  !! {label}: Datei {fmt(have)}, Datenbank {fmt(want)}" + (f"; Schlüsseldatei -> {fmt(key_m)}" if key_m else ""))
        todo.append((s, want, key_m))
    ftp.quit()
    shutil.rmtree(tmp, ignore_errors=True)   # Kopien der savedata.db: nur zum Lesen gebraucht
    if not todo:
        print("Alle Zeiten passen zur Datenbank.")
        return
    if not a.apply:
        print(f"\n{len(todo)} Abweichung(en). Mit --apply zurücksetzen.")
        return
    bad = 0
    for s, want, key_m in todo:
        body = {"key": s["key"], "mtime": want}
        if key_m is not None:
            body["key_mtime"] = key_m
        r = api(a.host, "/api/saves/times", body)
        ok = r.get("ok") is True
        bad += not ok
        print(f"  {'gesetzt' if ok else 'FEHLER'}: {s['title_name']} / {s['save_dir']}" + ("" if ok else f" - {r}"))
    print("Fertig." if not bad else f"{bad} Fehler.")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
