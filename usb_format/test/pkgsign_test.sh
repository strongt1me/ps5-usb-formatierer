#!/usr/bin/env bash
# PS4-fPKG vom offiziellen PKG unterscheiden (src/pkgsign.c): künstliche Paketköpfe, deren Image-Schlüssel mit dem Fake-Schlüssel
# (fPKG) bzw. einem anderen Schlüssel (offiziell) verschlüsselt ist; kaputte und fremde Dateien: nicht feststellbar.
# Aufruf: test/pkgsign_test.sh
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gcc -Wall -Wextra -Werror -O2 -o "$W/t" test/pkgsign_test.c src/pkgsign.c src/sha256.c || exit 1
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }

python3 - "$W" <<'PYEOF' || exit 1
import hashlib, os, re, struct, sys
W = sys.argv[1]
txt = open("src/pkgsign_keys.h", encoding="utf-8").read()
def arr(name):
    m = re.search(r"%s\[\d+\] = \{(.*?)\};" % name, txt, re.S)
    return int.from_bytes(bytes(int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))), "big")
def pub(tag):
    return arr(tag + "_P") * arr(tag + "_Q")

def pad(msg, rnd):
    ps = bytes(b % 255 + 1 for b in rnd[:256 - 3 - len(msg)])
    return b"\x00\x02" + ps + b"\x00" + msg

def rsa_enc(tag, msg, rnd):
    n = pub(tag)
    return pow(int.from_bytes(pad(msg, rnd), "big"), 65537, n).to_bytes(256, "big")

# AES-128 (Verschlüsseln) für die Prüfung
def xt(a): return ((a << 1) ^ 0x11b) & 0xff if a & 0x80 else a << 1
sbox = [0] * 256
p = q = 1
while True:
    p = (p ^ (p << 1) ^ (0x1b if p & 0x80 else 0)) & 0xff
    q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xff
    if q & 0x80: q ^= 0x09
    r = lambda v, s: ((v << s) | (v >> (8 - s))) & 0xff
    sbox[p] = q ^ r(q, 1) ^ r(q, 2) ^ r(q, 3) ^ r(q, 4) ^ 0x63
    if p == 1: break
sbox[0] = 0x63
def expand(key):
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]; rc = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = [sbox[b] for b in t[1:] + t[:1]]; t[0] ^= rc; rc = xt(rc)
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    return [sum(w[r * 4:r * 4 + 4], []) for r in range(11)]
def enc_block(rk, b):
    s = [x ^ k for x, k in zip(b, rk[0])]
    for r in range(1, 11):
        s = [sbox[x] for x in s]
        s = [s[((i // 4 + i % 4) % 4) * 4 + i % 4] for i in range(16)]   # ShiftRows
        if r < 10:
            n = []
            for c in range(4):
                a = s[c * 4:c * 4 + 4]
                n += [xt(a[0]) ^ xt(a[1]) ^ a[1] ^ a[2] ^ a[3], a[0] ^ xt(a[1]) ^ xt(a[2]) ^ a[2] ^ a[3],
                      a[0] ^ a[1] ^ xt(a[2]) ^ xt(a[3]) ^ a[3], xt(a[0]) ^ a[0] ^ a[1] ^ a[2] ^ xt(a[3])]
            s = n
        s = [x ^ k for x, k in zip(s, rk[r])]
    return bytes(s)
def cbc_enc(key, iv, data):
    rk = expand(key); prev = iv; out = b""
    for i in range(0, len(data), 16):
        blk = enc_block(rk, bytes(a ^ b for a, b in zip(data[i:i + 16], prev))); out += blk; prev = blk
    return out

assert enc_block(expand(bytes.fromhex("000102030405060708090a0b0c0d0e0f")), bytes.fromhex("00112233445566778899aabbccddeeff")).hex() == "69c4e0d86a7b0430d8cdb78070b4c55a"

def build(image_tag, seed):
    rnd = hashlib.sha512(seed).digest() * 16
    dk3 = hashlib.sha256(seed + b"dk3").digest()
    ekpfs = hashlib.sha256(seed + b"ekpfs").digest()
    keys = b"".join(rsa_enc("DK3", dk3, rnd[i:i + 256]) if i == 3 else os.urandom(256) for i in range(7))
    entry_keys = b"\x11" * 32 + b"\x22" * 224 + keys
    table, ent_off = 0x2000, 0x3000
    e10 = struct.pack(">6I8x", 0x10, 0, 0, 0, ent_off, len(entry_keys))
    key_off = ent_off + 0x1000
    e20 = struct.pack(">6I8x", 0x20, 0, 0, 0, key_off, 256)
    ivk = hashlib.sha256(e20 + dk3).digest()
    img = cbc_enc(ivk[16:32], ivk[:16], rsa_enc(image_tag, ekpfs, rnd[300:556]))
    data = bytearray(0x6000)
    data[0:4] = b"\x7fCNT"
    struct.pack_into(">I", data, 0x10, 2)
    struct.pack_into(">I", data, 0x18, table)
    data[table:table + 64] = e10 + e20
    data[ent_off:ent_off + len(entry_keys)] = entry_keys
    data[key_off:key_off + 256] = img
    return bytes(data)

open(W + "/fake.pkg", "wb").write(build("FAKE", b"a"))
open(W + "/fake2.pkg", "wb").write(build("FAKE", b"b"))
open(W + "/retail.pkg", "wb").write(build("DK3", b"c"))   # Image-Schlüssel mit einem anderen Schlüssel: Fake-Schlüssel passt nicht
open(W + "/kurz.pkg", "wb").write(b"\x7fCNT" + b"\0" * 20)
open(W + "/fremd.pkg", "wb").write(b"XXXX" * 100)
d = bytearray(build("FAKE", b"d")); k3 = 0x3000 + 32 + 224 + 3 * 256; d[k3:k3 + 256] = os.urandom(256)   # Schlüssel 3 kaputt
open(W + "/kaputt.pkg", "wb").write(bytes(d))
PYEOF

res() { "$W/t" "$W/$1" | tr -d '\n'; }
[ "$(res fake.pkg)" = 1 ] && [ "$(res fake2.pkg)" = 1 ] && ok "fPKG (Image-Schlüssel mit dem Fake-Schlüssel): 1" || bad "fPKG" "$(res fake.pkg) $(res fake2.pkg)"
[ "$(res retail.pkg)" = 0 ] && ok "offizielles PKG (anderer Schlüssel): 0" || bad "offiziell" "$(res retail.pkg)"
[ "$(res kurz.pkg)" = -1 ] && [ "$(res fremd.pkg)" = -1 ] && ok "zu kurz bzw. kein PS4-Paket: nicht feststellbar (-1)" || bad "kurz/fremd" "$(res kurz.pkg) $(res fremd.pkg)"
[ "$(res kaputt.pkg)" = -1 ] && ok "Schlüssel 3 kaputt: nicht feststellbar (-1)" || bad "kaputt" "$(res kaputt.pkg)"
[ "$("$W/t" "$W/gibtsnicht.pkg" | tr -d '\n')" = -1 ] && ok "fehlende Datei: -1" || bad "fehlende Datei"

[ "$fails" = 0 ] && echo "Alle fPKG-Erkennungs-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
