#!/usr/bin/env python3
"""Legt einen Prüfstand für den Spielstand-Teil an (wie die PS5, mit dem Host-Ersatz für das Einhängen, src/savemnt.c):
    <ordner>/home/<uid>/username.dat
    <ordner>/home/<uid>/savedata_prospero/<PPSA>/sdimg_<Name>          PS5-Abbild (Kopf Version 2, Kennung bei 0x100)
    <ordner>/home/<uid>/savedata/<CUSA>/sdimg_<Name> + <Name>.bin      PS4-Abbild (Kopf Version 1) mit Schlüssel
    <ordner>/store/<Kennung>/...                                       was ein Einhängen des Abbilds zeigt
    <ordner>/store/<Kennung>.param                                     das param.sfo im Kopf des Abbilds (PS5)
    <ordner>/sys/<savedata|savedata_prospero>/<uid>/db/user/savedata.db  Spielstand-Datenbank (Aufbau der Konsole)
    <ordner>/appmeta/<Titel>/param.sfo (Spielname)
Aufruf: saves_fixture.py <ordner>   - gibt die Kennungen der Abbilder als JSON aus"""
import hashlib
import json
import os
import sqlite3
import struct
import sys

USERS = {"1c3f8a0e": "Alex", "2d4e9b1f": "Sam", "3a5b7c9d": "Jo"}
ACCOUNTS = {"Alex": 0x1122334455667788, "Sam": 0xAABBCCDD00112233}   # Jo hat kein Konto in der Registry
TITLES = {"PPSA01234": "Beispiel-Spiel: Die \"Rückkehr\"", "PPSA07890": "Zweites Spiel", "PPSA09999": "Drittes Spiel", "CUSA00001": "PS4-Beispiel"}
# uid, Titel, Name, Blöcke (32 KiB)
SAVES = [
    ("1c3f8a0e", "PPSA01234", "SAVEDATA00", 64),
    ("1c3f8a0e", "PPSA07890", "SAVEDATA00", 40),
    ("2d4e9b1f", "PPSA07890", "SAVEDATA01", 40),
    ("1c3f8a0e", "PPSA09999", "EXTRA", 40),
    ("1c3f8a0e", "PPSA09999", "EXTRA2", 40),   # zweiter Spielstand desselben Spiels (wie SaveData und sce_sdmemory)
]
DB_SCHEMA = ("CREATE TABLE savedata (id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,title_id NOT NULL, dir_name NOT NULL, main_title NOT NULL, sub_title , detail , "
             "tmp_dir_name , is_broken , user_param , blocks , free_blocks , size_kib , mtime NOT NULL, fake_broken , account_id , user_id , faked_owner , "
             "cloud_icon_url , cloud_revision , game_title_id NOT NULL, system_blocks , convert_performance , account_id_in_usb_path );"
             "CREATE INDEX savedata_index on savedata(title_id, dir_name, user_param, size_kib, mtime, game_title_id);PRAGMA user_version=11;")


def sfo(entries):
    """param.sfo: entries = [(key, bytes, platz)]"""
    keys, data, idx = b"", b"", b""
    for e in entries:
        k, v = e[0], e[1]
        cap = e[2] if len(e) > 2 else len(v)
        idx += struct.pack("<HHIII", len(keys), 0x0204 if v.endswith(b"\0") else 0x0004, len(v), cap, len(data))
        keys += k.encode() + b"\0"
        data += v.ljust(cap, b"\0")
    while len(keys) % 4:
        keys += b"\0"
    kt = 20 + len(idx)
    return b"\0PSF" + struct.pack("<IIII", 0x101, kt, kt + len(keys), len(entries)) + idx + keys + data


def user_value(uid):
    """32-Byte-Besitzerwert (PARAMS +8): je Benutzer verschieden"""
    return hashlib.sha256(("owner" + uid).encode()).digest()


def save_sfo(uid, title, slot, blocks, ps4=False, account=None, value=None):
    params = bytearray(1024)
    params[4:8] = struct.pack("<I", int(uid, 16))
    params[8:40] = value if value is not None else user_value(uid)
    params[0x100:0x110] = hashlib.md5((title + slot).encode()).digest()   # eigener Teil des Spielstands
    acc = account if account is not None else ACCOUNTS.get(USERS.get(uid, ""), 0)
    ent = [("ACCOUNT_ID", struct.pack("<Q", acc)), ("DETAIL", b"Details" + bytes(1), 64),
           ("MAINTITLE", TITLES.get(title, title).encode() + bytes(1), 128), ("PARAMS", bytes(params)),
           ("SAVEDATA_DIRECTORY", slot.encode() + bytes(1), 32), ("SAVEDATA_LIST_PARAM", struct.pack("<I", 7)),
           ("SUBTITLE", b"Kapitel 3" + bytes(1), 128), ("TITLE_ID", title.encode() + bytes(1), 12)]
    if not ps4:
        ent += [("FORMAT", b"ppr\0", 4), ("SAVEDATA_BLOCKS", struct.pack("<Q", blocks)), ("SYSTEM_BLOCKS", struct.pack("<Q", 0))]
    return sfo(sorted(ent, key=lambda e: e[0]))


def image(path, ident, blocks, ps4=False, bs=32768):
    """Abbild: Kopf wie die Konsole (Version, Magic 20130315, mode 0x0d, Blockgröße, 1 Sicherungskopf, Blockzahl), Kennung bei 0x100"""
    head = bytearray(0x400)
    head[0:8] = struct.pack("<Q", 1 if ps4 else 2)
    head[8:16] = struct.pack("<Q", 20130315)
    if not ps4:
        head[0x1c] = 0x0d
        head[0x24] = 1
        head[0x38:0x40] = struct.pack("<Q", blocks)
    head[0x20:0x24] = struct.pack("<I", bs)
    tag = ("usbfmt-host-id:" + ident).encode()
    head[0x100:0x100 + len(tag)] = tag
    with open(path, "wb") as f:
        f.write(bytes(head))
        f.truncate(blocks * bs)


def write(path, data, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    if mode is not None:
        os.chmod(path, mode)


def ident_of(uid, title, slot):
    return hashlib.sha1((uid + title + slot).encode()).hexdigest()[:16]


def content(uid, title, slot):
    """Dateien im Spielstand (Name -> Inhalt)"""
    seed = (uid + title + slot).encode()
    return {"data/main.sav": hashlib.sha256(seed).digest() * 900, "data/sub/extra.bin": b"x" * 777 + seed,
            "sce_sys/icon0.png": bytes.fromhex("89504e470d0a1a0a") + hashlib.sha256(b"icon" + seed).digest() * 20}


def make_db(path, rows=()):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    db = sqlite3.connect(path)
    db.executescript(DB_SCHEMA)
    for r in rows:
        db.execute("INSERT INTO savedata (title_id, dir_name, main_title, sub_title, detail, tmp_dir_name, is_broken, user_param, blocks, free_blocks, size_kib, "
                   "mtime, fake_broken, account_id, user_id, faked_owner, cloud_icon_url, cloud_revision, game_title_id, system_blocks, convert_performance, "
                   "account_id_in_usb_path) VALUES (?,?,?,'','',  '',0,0,?,0,?, '2025-01-01T00:00:00.00Z',0,?,?,0,'',0,?,0,0,'0000000000000000')",
                   (r[0], r[1], r[2], r[3], r[3] * 32, r[4], r[5], r[0]))
    db.commit()
    db.close()


def main(root):
    out = {}
    store = os.path.join(root, "store")
    os.makedirs(store, exist_ok=True)
    for uid, name in USERS.items():
        write(os.path.join(root, "home", uid, "username.dat"), name.encode() + b"\0" * (16 - len(name)))
    for uid, title, slot, blocks in SAVES:
        d = os.path.join(root, "home", uid, "savedata_prospero", title)
        os.makedirs(d, exist_ok=True)
        ident = ident_of(uid, title, slot)
        image(os.path.join(d, "sdimg_" + slot), ident, blocks)
        for rel, data in content(uid, title, slot).items():
            write(os.path.join(store, ident, rel), data, 0o644)
        write(os.path.join(store, ident, "sce_sys", "param.sfo"), bytes(0x8000))   # im Dateisystem: ein Block Nullen
        write(os.path.join(store, ident + ".param"), save_sfo(uid, title, slot, blocks))   # das echte im Kopf
        out[f"{uid}/{title}/sdimg_{slot}"] = ident
    # PS5-Spielstand ohne Blob im Kopf: das param.sfo des Dateisystems ist das echte
    uid, title, slot = "2d4e9b1f", "PPSA01234", "SAVEDATA00"
    d = os.path.join(root, "home", uid, "savedata_prospero", title)
    os.makedirs(d, exist_ok=True)
    ident = ident_of(uid, title, slot)
    image(os.path.join(d, "sdimg_" + slot), ident, 64)
    for rel, data in content(uid, title, slot).items():
        write(os.path.join(store, ident, rel), data, 0o644)
    write(os.path.join(store, ident, "sce_sys", "param.sfo"), save_sfo(uid, title, slot, 64))
    out[f"{uid}/{title}/sdimg_{slot}"] = ident
    # PS4 (Alex): sdimg_PS4DATA + PS4DATA.bin, dazu eine Systemkopie und ein Abbild ohne Schlüssel
    d = os.path.join(root, "home", "1c3f8a0e", "savedata", "CUSA00001")
    os.makedirs(d, exist_ok=True)
    for slot in ("PS4DATA", "sce_bu_PS4DATA"):
        ident = ident_of("1c3f8a0e", "CUSA00001", slot)
        image(os.path.join(d, "sdimg_" + slot), ident, 1024, ps4=True)
        write(os.path.join(d, slot + ".bin"), hashlib.sha256(slot.encode()).digest() * 3)
        for rel, data in content("1c3f8a0e", "CUSA00001", slot).items():
            write(os.path.join(store, ident, rel), data, 0o640)
        write(os.path.join(store, ident, "sce_sys", "param.sfo"), save_sfo("1c3f8a0e", "CUSA00001", slot, 0, ps4=True))
        out["1c3f8a0e/CUSA00001/sdimg_%s#PS4" % slot] = ident
    image(os.path.join(d, "sdimg_ohneschluessel"), "0000000000000000", 1024, ps4=True)
    # Fremdes, das nicht als Spielstand erscheinen darf
    write(os.path.join(root, "home", "1c3f8a0e", "savedata_prospero", "PPSA01234", "SAVEDATA00.bin"), b"\1" * 0x2000)
    write(os.path.join(root, "home", "1c3f8a0e", "savedata_prospero", "PPSA01234", "klein"), b"x" * 100)
    os.makedirs(os.path.join(root, "home", "3a5b7c9d", "savedata_prospero", "PPSA00001"), exist_ok=True)
    # Datenbanken: Alex PS5 mit Zeilen, Sam PS5 nur der Ordner (die Datenbank entsteht), Alex PS4 mit Zeile, Jo ohne Ordner
    make_db(os.path.join(root, "sys", "savedata_prospero", "1c3f8a0e", "db", "user", "savedata.db"),
            [("PPSA01234", "SAVEDATA00", "Alt", 64, ACCOUNTS["Alex"], 0x1c3f8a0e), ("PPSA07890", "SAVEDATA00", "Alt2", 40, ACCOUNTS["Alex"], 0x1c3f8a0e)])
    os.makedirs(os.path.join(root, "sys", "savedata_prospero", "2d4e9b1f", "db", "user"), exist_ok=True)
    make_db(os.path.join(root, "sys", "savedata", "1c3f8a0e", "db", "user", "savedata.db"),
            [("CUSA00001", "PS4DATA", "Alt", 1024, ACCOUNTS["Alex"], 0x1c3f8a0e)])
    # Apps (wie YouTube): bleiben unberührt - eine PS5-App mit param.json (applicationCategoryType != 0) und eine PS4-Systemanwendung
    d = os.path.join(root, "home", "1c3f8a0e", "savedata_prospero", "PPSA88888")
    os.makedirs(d, exist_ok=True)
    image(os.path.join(d, "sdimg_APPDATA"), ident_of("1c3f8a0e", "PPSA88888", "APPDATA"), 40)
    write(os.path.join(root, "appmeta", "PPSA88888", "param.json"), b'{"applicationCategoryType":65536,"titleId":"PPSA88888"}')
    d = os.path.join(root, "home", "1c3f8a0e", "savedata", "NPXS20001")
    os.makedirs(d, exist_ok=True)
    image(os.path.join(d, "sdimg_SYS"), ident_of("1c3f8a0e", "NPXS20001", "SYS"), 1024, ps4=True)
    write(os.path.join(d, "SYS.bin"), b"\1" * 96)
    for title, name in TITLES.items():
        write(os.path.join(root, "appmeta", title, "param.sfo"), sfo([("TITLE_ID", title.encode() + b"\0"), ("TITLE", name.encode() + b"\0")]))
    print(json.dumps(out))


if __name__ == "__main__":
    main(sys.argv[1])
