"""AppImage mit zsync aktualisieren: nur die geänderten Teile laden.

Die .zsync-Datei neben dem AppImage im Release (appimagetool -u, zsyncmake) listet für jeden Block der neuen Datei eine
schwache, rollende Prüfsumme (rsum) und den Anfang seiner MD4-Prüfsumme. Gesucht wird jeder Block an beliebiger Stelle
der laufenden, alten Datei; was sich nicht findet, kommt per HTTP-Range aus dem Release. Die fertige Datei muss die
SHA-1 aus der .zsync und - wenn GitHub sie nennt - die SHA-256 des Releases haben, erst dann ersetzt sie die alte.
Zwischen zwei Versionen sind so meist nur etwa 10 % zu laden. Nur Python-Standardbibliothek."""
import hashlib
import os
import struct
import urllib.request

MAX_ZSYNC = 8 << 20      # .zsync-Datei höchstens (34 MB AppImage, 2-KiB-Blöcke: ~100 KiB)
MAX_RANGES = 48          # so viele HTTP-Anfragen höchstens - nahe Lücken werden dazu zusammengelegt
TIMEOUT = 30


class ZsyncError(Exception):
    pass


def _md4_pure(data):
    """MD4 (RFC 1320) - OpenSSL 3 bietet es oft nicht mehr an"""
    def f(x, y, z): return (x & y) | (~x & z)
    def g(x, y, z): return (x & y) | (x & z) | (y & z)
    def h(x, y, z): return x ^ y ^ z
    def rol(v, s): v &= 0xffffffff; return ((v << s) | (v >> (32 - s))) & 0xffffffff
    msg = bytearray(data)
    bits = (8 * len(data)) & 0xffffffffffffffff
    msg.append(0x80)
    while len(msg) % 64 != 56:
        msg.append(0)
    msg += struct.pack("<Q", bits)
    a, b, c, d = 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
    for off in range(0, len(msg), 64):
        x = struct.unpack("<16I", msg[off:off + 64])
        aa, bb, cc, dd = a, b, c, d
        for i in range(16):
            k, s = i, (3, 7, 11, 19)[i % 4]
            if i % 4 == 0: a = rol(a + f(b, c, d) + x[k], s)
            elif i % 4 == 1: d = rol(d + f(a, b, c) + x[k], s)
            elif i % 4 == 2: c = rol(c + f(d, a, b) + x[k], s)
            else: b = rol(b + f(c, d, a) + x[k], s)
        for i in range(16):
            k, s = (0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15)[i], (3, 5, 9, 13)[i % 4]
            if i % 4 == 0: a = rol(a + g(b, c, d) + x[k] + 0x5a827999, s)
            elif i % 4 == 1: d = rol(d + g(a, b, c) + x[k] + 0x5a827999, s)
            elif i % 4 == 2: c = rol(c + g(d, a, b) + x[k] + 0x5a827999, s)
            else: b = rol(b + g(c, d, a) + x[k] + 0x5a827999, s)
        for i in range(16):
            k, s = (0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15)[i], (3, 9, 11, 15)[i % 4]
            if i % 4 == 0: a = rol(a + h(b, c, d) + x[k] + 0x6ed9eba1, s)
            elif i % 4 == 1: d = rol(d + h(a, b, c) + x[k] + 0x6ed9eba1, s)
            elif i % 4 == 2: c = rol(c + h(d, a, b) + x[k] + 0x6ed9eba1, s)
            else: b = rol(b + h(c, d, a) + x[k] + 0x6ed9eba1, s)
        a, b, c, d = (a + aa) & 0xffffffff, (b + bb) & 0xffffffff, (c + cc) & 0xffffffff, (d + dd) & 0xffffffff
    return struct.pack("<4I", a, b, c, d)


def md4(data):
    try:
        return hashlib.new("md4", data).digest()
    except ValueError:
        return _md4_pure(data)


def parse(raw):
    """.zsync-Datei -> {"blocksize", "length", "sha1", "seq", "rlen", "clen", "blocks": [(rsum, md4-anfang)]}"""
    head, sep, body = raw.partition(b"\n\n")
    if not sep:
        raise ZsyncError("zsync: kein Kopf")
    hdr = {}
    for line in head.decode("ascii", "replace").split("\n"):
        k, _, v = line.partition(":")
        hdr[k.strip()] = v.strip()
    try:
        bs, length = int(hdr["Blocksize"]), int(hdr["Length"])
        seq, rlen, clen = (int(x) for x in hdr["Hash-Lengths"].split(","))
        sha1 = bytes.fromhex(hdr["SHA-1"])
    except (KeyError, ValueError) as e:
        raise ZsyncError(f"zsync: Kopf unvollständig ({e})") from None
    if not (bs >= 512 and bs & (bs - 1) == 0 and 0 < length < (1 << 34) and 1 <= seq <= 2 and 1 <= rlen <= 4
            and 3 <= clen <= 16 and len(sha1) == 20):
        raise ZsyncError("zsync: ungültige Angaben")
    n = (length + bs - 1) // bs
    if len(body) < n * (rlen + clen):
        raise ZsyncError("zsync: Blockliste unvollständig")
    blocks = []
    for i in range(n):
        o = i * (rlen + clen)
        blocks.append((int.from_bytes(body[o:o + rlen], "big"), bytes(body[o + rlen:o + rlen + clen])))
    return {"blocksize": bs, "length": length, "sha1": sha1, "seq": seq, "rlen": rlen, "clen": clen, "blocks": blocks}


def _rsum(buf):
    """(a, b) wie zsync: a = Summe der Bytes, b = Summe der Teilsummen, je 16 Bit"""
    a = b = 0
    for x in buf:
        a += x
        b += a
    return a & 0xffff, b & 0xffff


def _key(a, b, rlen):
    """die letzten rlen Bytes von a,b (je 16 Bit, big-endian) - so speichert zsyncmake die rsum"""
    return ((a << 16) | b) & ((1 << (8 * rlen)) - 1)


def match(old, z, progress=None):
    """Für jeden Block der neuen Datei: Stelle in old (bytes) oder None. Bei seq 2 zählt ein Treffer erst, wenn auch
    der folgende Block schwach passt (wie zsync - die rsum ist dafür kürzer). Nach einem Treffer geht es direkt hinter
    der Strecke weiter, die sich am Stück übernehmen ließ: geänderte Dateien haben lange gleiche Strecken."""
    bs, rlen, clen, blocks = z["blocksize"], z["rlen"], z["clen"], z["blocks"]
    n, size = len(blocks), len(old)
    where = [None] * n
    weak = {}
    for i, (r, _c) in enumerate(blocks):
        weak.setdefault(r, []).append(i)
    same = {}   # gleiche Blöcke (etwa Nullen) bekommen dieselbe Stelle
    for i, blk in enumerate(blocks):
        same.setdefault(blk, []).append(i)

    def strong(off, i):
        chunk = old[off:off + bs]
        if len(chunk) < bs:   # nur der letzte Block darf kürzer sein: zsyncmake füllt ihn mit Nullen auf
            if i != n - 1:
                return False
            chunk += bytes(bs - len(chunk))
        return md4(chunk)[:clen] == blocks[i][1]

    def take(off, i):
        for j in same[blocks[i]]:
            if where[j] is None:
                where[j] = off

    def follow(off, i):   # ab Block i an Stelle off übernehmen, solange es passt; Rückgabe: Stelle dahinter
        while i < n and off < size and (where[i] is None or where[i] == off) and strong(off, i):
            take(off, i)
            off += bs
            i += 1
        return off

    pos, fresh, last_report = 0, True, 0
    a = b = a2 = b2 = 0
    while pos + bs <= size:
        if fresh:
            a, b = _rsum(old[pos:pos + bs])
            a2, b2 = _rsum(old[pos + bs:pos + 2 * bs]) if pos + 2 * bs <= size else (0, 0)
            fresh = False
        hit = None
        for i in weak.get(_key(a, b, rlen), ()):
            if where[i] is not None:
                continue
            if z["seq"] > 1 and i + 1 < n and (pos + 2 * bs > size or blocks[i + 1][0] != _key(a2, b2, rlen)):
                continue
            if strong(pos, i):
                hit = i
                break
        if hit is not None:
            pos = follow(pos, hit)
            fresh = True
            continue
        if progress and pos - last_report >= (1 << 20):
            last_report = pos
            progress(pos, size)
        if pos + bs >= size:
            break
        x, y = old[pos], old[pos + bs]   # eine Stelle weiter rollen (beide Fenster)
        a = (a - x + y) & 0xffff
        b = (b - bs * x + a) & 0xffff
        if pos + 2 * bs < size:
            y2 = old[pos + 2 * bs]
            a2 = (a2 - y + y2) & 0xffff
            b2 = (b2 - bs * y + a2) & 0xffff
        pos += 1
    if where[-1] is None and size % bs and size >= bs:   # kurzer letzter Block der alten Datei
        if strong(size - size % bs, n - 1):
            take(size - size % bs, n - 1)
    return where


def missing_ranges(where, bs, length, max_ranges=MAX_RANGES):
    """fehlende Blöcke als Byte-Bereiche [(von, bis_einschließlich)]; mehr als max_ranges: die kleinsten Lücken
    dazwischen mitladen"""
    runs, i, n = [], 0, len(where)
    while i < n:
        if where[i] is None:
            j = i
            while j + 1 < n and where[j + 1] is None:
                j += 1
            runs.append([i, j])
            i = j + 1
        else:
            i += 1
    while len(runs) > max_ranges:
        gaps = sorted(range(len(runs) - 1), key=lambda k: runs[k + 1][0] - runs[k][1])
        k = gaps[0]
        runs[k][1] = runs[k + 1][1]
        del runs[k + 1]
    return [(s * bs, min(length, (e + 1) * bs) - 1) for s, e in runs]


def _get_range(url, start, end):
    req = urllib.request.Request(url, headers={"Range": f"bytes={start}-{end}", "User-Agent": "USB-Formatierer"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        if r.getcode() != 206:
            raise ZsyncError(f"Server liefert keine Teilbereiche (HTTP {r.getcode()})")
        data = r.read(end - start + 2)
    if len(data) != end - start + 1:
        raise ZsyncError("Teilbereich unvollständig geladen")
    return data


def fetch(url, max_bytes=MAX_ZSYNC):
    req = urllib.request.Request(url, headers={"User-Agent": "USB-Formatierer"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        data = r.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise ZsyncError("zsync-Datei zu groß")
    return data


def update(old_path, zsync_url, file_url, out_path, sha256=None, progress=None, get_range=_get_range):
    """neue Datei nach out_path bauen. progress(text, anteil 0..1). Rückgabe: (geladen, gesamt) in Bytes."""
    say = progress or (lambda _t, _f: None)
    say("zsync", 0.0)
    z = parse(fetch(zsync_url))
    with open(old_path, "rb") as f:
        old = f.read()
    where = match(old, z, lambda pos, size: say("match", 0.05 + 0.45 * pos / max(1, size)))
    bs, length = z["blocksize"], z["length"]
    ranges = missing_ranges(where, bs, length)
    need = sum(e - s + 1 for s, e in ranges)
    got = {}
    done = 0
    for s, e in ranges:
        got[s] = get_range(file_url, s, e)
        done += e - s + 1
        say("load", 0.5 + 0.45 * done / max(1, need))
    out = bytearray(length)
    for i, off in enumerate(where):
        if off is not None:
            chunk = old[off:off + bs]
            out[i * bs:i * bs + len(chunk)] = chunk[:max(0, min(bs, length - i * bs))]
    for s, data in got.items():
        out[s:s + len(data)] = data
    del out[length:]
    if hashlib.sha1(out).digest() != z["sha1"]:
        raise ZsyncError("SHA-1 der neuen Datei stimmt nicht - nichts ersetzt")
    if sha256 and hashlib.sha256(out).hexdigest() != sha256.lower():
        raise ZsyncError("SHA-256 der neuen Datei stimmt nicht mit dem Release überein - nichts ersetzt")
    with open(out_path, "wb") as f:
        f.write(out)
    say("done", 1.0)
    return need, length


def replace_appimage(new_path, appimage):
    """fertige Datei an die Stelle des laufenden AppImage setzen (gleicher Ordner, umbenennen ist atomar; das laufende
    Programm behält seine alte Datei, bis es endet)"""
    os.chmod(new_path, os.stat(appimage).st_mode | 0o111)
    os.replace(new_path, appimage)
