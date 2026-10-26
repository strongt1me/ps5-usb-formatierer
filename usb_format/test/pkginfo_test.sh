#!/bin/bash
# Spielname, Version, Art (Spiel/Update/DLC) und Mindest-Firmware aus den Metadaten von .pkg-Dateien (PS4: param.sfo, PS5: param.json)
# Aufruf: test/pkginfo_test.sh   (baut selbst, Port 18121)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18121 || exit 1
U=http://127.0.0.1:18121
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
truncate -s 64M "$W/a.img"
mkdir -p "$W/mnt/usb0"
python3 - "$W/mnt/usb0" <<'PYEOF'
import json, struct, sys
r = sys.argv[1]
def sfo(d):
    keys = b""; data = b""; ents = b""
    for k, v in d.items():
        ko = len(keys); keys += k.encode() + b"\0"
        if isinstance(v, int): raw = struct.pack("<I", v); fmt = 0x0404; ln = 4
        else: raw = v.encode() + b"\0"; fmt = 0x0204; ln = len(raw)
        ents += struct.pack("<HHIII", ko, fmt, ln, ln, len(data)); data += raw
    ks = 20 + len(ents); ds = ks + len(keys)
    return b"\0PSF" + struct.pack("<IIII", 0x101, ks, ds, len(d)) + ents + keys + data
def cnt(entry_id, payload, ctype, off=0x400):
    h = bytearray(0x400)
    h[0:4] = b"\x7fCNT"; struct.pack_into(">I", h, 0x10, 1); struct.pack_into(">I", h, 0x18, 0x100)
    struct.pack_into(">I", h, 0x70, 0x10); struct.pack_into(">I", h, 0x74, ctype)
    h[0x40:0x40 + 36] = b"EP0001-CUSA00001_00-TESTGAME00000000"
    struct.pack_into(">6I", h, 0x100, entry_id, 0, 0, 0, off, len(payload))
    return bytes(h) + payload
def ps4(name, d): open(r + "/" + name, "wb").write(cnt(0x1000, sfo(d), 0x1a))
ps4("spiel.pkg", {"TITLE": "Test Spiel", "TITLE_ID": "CUSA00001", "VERSION": "01.02", "APP_VER": "01.00", "CATEGORY": "gd", "SYSTEM_VER": 0x12008000})
ps4("patch.pkg", {"TITLE": "Test Spiel", "TITLE_ID": "CUSA00001", "VERSION": "01.05", "CATEGORY": "gp", "SYSTEM_VER": 0x09508000})
def ps5(name, j, ctype=0x20):
    meta = cnt(0x2000, ("\ufeff" + json.dumps(j)).encode("utf-8"), ctype)
    fih = bytearray(0x1000)
    fih[0:4] = b"\x7fFIH"; fih[5] = 0x00
    struct.pack_into("<Q", fih, 0x58, 0x1000)
    open(r + "/" + name, "wb").write(bytes(fih) + meta)
loc = {"defaultLanguage": "en-US", "en-US": {"titleName": "PS5 Spiel"}}
ps5("p5spiel.pkg", {"titleId": "PPSA00001", "contentVersion": "01.010.000", "requiredSystemSoftwareVersion": "0x0900000000000000", "localizedParameters": loc})
ps5("p5update.pkg", {"titleId": "PPSA00001", "contentVersion": "01.004.000", "targetContentVersion": "01.003.000", "requiredSystemSoftwareVersion": "0x1342000000000000", "localizedParameters": loc})
ps5("p5dlc.pkg", {"titleId": "PPSA00001", "contentVersion": "01.000.000", "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": "Zusatzpaket"}}}, 0x21)
open(r + "/kaputt.pkg", "wb").write(b"\x7fCNT" + b"\xff" * 300)
open(r + "/leer.pkg", "wb").write(b"")
PYEOF
env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done
f() { q "[(p['title'], p['version'], p['cat'], p['minfw'], p['update']) for p in d['pkgfiles'] if p['name'] == '$1'][0]" <"$W/lib"; }
[ "$(f spiel.pkg)" = "('Test Spiel', '01.02', 'game', '12.00', False)" ] && ok "PS4-Spiel: Name, Version, Art, Firmware 12.00" || bad "PS4-Spiel" "$(f spiel.pkg)"
[ "$(f patch.pkg)" = "('Test Spiel', '01.05', 'update', '9.50', True)" ] && ok "PS4-Update (CATEGORY gp): Art Update, Firmware 9.50" || bad "PS4-Update" "$(f patch.pkg)"
[ "$(f p5spiel.pkg)" = "('PS5 Spiel', '01.010.000', 'game', '9.00', False)" ] && ok "PS5-Spiel: Name, Version, Art, Firmware 9.00 (param.json mit BOM)" || bad "PS5-Spiel" "$(f p5spiel.pkg)"
[ "$(f p5update.pkg)" = "('PS5 Spiel', '01.004.000', 'update', '13.42', True)" ] && ok "PS5-Update (targetContentVersion): Art Update, Firmware 13.42" || bad "PS5-Update" "$(f p5update.pkg)"
[ "$(f p5dlc.pkg)" = "('Zusatzpaket', '01.000.000', 'dlc', '', False)" ] && ok "PS5-Zusatzinhalt (Inhaltstyp 0x21): Art DLC" || bad "PS5-DLC" "$(f p5dlc.pkg)"
[ "$(q "[(p['title'], p['version'], p['cat'], p['minfw']) for p in d['pkgfiles'] if p['name'] == 'kaputt.pkg'][0]" <"$W/lib")" = "('', '', '', '')" ] && [ "$(q 'len(d["pkgfiles"])' <"$W/lib")" = 7 ] && ok "kaputte und leere Pakete: keine Angaben, kein Absturz" || bad "kaputt" "$(f kaputt.pkg)"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Paket-Info-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
