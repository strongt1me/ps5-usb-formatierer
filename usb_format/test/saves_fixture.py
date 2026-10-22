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


SFO_OFF = 0x2000   # Pseudo-Container: param.sfo bei 0x2000, Symbolbild bei 0x3000 (für Inhaltsprüfungen)
ICON_OFF = 0x3000  # und das Symbolbild (icon0.png, ICON_LEN Byte)
ICON_LEN = 0x400

def user_hmac(uid):
    """PARAMS-HMAC (+8, 32 Byte): hier je Benutzer verschieden, damit die Übernahme beim Kopieren prüfbar ist"""
    return hashlib.sha256(("hmac" + uid).encode()).digest()


def container(seed, size, ps4=False, uid="1c3f8a0e", title="PPSA01234", name="sdimg_SAVEDATA00"):
    """Pseudo-Container: Byte 0 = 2 (PS5) / 1 (PS4), 0x60 Byte "Sealed Key" bei 0x800, param.sfo bei 0x2000,
    Symbolbild bei 0x3000, Rest deterministisch"""
    out = bytearray(hashlib.sha256(seed.encode()).digest() * (size // 32))
    out[0] = 1 if ps4 else 2
    d = name[6:] if name.startswith("sdimg_") else name
    params = bytearray(1024)
    params[4:8] = struct.pack("<I", int(uid, 16))
    params[8:40] = user_hmac(uid)
    params[0x28:0x2c] = struct.pack("<I", 1)
    params[0x2C:0x2C + len(title)] = title.encode()
    params[0x38:0x38 + len(title)] = title.encode()
    blob = sfo([("ACCOUNT_ID", bytes.fromhex("1122334455667788")), ("DETAIL", b"Details" + bytes(1)),
                ("MAINTITLE", TITLES.get(title, title).encode() + bytes(1)), ("PARAMS", bytes(params)),
                ("SAVEDATA_BLOCKS", struct.pack("<Q", 96)), ("SAVEDATA_DIRECTORY", d.encode() + bytes(1)),
                ("SAVEDATA_LIST_PARAM", struct.pack("<I", 7)), ("SUBTITLE", b"Kapitel 3" + bytes(1)),
                ("SYSTEM_BLOCKS", struct.pack("<Q", 2)), ("TITLE_ID", title.encode() + bytes(1))])
    out[SFO_OFF:SFO_OFF + 0x1000] = blob.ljust(0x1000, bytes(1))
    out[ICON_OFF:ICON_OFF + ICON_LEN] = (bytes.fromhex("89504e470d0a1a0a") + hashlib.sha256(("icon" + seed).encode()).digest() * 40)[:ICON_LEN]
    return bytes(out)


USERS = {"1c3f8a0e": "Alex", "2d4e9b1f": "Sam", "3a5b7c9d": "Jo"}
SAVES = [  # uid, Titel, Name, Größe
    ("1c3f8a0e", "PPSA01234", "sdimg_SAVEDATA00", 2 << 20),
    ("1c3f8a0e", "PPSA07890", "sdimg_SAVEDATA00", 1 << 20),
    ("2d4e9b1f", "PPSA01234", "sdimg_SAVEDATA00", 3 << 20),
    ("2d4e9b1f", "PPSA07890", "sdimg_SAVEDATA01", 1 << 20),
    ("1c3f8a0e", "PPSA09999", "sdimg_EXTRA", 1 << 20),
]
TITLES = {"PPSA01234": "Beispiel-Spiel: Die \"Rückkehr\"", "PPSA07890": "Zweites Spiel", "PPSA09999": "Drittes Spiel"}


def main(root):
    sums = {}
    for uid, name in USERS.items():
        os.makedirs(os.path.join(root, "home", uid), exist_ok=True)
        with open(os.path.join(root, "home", uid, "username.dat"), "wb") as f:
            f.write(name.encode() + b"\0" * (16 - len(name)))
    for uid, title, name, size in SAVES:
        d = os.path.join(root, "home", uid, "savedata_prospero", title)
        os.makedirs(d, exist_ok=True)
        data = container(uid + title + name, size, uid=uid, title=title, name=name)
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
