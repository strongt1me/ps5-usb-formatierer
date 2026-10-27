"""Logik der Windows-App ohne Fenster: Versionsvergleich, Duplikat-Kennzeichnung, Vorabprüfung (Fehlertext und "Danach frei"),
Paket-Hinweise und - gegen einen laufenden Testserver (Host-Fassung, optional) - die API-Aufrufe move_check und verify_games.
Aufruf: python tests/app_logic_test.py [host:port des Testservers]"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
W = tempfile.mkdtemp(prefix="app-logic-")
os.environ["XDG_CONFIG_HOME"] = os.environ["APPDATA"] = W

import usb_formatierer as uf  # noqa: E402

fails = 0


def check(cond, text, why=""):
    global fails
    if cond:
        print("OK      " + text)
    else:
        fails += 1
        print("FEHLER  " + text + (" - " + str(why) if why else ""))


check(uf.ver_key("01.010.000") > uf.ver_key("01.009.999") and uf.ver_key("9.00") < uf.ver_key("12.00") and uf.ver_key("") == (),
      "ver_key: Abschnitte werden als Zahlen verglichen")

data = {"titles": [{"path": "/a", "version": "01.005.000"}, {"path": "/b", "version": "01.003.000"}, {"path": "/c", "version": ""},
                   {"path": "/d", "version": "01.000.000"}, {"path": "/e", "version": "01.000.000"}],
        "duplicates": [{"paths": ["/a", "/b"]}, {"paths": ["/d", "/e"]}, {"paths": ["/c", "/a"]}]}
m = uf.dup_marks(data)
check(m["/b"] == ("older", "01.003.000") and m["/d"] == ("dup", "01.000.000") and m["/e"] == ("dup", "01.000.000"),
      "dup_marks: ältere Version und gleiche Versionen")
m2 = uf.dup_marks({"titles": data["titles"][:2], "duplicates": [data["duplicates"][0]]})
check(m2["/a"][0] == "newest" and m2["/b"][0] == "older", "dup_marks: neueste und ältere Version", m2)
check(uf.dup_marks({}) == {}, "dup_marks: ohne Angaben leer")

bad, info = uf.move_check_text({"fits": False, "need": 100, "reserve": 10, "free": 50, "fat32": True, "big": ["riesig.bin"]})
check("riesig.bin" in bad and bad.count(".") >= 2 and info == "", "move_check_text: Platz und FAT32 zusammen, kein Hinweis", (bad, info))
bad, info = uf.move_check_text({"fits": True, "need": 1 << 20, "reserve": 0, "free": 10 << 20, "fat32": False, "big": []})
check(bad == "" and "9" in info, "move_check_text: passt – Hinweis nennt den freien Platz danach", (bad, info))

hints = uf.pkg_hints({"fwhigh": True, "minfw": "9.00", "vs": "older", "inst": "01.005.000", "cat": "update"}, "8.20")
check([k for k, _t in hints] == ["bad", "warn"], "pkg_hints: Firmware zu alt und ältere Version", hints)
check(uf.pkg_hints({"cat": "dlc", "fwhigh": False}, "8.20") == [], "pkg_hints: Zusatzinhalt ohne Hinweise")

if len(sys.argv) > 1:   # laufender Testserver: die neuen Aufrufe der App wirklich abschicken
    host = sys.argv[1].split(":")[0]
    uf.API_PORT = int(sys.argv[1].split(":")[1]) if ":" in sys.argv[1] else uf.API_PORT
    api = uf.Api(host)
    lib = api._call("/api/library")
    keys = {t["path"].rsplit("/", 1)[-1]: t["key"] for t in lib["titles"]}
    r = api.move_check("da1", keys=[keys["Beta"]])
    check(r.get("fits") is True and "need" in r and "free" in r, "move_check gegen den Server", r)
    check(uf.move_check_text(r)[1] != "", "move_check_text mit der Serverantwort: Hinweis vorhanden")
    api.verify_games([keys["gut.ffpfs"]])
    import time
    for _ in range(100):
        st = api._call("/api/status")
        if not st.get("busy"):
            break
        time.sleep(0.2)
    check(st.get("ok") and "stimmt" in st.get("message", ""), "verify_games: Prüfsumme stimmt", st.get("message"))

print("Alle App-Logik-Tests bestanden." if not fails else "%d Fehler" % fails)
sys.exit(1 if fails else 0)
