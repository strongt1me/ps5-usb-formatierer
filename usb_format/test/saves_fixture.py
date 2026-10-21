#!/usr/bin/env python3
"""Legt einen Test-Ordner für den Spielstand-Teil an (USBFMT_SAVES=<ordner>/home):
    <ordner>/home/<uid>/username.dat
    <ordner>/home/<uid>/savedata_prospero/<Titel>/<Container>
    <ordner>/appmeta/<Titel>/param.sfo      (Spielname)
Aufruf: saves_fixture.py <ordner>   - gibt die Container-Prüfsummen als JSON aus"""
import hashlib
import json
import os
import struct
import sys


def sfo(entries):
    """einfache param.sfo: entries = [(key, bytes)]"""
    keys, data, idx = b"", b"", b""
    for k, v in entries:
        idx += struct.pack("<HHIII", len(keys), 0x0204 if v.endswith(b"\0") else 0x0004, len(v), len(v), len(data))
        keys += k.encode() + b"\0"
        data += v
    while len(keys) % 4:
        keys += b"\0"
    kt = 20 + len(idx)
    return b"\0PSF" + struct.pack("<IIII", 0x101, kt, kt + len(keys), len(entries)) + idx + keys + data


SFO_OFF = 0x2000   # dort liegt im Pseudo-Container die param.sfo (die Attrappe in platform_host.c "hängt" sie ein)


def container(seed, size, ps4=False):
    """Pseudo-Container: Byte 0 = 2 (PS5) / 1 (PS4), 0x60 Byte "Sealed Key" bei 0x800, param.sfo bei 0x2000,
    Rest deterministisch"""
    out = bytearray(hashlib.sha256(seed.encode()).digest() * (size // 32))
    out[0] = 1 if ps4 else 2
    blob = sfo([("TITLE_ID", b"PPSA01234" + bytes(1)), ("ACCOUNT_ID", bytes.fromhex("1122334455667788")),
                ("USER_ID", struct.pack("<I", 0x1c3f8a0e)), ("SAVEDATA_DIRECTORY", b"SAVEDATA00" + bytes(1))])
    out[SFO_OFF:SFO_OFF + 0x1000] = blob.ljust(0x1000, bytes(1))
    return bytes(out)


USERS = {"1c3f8a0e": "Alex", "2d4e9b1f": "Sam", "3a5b7c9d": "Jo"}
SAVES = [  # uid, Titel, Name, Größe
    ("1c3f8a0e", "PPSA01234", "sdimg_SAVEDATA00", 2 << 20),
    ("1c3f8a0e", "PPSA07890", "sdimg_SAVEDATA00", 1 << 20),
    ("2d4e9b1f", "PPSA01234", "sdimg_SAVEDATA00", 3 << 20),
    ("2d4e9b1f", "PPSA07890", "sdimg_SAVEDATA01", 1 << 20),
]
TITLES = {"PPSA01234": "Beispiel-Spiel: Die \"Rückkehr\"", "PPSA07890": "Zweites Spiel"}


def main(root):
    sums = {}
    for uid, name in USERS.items():
        os.makedirs(os.path.join(root, "home", uid), exist_ok=True)
        with open(os.path.join(root, "home", uid, "username.dat"), "wb") as f:
            f.write(name.encode() + b"\0" * (16 - len(name)))
    for uid, title, name, size in SAVES:
        d = os.path.join(root, "home", uid, "savedata_prospero", title)
        os.makedirs(d, exist_ok=True)
        data = container(uid + title + name, size)
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
        sums[f"{uid}/{title}/{name}"] = hashlib.sha256(data).hexdigest()
    # Begleiter und Fremdes, das nicht als Spielstand erscheinen darf
    with open(os.path.join(root, "home", "1c3f8a0e", "savedata_prospero", "PPSA01234", "SAVEDATA00.bin"), "wb") as f:
        f.write(b"\1" * 0x2000)
    with open(os.path.join(root, "home", "1c3f8a0e", "savedata_prospero", "PPSA01234", "klein"), "wb") as f:
        f.write(b"x" * 100)
    os.makedirs(os.path.join(root, "home", "3a5b7c9d", "savedata_prospero", "PPSA00001"), exist_ok=True)   # leer
    for title, name in TITLES.items():
        d = os.path.join(root, "appmeta", title)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "param.sfo"), "wb") as f:
            f.write(sfo([("TITLE_ID", title.encode() + b"\0"), ("TITLE", name.encode() + b"\0")]))
    print(json.dumps(sums))


if __name__ == "__main__":
    main(sys.argv[1])
