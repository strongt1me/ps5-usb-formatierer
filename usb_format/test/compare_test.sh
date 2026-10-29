#!/usr/bin/env bash
# Zwei Kopien desselben Spiels vergleichen (Ordner und Abbilder), fehlende Pflichtdateien bei Ordner-Spielen, Lesetempo im Verlauf.
# Aufruf: test/compare_test.sh   (baut selbst, Port 18131)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18131 || exit 1
U=http://127.0.0.1:18131
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 300); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
key() { q "[t['key'] for t in d['titles'] if t['path'].endswith('/$1')][0]" <"$W/lib"; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
truncate -s 64M "$W/a.img"
H="$W/mnt/usb0/homebrew"
game() {   # game <Ordner> <Titel-ID>: Ordner-Spiel mit eboot.bin, Titelbild und Daten
    mkdir -p "$H/$1/sce_sys" "$H/$1/data"
    printf '{"titleId":"%s","contentVersion":"01.000.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Spiel %s"}}}' "$2" "$2" >"$H/$1/sce_sys/param.json"
    printf 'ELF' >"$H/$1/eboot.bin"
    printf 'PNG' >"$H/$1/sce_sys/icon0.png"
}
game Eins PPSA00011
head -c 2000000 /dev/urandom >"$H/Eins/data/a.bin"
printf 'x' >"$H/Eins/data/b.txt"
mkdir -p "$H/Kopie" && cp -a "$H/Eins/." "$H/Kopie/"
mkdir -p "$H/Anders" && cp -a "$H/Eins/." "$H/Anders/"
printf 'y' >"$H/Anders/data/b.txt"; rm "$H/Anders/data/a.bin"; printf 'neu' >"$H/Anders/data/c.txt"
game Kaputt PPSA00012
rm "$H/Kaputt/eboot.bin" "$H/Kaputt/sce_sys/icon0.png"
env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
library
cmp() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "{\"keys\":\"$1\"}" "$U/api/game/compare"; }

[ "$(cmp "$(key Eins),$(key Kopie)")" = 202 ] && wait_job && [ "$(q 'd["result"]["same"], d["ok"]' <"$W/st")" = "(True, True)" ] && grep -q 'Inhalt gleich' "$W/st" \
    && ok "gleiche Ordner-Kopien: als Byte für Byte gleich erkannt" || bad "gleiche Kopien" "$(cat "$W/st" "$W/body")"
[ "$(cmp "$(key Eins),$(key Anders)")" = 202 ] && wait_job \
    && [ "$(q 'd["result"]["same"], d["result"]["changed"], d["result"]["only_first"], d["result"]["only_second"]' <"$W/st")" = "(False, 1, 1, 1)" ] \
    && ok "verschiedene Kopien: 1 geändert, 1 nur in der ersten, 1 nur in der zweiten" || bad "verschiedene Kopien" "$(cat "$W/st")"
[ ! -e "$W/cfg/compare.manifest.tmp" ] && [ ! -e "$H/Eins.manifest.sha256" ] && ok "keine Reste: weder Zwischenliste noch Prüfsummenliste angelegt" || bad "Reste" "$(ls -a "$W/cfg" "$H")"
[ "$(cmp "$(key Eins),$(key Kaputt)")" = 400 ] && ok "verschiedene Spiele: abgelehnt" || bad "verschiedene Spiele" "$(cat "$W/body")"
[ "$(cmp "$(key Eins)")" = 400 ] && ok "nur ein Spiel: abgelehnt" || bad "ein Spiel"
[ "$(cmp "$(key Eins),$(key Eins)")" = 400 ] && ok "zweimal dasselbe Spiel: abgelehnt (nicht mit sich selbst vergleichen)" || bad "dasselbe Spiel" "$(cat "$W/body")"
[ "$(q "[t.get('missing','') for t in d['titles'] if t['path'].endswith('/Kaputt')][0]" <"$W/lib")" = "eboot.bin, sce_sys/icon0.png" ] \
    && [ "$(q "[t.get('missing','') for t in d['titles'] if t['path'].endswith('/Eins')][0]" <"$W/lib")" = "" ] \
    && ok "fehlende Pflichtdateien gemeldet, vollständiges Spiel ohne Meldung" || bad "fehlende Dateien" "$(grep -o '"missing":"[^"]*"' "$W/lib")"

# Abbilder: gleich (Kopie) und verschieden; dieselbe Titel-ID über eine eingebettete param.json gibt es im Test nicht, beide ohne ID
head -c 3000000 /dev/urandom >"$H/x.ffpfs"; cp "$H/x.ffpfs" "$H/y.ffpfs"; head -c 3000000 /dev/urandom >"$H/z.ffpfs"
library
[ "$(cmp "$(key x.ffpfs),$(key y.ffpfs)")" = 202 ] && wait_job && [ "$(q 'd["result"]["same"]' <"$W/st")" = True ] && ok "gleiche Abbilder: gleich" || bad "gleiche Abbilder" "$(cat "$W/st" "$W/body")"
[ "$(cmp "$(key x.ffpfs),$(key z.ffpfs)")" = 202 ] && wait_job && [ "$(q 'd["result"]["same"]' <"$W/st")" = False ] && grep -q 'nicht gleich' "$W/st" && ok "verschiedene Abbilder: verschieden" || bad "verschiedene Abbilder" "$(cat "$W/st")"
[ "$(cmp "$(key x.ffpfs),$(key Eins)")" = 400 ] && ok "Abbild und Ordner (verschiedene Titel-ID): abgelehnt" || bad "Abbild/Ordner" "$(cat "$W/body")"

# Tempo messen: das Lesetempo landet im Verlauf (für die Ziel-Auswahl)
DEV=$(curl -s "$U/api/devices" | q 'd["devices"][0]["name"], d["devices"][0]["size"]')
[ "$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$(python3 -c 'import sys; n, s = eval(sys.argv[1]); print("{\"device\":\"%s\",\"size\":%d}" % (n, s))' "$DEV")" "$U/api/speed")" = 202 ] && wait_job \
    && curl -s "$U/api/history" | q '[e for e in d["entries"] if e["kind"] == "speed"][0].get("mbps", 0) > 0' | grep -q True \
    && ok "Tempo messen: Lesetempo (mbps) im Verlauf" || bad "Tempo im Verlauf" "$(cat "$W/body"; curl -s "$U/api/history" | head -c 400)"
curl -s "$U/api/history" | q '[e["kind"] for e in d["entries"]].count("game_compare") >= 4' | grep -q True && ok "Vergleiche im Verlauf" || bad "Verlauf"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Vergleichs-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
