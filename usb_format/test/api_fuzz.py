"""Belastungstest der HTTP-API: zufällige, kaputte und bösartige Anfragen an alle Routen (falsches JSON, falsche Typen,
riesige Zahlen, Pfad-Tricks, ungültige Schlüssel, abgeschnittene Anfragen direkt über den Socket). Laufende Aufträge
werden abgebrochen. Erwartet: jede Anfrage bekommt eine Antwort (oder die Verbindung wird sauber geschlossen), der
Server läuft am Ende noch und antwortet normal.
Aufruf: python3 test/api_fuzz.py <port> <anfragen> <seed>"""
import json
import random
import socket
import sys
import time
import urllib.parse

PORT, N, SEED = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
rnd = random.Random(SEED)
HOST = f"Host: 127.0.0.1:{PORT}\r\n"   # sonst 403 (Schutz vor DNS-Rebinding)
HOSTB = HOST.encode()
ROUTES = ["/api/backup", "/api/backup/download", "/api/backup/resume", "/api/cancel", "/api/check", "/api/delete",
          "/api/devices", "/api/diag", "/api/eject", "/api/format", "/api/game/checksum", "/api/game/compare",
          "/api/game/info", "/api/game/verify", "/api/history", "/api/history/clear", "/api/icon", "/api/images",
          "/api/images/delete", "/api/images/discard", "/api/images/verify", "/api/info", "/api/label", "/api/library",
          "/api/library/sizes", "/api/move", "/api/orphans", "/api/orphans/delete", "/api/partials/discard",
          "/api/pkg/delete", "/api/restore", "/api/restore/upload", "/api/saves", "/api/saves/backup",
          "/api/saves/backups", "/api/saves/download", "/api/saves/locations", "/api/saves/restore", "/api/saves/times",
          "/api/saves/users", "/api/settings", "/api/speed", "/api/status", "/api/system", "/api/tile", "/", "/help",
          "/bg.jpg", "/avatar.png", "/api/nichts", "/api/../etc/passwd", "//api//info"]
FIELDS = ["device", "size", "keys", "key", "path", "dest", "label", "fs", "replug", "repair", "uninstall", "ident",
          "lang", "confirm", "cluster", "scheme", "verify", "full", "folders", "limit", "extras", "theme", "scale_tv",
          "scale_pc", "user", "users", "titles", "keep", "to", "copy", "pkgs", "tv", "pc", "refresh", "job"]
STRINGS = ["da0", "da1", "da9", "", "../../etc/passwd", "/mnt/usb0/../../etc", "a" * 5000, "%00", "äöü\U0001F600",
           "1,2,3", ",,,", "ffffffff", "zzzz", "0", "-1", "18446744073709551616", "exfat", "fat32", "ntfs", "ext4", "hfsplus",
           "nan", "1e309", "\\u0000", "\"", "{}", "download", "PPSA00011", "true", "dark", "250", "9" * 40,
           ",".join("%08x" % rnd.getrandbits(32) for _ in range(300))]
QKEYS = ["key", "keys", "device", "size", "ident", "replug", "refresh", "lang", "path", "x"]


def value():
    r = rnd.random()
    if r < 0.35:
        return rnd.choice(STRINGS)
    if r < 0.55:
        return rnd.choice([0, 1, -1, 2 ** 31, 2 ** 63, -2 ** 63, 2 ** 64 + 5, 67108864, 3.5, 1e300])
    if r < 0.65:
        return rnd.choice([True, False, None])
    if r < 0.8:
        return [value() for _ in range(rnd.randint(0, 4))]
    return {rnd.choice(FIELDS): rnd.choice(STRINGS)}


def body():
    r = rnd.random()
    if r < 0.6:
        return json.dumps({rnd.choice(FIELDS): value() for _ in range(rnd.randint(0, 6))}, ensure_ascii=rnd.random() < 0.5).encode()
    if r < 0.7:
        return bytes(rnd.getrandbits(8) for _ in range(rnd.randint(0, 300)))
    if r < 0.8:
        s = json.dumps({rnd.choice(FIELDS): value() for _ in range(3)}).encode()
        return s[:rnd.randint(0, len(s))]   # abgeschnitten
    if r < 0.88:
        return b"[" * 5000 + b"]" * 5000   # tief verschachtelt
    if r < 0.94:
        return b'{"device":"' + b"x" * 40000 + b'"}'   # größer als erlaubt
    return b""


def request(raw, timeout=15):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    try:
        s.sendall(raw)
        if rnd.random() < 0.05:
            s.shutdown(socket.SHUT_WR)   # Verbindung halb geschlossen
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if len(data) > 64 << 20:
                break
        return data
    finally:
        s.close()


def build():
    path = rnd.choice(ROUTES)
    if rnd.random() < 0.6:
        q = {rnd.choice(QKEYS): str(value()) for _ in range(rnd.randint(1, 3))}
        path += "?" + urllib.parse.urlencode(q)
    method = rnd.choice(["GET", "POST", "POST", "PUT", "DELETE", "OPTIONS", "HEAD", "GARBAGE"])
    b = body() if method != "GET" or rnd.random() < 0.1 else b""
    r = rnd.random()
    if r < 0.85:
        host = HOST if rnd.random() < 0.9 else "Host: evil.example\r\n"   # fremder Host: muss 403 geben
        head = f"{method} {path} HTTP/1.1\r\n{host}Content-Type: application/json\r\nContent-Length: {len(b)}\r\n\r\n"
        return head.encode("utf-8", "replace") + b
    if r < 0.9:   # falsche Länge
        return f"POST {path} HTTP/1.1\r\n{HOST}Content-Length: {rnd.choice([0, 5, 999999, -3])}\r\n\r\n".encode() + b
    if r < 0.95:   # kaputte Kopfzeilen
        return b"POST " + path.encode() + b" HTTP/1.1\r\n" + HOSTB + b"X: " + b"y" * rnd.randint(1, 20000) + b"\r\n\r\n"
    return bytes(rnd.getrandbits(8) for _ in range(rnd.randint(0, 2000)))


def wait_idle():
    for _ in range(600):
        st = request(b"GET /api/status HTTP/1.1\r\n" + HOSTB + b"\r\n")
        if b'"busy":false' in st:
            return
        if rnd.random() < 0.5:
            request(b"POST /api/cancel HTTP/1.1\r\n" + HOSTB + b"Content-Length: 2\r\n\r\n{}")
        time.sleep(0.1)


answered = closed = 0
t0 = time.time()
for i in range(N):
    raw = build()
    try:
        resp = request(raw)
    except (ConnectionResetError, BrokenPipeError):
        resp = b""
    except socket.timeout:
        print(f"FEHLER  keine Antwort auf Anfrage {i}: {raw[:120]!r}")
        sys.exit(1)
    if resp.startswith(b"HTTP/1.1 "):
        answered += 1
        code = resp[9:12]
        if code == b"500" and b"Kein Speicher" not in resp:
            print(f"HINWEIS 500 auf {raw[:100]!r}: {resp[:200]!r}")
        if code == b"202":
            wait_idle()
    else:
        closed += 1
    if i % 500 == 0:
        try:
            info = request(b"GET /api/info HTTP/1.1\r\n" + HOSTB + b"\r\n")
        except OSError as e:
            print(f"FEHLER  Server antwortet nicht mehr nach Anfrage {i}: {e} - zuletzt {raw[:160]!r}")
            sys.exit(1)
        if not info.startswith(b"HTTP/1.1 200"):
            print(f"FEHLER  /api/info nach Anfrage {i}: {info[:200]!r}")
            sys.exit(1)
wait_idle()
info = request(b"GET /api/info HTTP/1.1\r\n" + HOSTB + b"\r\n")
ok = info.startswith(b"HTTP/1.1 200")
print(f"{N} Anfragen in {time.time() - t0:.0f} s: {answered} beantwortet, {closed} geschlossen; Server {'läuft' if ok else 'ANTWORTET NICHT'}")
sys.exit(0 if ok else 1)
