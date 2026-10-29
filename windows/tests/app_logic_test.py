"""Logik der Windows-App ohne Fenster: Versionsvergleich, Duplikat-Kennzeichnung, Vorabprüfung (Fehlertext und "Danach frei"),
Paket-Hinweise und - gegen einen laufenden Testserver (Host-Fassung, optional) - die API-Aufrufe move_check und verify_games.
Aufruf: python tests/app_logic_test.py [host:port des Testservers]"""
import sys as _sys
for _s in (_sys.stdout, _sys.stderr):   # CI-Rechner geben oft in cp1252 aus: Umlaute und Ersatzzeichen dürfen nie zum Absturz führen
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
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

# Ziel-Auswahl: Lesetempo aus dem Verlauf, zu wenig Platz, FAT32
hist = [{"kind": "format", "mbps": 0, "model": "A", "size": 5}, {"kind": "speed", "mbps": 25, "model": "A", "size": 5},
        {"kind": "speed", "mbps": 300, "ident": "S1", "model": "B", "size": 9}]
small = {"name": "da1", "model": "A", "size": 5, "format": "FAT32", "mounts": [{"free": 100}]}
fast = {"name": "da2", "model": "B", "size": 9, "ident": "S1", "format": "exFAT", "mounts": [{"free": 10 << 30}]}
check(uf.read_mbps(small, hist) == 25 and uf.read_mbps(fast, hist) == 300 and uf.read_mbps({"name": "x"}, hist) == 0,
      "read_mbps: über Seriennummer bzw. Modell und Größe, Einträge ohne Tempo übersprungen")
t1, fits1 = uf.target_label(small, need=1000, history=hist)
t2, fits2 = uf.target_label(fast, need=1000, history=hist)
check(not fits1 and "25 MB/s" in t1 and "langsam" in t1 and "zu wenig Platz" in t1 and "4 GB" in t1, "target_label: zu klein, langsam, FAT32", t1)
check(fits2 and "300 MB/s" in t2 and "langsam" not in t2 and "4 GB" not in t2, "target_label: passt, schnell", t2)

# Verlauf als CSV und Namen aller Auftragsarten
import pc_common  # noqa: E402
csv_text = pc_common.history_csv([("PC", [{"t": 1, "kind": "format", "ok": True, "model": "M;1", "msg": 'a "b"'}]),
                                  ("PS5", [{"t": 2, "kind": "speed", "ok": False, "mbps": 88, "device": "da0"}])])
lines = csv_text.lstrip("\ufeff").split("\r\n")
check(csv_text.startswith("\ufefftime;where;kind") and lines[1].split(";")[1:4] == ["PS5", "speed", "failed"] and '"M;1"' in lines[2]
      and '"a ""b"""' in lines[2] and ";88;" in lines[1], "history_csv: neueste zuerst, Semikolon, Anführungszeichen", csv_text[:200])
check(all(pc_common.history_kind(k) != k for k in ("move", "game_verify", "game_checksum", "partial_discard", "orphan_delete",
                                                    "game_compare", "pkg_delete", "save_backup", "save_restore")),
      "history_kind: alle Auftragsarten des Formatierers haben einen Namen")

# Tempo-Verlauf je Datenträger (neueste Einträge zuerst, wie im Verlauf)
hist2 = [{"kind": "speed", "mbps": 60, "ident": "A", "model": "Stick", "size": 8},
         {"kind": "format", "mbps": 0, "ident": "A"},
         {"kind": "speed", "mbps": 120, "ident": "A", "model": "Stick", "size": 8},
         {"kind": "speed", "mbps": 118, "ident": "A", "model": "Stick", "size": 8},
         {"kind": "speed", "mbps": 300, "ident": "B", "model": "SSD", "size": 9},
         {"kind": "speed", "mbps": 30, "model": "Alt", "size": 4},
         {"kind": "speed", "mbps": 31, "model": "Alt", "size": 4}]
tr = {g["name"]: g for g in pc_common.speed_trends(hist2)}
check(set(tr) == {"Stick", "Alt"} and tr["Stick"]["vals"] == [118, 120, 60] and tr["Stick"]["drop"] and not tr["Alt"]["drop"],
      "speed_trends: je Datenträger älteste zuerst, nur ab zwei Messungen, Einbruch erkannt", tr)
check(pc_common.system_report({"fw": "12.00", "kext": "kstuff", "smp": {"version": "1.7", "caps": ["storage_eject", "x"]}})
      == "PS5: Firmware 12.00 · ShadowMountPlus 1.7 (2 capabilities, eject) · Kernel: kstuff"
      and "not reachable" in pc_common.system_report({"fw": "9.00", "smp": None}) and pc_common.system_report(None) == "PS5: -",
      "system_report: Firmware, ShadowMountPlus und Kernel-Erweiterungen für den Fehlerbericht")

# Inhalt vergleichen: welche Paare der Server vergleichen kann
F, EX, UFS, PFS = {"kind": "Ordner"}, {"kind": "exFAT"}, {"kind": "UFS2"}, {"kind": "PFS"}
check(uf.comparable(F, F) and uf.comparable(PFS, EX) and uf.comparable(F, EX) and uf.comparable(UFS, F) and not uf.comparable(F, PFS)
      and not uf.comparable({"kind": "?"}, F), "comparable: Ordner/Abbild gemischt nur mit exFAT- oder UFS2-Abbild")

# Verschleiß: Lesetempo kommt in den Verlauf, eine deutlich langsamere Messung desselben Datenträgers bringt einen Hinweis
pc_common.enable_history("wear-test")
stick = {"disk": 4, "model": "M", "size": 8 << 30, "serial": "S9", "volumes": []}
other = dict(stick, serial="S8")
for mbps in (100, 110, 95):
    check(pc_common.record_job(["speed", "4", str(stick["size"])], True, "gemessen", stick, info={"read": mbps}) == "",
          "record_job: ähnliches Tempo ohne Hinweis (%d MB/s)" % mbps)
check(pc_common.history_load()[0].get("mbps") == 95, "record_job: Lesetempo im Verlauf", pc_common.history_load()[0])
check(pc_common.record_job(["speed", "4", str(stick["size"])], True, "gemessen", other, info={"read": 30}) == "",
      "record_job: anderer Stick (andere Seriennummer) ohne Hinweis")
note = pc_common.record_job(["speed", "4", str(stick["size"])], True, "gemessen", stick, info={"read": 40})
check("100 MB/s" in note and "verschlissen" in note and "verschlissen" in pc_common.history_load()[0]["msg"],
      "record_job: deutlich langsamer als der Median früherer Messungen - Hinweis in Meldung und Verlauf", note)
check(pc_common.slower_note([{"mbps": 8, "ident": "A"}], {"mbps": 4, "ident": "A"}) == "",
      "slower_note: langsame Sticks schwanken - unter 5 MB/s Unterschied kein Hinweis")
check(pc_common.record_job(["speed", "4", "1"], False, "Abgebrochen.", stick, info={"read": 1}) == ""
      and "mbps" not in pc_common.history_load()[0], "record_job: fehlgeschlagene Messung ohne Tempo und ohne Hinweis")

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

    # neue Aufrufe: Einzelheiten, Prüfsumme anlegen, Teilkopie verwerfen (der Testserver hat dafür ohne.ffpfs und eine Teilkopie auf da1)
    lib = api._call("/api/library")
    keys = {t["path"].rsplit("/", 1)[-1]: t["key"] for t in lib["titles"]}
    info = api.game_info(keys["Beta"])
    check(info.get("id") == "PPSA00002" and "entries" in info and "checksum" in info, "game_info gegen den Server", info)
    if "ohne.ffpfs" in keys:
        api.checksum_games([keys["ohne.ffpfs"]])
        for _ in range(100):
            st = api._call("/api/status")
            if not st.get("busy"):
                break
            time.sleep(0.2)
        check(st.get("ok") and "angelegt" in st.get("message", ""), "checksum_games: Prüfsumme angelegt", st.get("message"))
    if lib.get("partials"):
        dev = lib["partials"][0]["device"]
        api.discard_copy_partial(dev)
        for _ in range(100):
            st = api._call("/api/status")
            if not st.get("busy"):
                break
            time.sleep(0.2)
        lib2 = api._call("/api/library?refresh=1")
        check(st.get("ok") and not lib2.get("partials"), "discard_copy_partial: Teilkopie verworfen", st.get("message"))

# zwei Fenster desselben Programms: keines überschreibt die Änderungen des anderen
APP = "merge-test"
pc_common.save_settings(APP, {})
a = pc_common.load_settings(APP)
pc_common._settings_base.pop(APP, None)   # zweites Fenster: eigener Ladestand (im selben Prozess nachgestellt)
b = pc_common.load_settings(APP)
base_b = pc_common._settings_base[APP]
pc_common._settings_base[APP] = {}        # Fenster A hat {} geladen
a["host"] = "192.168.1.5"
a["copy_queue"] = [{"keys": ["1"], "device": "da1"}]
pc_common.save_settings(APP, a)
pc_common._settings_base[APP] = base_b    # Fenster B (hat ebenfalls {} geladen) ändert nur die Sprache
b["lang"] = "en"
pc_common.save_settings(APP, b)
now = pc_common.load_settings(APP)
check(now.get("host") == "192.168.1.5" and now.get("lang") == "en" and now.get("copy_queue"), "save_settings: Änderungen zweier Fenster bleiben beide erhalten", now)
check(b.get("host") == "192.168.1.5", "save_settings: das speichernde Fenster bekommt die Änderung des anderen mit", b)
b.pop("copy_queue")
pc_common.save_settings(APP, b)
check("copy_queue" not in pc_common.load_settings(APP), "save_settings: hier gelöschter Schlüssel bleibt gelöscht")

# verschachtelt: das Bedienfeld "Am PC – frei" hält einen Verweis auf settings["standard"] - der muss nach jedem Speichern gelten
APP2 = "merge-test-2"
pc_common.save_settings(APP2, {})
s = pc_common.load_settings(APP2)
panel = s.setdefault("standard", {})
s["page"] = "games"
pc_common.save_settings(APP2, s)           # anderes Speichern (Reiterwechsel)
panel["fs"] = "ntfs"                       # danach ändert das Bedienfeld seine Einstellung
pc_common.save_settings(APP2, s)
check(s["standard"] is panel and pc_common.load_settings(APP2).get("standard", {}).get("fs") == "ntfs",
      "save_settings: verschachtelte Einstellung bleibt dasselbe Objekt und wird gespeichert", pc_common.load_settings(APP2))
# zwei Fenster ändern verschiedene Schlüssel im selben verschachtelten Wörterbuch
pc_common.save_settings(APP2, {"standard": {"fs": "exfat", "label": ""}})
x = pc_common.load_settings(APP2)
base_x = pc_common._settings_base[APP2]
y = pc_common.load_settings(APP2)
pc_common._settings_base[APP2] = base_x
x["standard"]["fs"] = "fat32"
pc_common.save_settings(APP2, x)
pc_common._settings_base[APP2] = {"standard": {"fs": "exfat", "label": ""}}
y["standard"]["label"] = "STICK"
pc_common.save_settings(APP2, y)
now = pc_common.load_settings(APP2)["standard"]
check(now.get("fs") == "fat32" and now.get("label") == "STICK", "save_settings: Änderungen zweier Fenster im selben Bereich bleiben beide", now)

print("Alle App-Logik-Tests bestanden." if not fails else "%d Fehler" % fails)
sys.exit(1 if fails else 0)
