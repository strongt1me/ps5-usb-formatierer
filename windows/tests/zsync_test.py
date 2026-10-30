"""AppImage mit zsync aktualisieren (zsync_update.py): .zsync-Datei wie zsyncmake (rsum, MD4, Hash-Lengths 2,2,5),
alte Datei mit verschobenen, geänderten und gleichen Teilen; geladen wird nur, was fehlt, das Ergebnis ist byte-gleich,
falsche Prüfsummen ersetzen nichts. Ohne Netz (Teilbereiche aus dem Speicher). Aufruf: python tests/zsync_test.py"""
import sys as _sys
for _s in (_sys.stdout, _sys.stderr):   # CI-Rechner geben oft in cp1252 aus: Umlaute dürfen nie zum Absturz führen
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
import hashlib  # noqa: E402
import os  # noqa: E402
import random  # noqa: E402
import sys  # noqa: E402
import tempfile  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import zsync_update as zu  # noqa: E402

fails = 0


def check(cond, text, why=""):
    global fails
    print(("OK      " if cond else "FEHLER  ") + text + ("" if cond or not why else " - " + str(why)))
    fails += 0 if cond else 1


def make_zsync(data, bs=2048, rlen=2, clen=5, seq=2):
    """wie zsyncmake: je Block die letzten rlen Bytes von (a, b) big-endian und die ersten clen Bytes der MD4 (letzter
    Block mit Nullen aufgefüllt)"""
    head = (f"zsync: 0.6.2\nFilename: t.AppImage\nBlocksize: {bs}\nLength: {len(data)}\n"
            f"Hash-Lengths: {seq},{rlen},{clen}\nURL: t.AppImage\nSHA-1: {hashlib.sha1(data).hexdigest()}\n\n")
    body = bytearray()
    for o in range(0, len(data), bs):
        blk = data[o:o + bs].ljust(bs, b"\0")
        a, b = zu._rsum(blk)
        body += zu._key(a, b, rlen).to_bytes(rlen, "big") + zu.md4(blk)[:clen]
    return head.encode() + bytes(body)


check(zu._md4_pure(b"") .hex() == "31d6cfe0d16ae931b73c59d7e0c089c0", "MD4 (RFC 1320): leer")
check(zu._md4_pure(b"message digest").hex() == "d9130a8164549fe818874806e1c7014b", "MD4 (RFC 1320): message digest")
check(zu._md4_pure(bytes(range(256)) * 3) == zu.md4(bytes(range(256)) * 3), "MD4: eigene = hashlib (falls vorhanden)")

rnd = random.Random(7)
new = bytearray(rnd.randbytes(600 * 1024 + 123))   # nicht durch die Blockgröße teilbar
new[100000:140000] = bytes(40000)                     # Nullen: viele gleiche Blöcke
old = bytearray(new)
del old[5000:5017]                                    # verschoben: alles dahinter liegt 17 Bytes früher
old[300000:310000] = rnd.randbytes(10000)             # geändert
old += rnd.randbytes(5000)                            # angehängt
new, old = bytes(new), bytes(old)

W = tempfile.mkdtemp(prefix="zsync-test-")
paths = {k: os.path.join(W, k) for k in ("old", "z", "out", "bad", "empty")}
open(paths["old"], "wb").write(old)
open(paths["z"], "wb").write(make_zsync(new))
open(paths["empty"], "wb").close()
zurl = "file:///" + paths["z"].replace("\\", "/").lstrip("/")
calls = []


def get_range(url, s, e):
    calls.append((s, e))
    return new[s:e + 1]


need, total = zu.update(paths["old"], zurl, "x", paths["out"], hashlib.sha256(new).hexdigest(), get_range=get_range)
check(open(paths["out"], "rb").read() == new, "neue Datei byte-gleich")
check(total == len(new) and need < len(new) // 5, f"nur Fehlendes geladen: {need} von {total} Bytes", need)
check(len(calls) <= zu.MAX_RANGES, f"{len(calls)} Teilbereiche (höchstens {zu.MAX_RANGES})")

calls.clear()
need, total = zu.update(paths["empty"], zurl, "x", paths["out"], get_range=get_range)
check(need == total and open(paths["out"], "rb").read() == new, "ohne alte Datei: alles geladen, byte-gleich")

try:
    zu.update(paths["old"], zurl, "x", paths["bad"], "0" * 64, get_range=get_range)
    check(False, "falsche SHA-256 abgelehnt")
except zu.ZsyncError:
    check(not os.path.exists(paths["bad"]), "falsche SHA-256 abgelehnt, nichts geschrieben")

try:
    zu.update(paths["old"], zurl, "x", paths["bad"], get_range=lambda u, s, e: bytes(e - s + 1))
    check(False, "falsche Teilbereiche abgelehnt (SHA-1)")
except zu.ZsyncError:
    check(not os.path.exists(paths["bad"]), "falsche Teilbereiche abgelehnt (SHA-1), nichts geschrieben")

for bad, why in ((b"kein kopf", "ohne Kopf"), (make_zsync(new)[:400], "Blockliste abgeschnitten"),
                 (make_zsync(new).replace(b"Blocksize: 2048", b"Blocksize: 1000"), "Blockgröße keine Zweierpotenz")):
    try:
        zu.parse(bad)
        check(False, f"kaputte .zsync abgelehnt: {why}")
    except zu.ZsyncError:
        check(True, f"kaputte .zsync abgelehnt: {why}")

check(zu.missing_ranges([None, 0, None, None, 5, None], 10, 55, 2) == [(0, 39), (50, 54)],
      "Bereiche: nahe Lücken zusammengelegt, letzter Block kurz", zu.missing_ranges([None, 0, None, None, 5, None], 10, 55, 2))

print()
print("Alle zsync-Tests bestanden." if not fails else f"{fails} Test(s) fehlgeschlagen.")
sys.exit(1 if fails else 0)
