#!/usr/bin/env bash
# Vorabprüfung vor dem Kopieren (/api/move mit "check": Platz am Ziel, FAT32 und Dateien über 4 GB) und Vergleich der .pkg-Dateien
# mit dem, was liegt (installierte Version, Firmware der PS5): /api/library -> pkgfiles[].vs/inst/fwhigh, fw.
# Aufruf: test/precheck_test.sh   (baut selbst, Port 18122)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18122 || exit 1
U=http://127.0.0.1:18122
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
truncate -s 64M "$W/a.img" "$W/b.img"
mkdir -p "$W/mnt/usb0/homebrew/Spiel" "$W/mnt/usb0/homebrew/Gross" "$W/mnt/usb1" "$W/mnt/usb0/homebrew/Spiel/sce_sys" "$W/mnt/usb0/homebrew/Gross/sce_sys"
printf '{"titleId":"PPSA00001","contentVersion":"01.005.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"PS5 Spiel"}}}' >"$W/mnt/usb0/homebrew/Spiel/sce_sys/param.json"
head -c 100000 /dev/urandom >"$W/mnt/usb0/homebrew/Spiel/data.bin"
printf '{"titleId":"PPSA00002","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Grosses Spiel"}}}' >"$W/mnt/usb0/homebrew/Gross/sce_sys/param.json"
truncate -s 5G "$W/mnt/usb0/homebrew/Gross/riesig.bin"
python3 - "$W/mnt/usb0" <<'PYEOF'
import json, struct, sys
r = sys.argv[1]
def cnt(entry_id, payload, ctype, off=0x400):
    h = bytearray(0x400)
    h[0:4] = b"\x7fCNT"; struct.pack_into(">I", h, 0x10, 1); struct.pack_into(">I", h, 0x18, 0x100)
    struct.pack_into(">I", h, 0x70, 0x10); struct.pack_into(">I", h, 0x74, ctype)
    struct.pack_into(">6I", h, 0x100, entry_id, 0, 0, 0, off, len(payload))
    return bytes(h) + payload
def ps5(name, j, ctype=0x20):
    meta = cnt(0x2000, ("﻿" + json.dumps(j)).encode("utf-8"), ctype)
    fih = bytearray(0x1000)
    fih[0:4] = b"\x7fFIH"; fih[5] = 0x00
    struct.pack_into("<Q", fih, 0x58, 0x1000)
    open(r + "/" + name, "wb").write(bytes(fih) + meta)
loc = {"defaultLanguage": "en-US", "en-US": {"titleName": "PS5 Spiel"}}
ps5("neu.pkg", {"titleId": "PPSA00001", "contentVersion": "01.010.000", "requiredSystemSoftwareVersion": "0x0900000000000000", "localizedParameters": loc})
ps5("alt.pkg", {"titleId": "PPSA00001", "contentVersion": "01.004.000", "targetContentVersion": "01.003.000", "requiredSystemSoftwareVersion": "0x0800000000000000", "localizedParameters": loc})
ps5("gleich.pkg", {"titleId": "PPSA00001", "contentVersion": "01.005.000", "requiredSystemSoftwareVersion": "0x0800000000000000", "localizedParameters": loc})
ps5("fremd.pkg", {"titleId": "PPSA00077", "contentVersion": "01.000.000", "requiredSystemSoftwareVersion": "0x1342000000000000", "localizedParameters": loc})
PYEOF
start() {
    [ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }
    env USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" USBFMT_FW=8.20 "$@" "$W/srv" >>"$W/log" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && return; sleep 0.1; done
}
key() { q "[t['key'] for t in d['titles'] if t['path'].endswith('/$1')][0]" <"$W/lib"; }
pk()  { q "[(p['vs'], p['inst'], p['installed'], p['fwhigh']) for p in d['pkgfiles'] if p['name'] == '$1'][0]" <"$W/lib"; }

start
library
[ "$(q 'd["fw"]' <"$W/lib")" = "8.20" ] && ok "Firmware der PS5 steht in /api/library (8.20)" || bad "fw" "$(q 'd.get("fw")' <"$W/lib")"
[ "$(q "[t['version'] for t in d['titles'] if t['id'] == 'PPSA00001'][0]" <"$W/lib")" = "01.005.000" ] && ok "Installierte Version aus param.json" || bad "Version"
[ "$(pk neu.pkg)" = "('newer', '01.005.000', True, True)" ] && ok "neu.pkg: neuer als installiert, braucht Firmware 9.00 (PS5 hat 8.20)" || bad "neu.pkg" "$(pk neu.pkg)"
[ "$(pk alt.pkg)" = "('older', '01.005.000', True, False)" ] && ok "alt.pkg: älter als installiert, Firmware passt" || bad "alt.pkg" "$(pk alt.pkg)"
[ "$(pk gleich.pkg)" = "('same', '01.005.000', True, False)" ] && ok "gleich.pkg: schon in dieser Version installiert" || bad "gleich.pkg" "$(pk gleich.pkg)"
[ "$(pk fremd.pkg)" = "('', '', False, True)" ] && ok "fremd.pkg: nicht installiert, braucht Firmware 13.42" || bad "fremd.pkg" "$(pk fremd.pkg)"

KS=$(key Spiel)
KG=$(key Gross)
curl -s -H "$J" -d "{\"keys\":\"$KS\",\"device\":\"da1\",\"check\":true}" "$U/api/move" >"$W/c1"
[ "$(q '(d["fits"], d["fat32"], d["big"], d["need"] >= 100000)' <"$W/c1")" = "(True, False, [], True)" ] && ok "Vorabprüfung: kleines Spiel passt, kein FAT32" || bad "Prüfung klein" "$(cat "$W/c1")"
curl -s "$U/api/status" | grep -q '"busy":false' && ok "Vorabprüfung startet keinen Auftrag" || bad "Auftrag gestartet"
curl -s -H "$J" -d "{\"keys\":\"$KG\",\"device\":\"da1\",\"check\":true}" "$U/api/move" >"$W/c2"
[ "$(q '(d["fits"], d["fat32"], d["big"])' <"$W/c2")" = "(True, False, [])" ] && ok "ohne FAT32: Datei über 4 GB wird nicht beanstandet" || bad "Prüfung groß" "$(cat "$W/c2")"
start USBFMT_TARGET_FAT32=1
library
curl -s -H "$J" -d "{\"keys\":\"$KG\",\"device\":\"da1\",\"check\":true}" "$U/api/move" >"$W/c3"
[ "$(q '(d["fat32"], d["big"])' <"$W/c3")" = "(True, ['riesig.bin'])" ] && ok "FAT32-Ziel: riesig.bin (5 GB) wird genannt" || bad "FAT32" "$(cat "$W/c3")"
truncate -s 15T "$W/mnt/usb0/homebrew/Spiel/riesig2.bin" 2>/dev/null
curl -s -H "$J" -d "{\"keys\":\"$KS\",\"device\":\"da1\",\"check\":true}" "$U/api/move" >"$W/c4"
FREE=$(df -B1 --output=avail "$W" | tail -1)
if [ "$FREE" -lt $((15 * 1024 * 1024 * 1024 * 1024)) ]; then
    [ "$(q 'd["fits"]' <"$W/c4")" = "False" ] && ok "zu wenig Platz am Ziel: fits=false (nötig mehr als frei)" || bad "Platz" "$(cat "$W/c4")"
else
    echo "ÜBERSPRUNGEN  Platzprüfung (Dateisystem hat mehr als 15 TB frei)"
fi
curl -s -o /dev/null -w '%{http_code}' -H "$J" -d '{"keys":"zzzz","device":"da1","check":true}' "$U/api/move" | grep -q '^40' && ok "ungültige Schlüssel bei der Prüfung: Fehler, kein Absturz" || bad "ungültiger Schlüssel"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Vorabprüfungs-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
