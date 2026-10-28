#!/usr/bin/env bash
# Prüfsumme anlegen (/api/game/checksum), Spiel-Einzelheiten (/api/game/info) und liegengebliebene Teilkopien (library: partials, /api/partials/discard).
# Aufruf: test/partials_test.sh   (baut selbst, Port 18126)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18126 || exit 1
U=http://127.0.0.1:18126
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 300); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
key() { q "[t['key'] for t in d['titles'] if t['path'].endswith('/$1')][0]" <"$W/lib"; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
post() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$1" "$U/api/$2"; }
truncate -s 64M "$W/a.img" "$W/b.img"
H="$W/mnt/usb0/homebrew"
mkdir -p "$H/Ordner/sce_sys" "$W/mnt/usb1"
printf '{"titleId":"PPSA00009","contentVersion":"01.002.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Ordnerspiel"}}}' >"$H/Ordner/sce_sys/param.json"
head -c 700000 /dev/urandom >"$H/Ordner/gross.bin"; head -c 1000 /dev/urandom >"$H/Ordner/klein.bin"
head -c 2000000 /dev/urandom >"$H/neu.ffpfs"
head -c 2000000 /dev/urandom >"$H/alt.ffpfs"; sha256sum "$H/alt.ffpfs" | cut -d' ' -f1 >"$H/alt.ffpfs.sha256"
env USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
library

# ---- Prüfsumme anlegen
KN=$(key neu.ffpfs); KA=$(key alt.ffpfs); KO=$(key Ordner)
[ "$(post "{\"keys\":\"$KN,$KA,$KO\"}" game/checksum)" = 202 ] && wait_job && [ "$(q 'd["result"]["ok"], d["result"]["have"], d["result"]["none"]' <"$W/st")" = "(1, 1, 1)" ] \
    && ok "Prüfsumme anlegen: 1 angelegt, 1 schon vorhanden, 1 Ordner-Spiel übersprungen" || bad "Prüfsumme anlegen" "$(cat "$W/st")"
[ -f "$H/neu.ffpfs.sha256" ] && [ "$(cut -d' ' -f1 "$H/neu.ffpfs.sha256")" = "$(sha256sum "$H/neu.ffpfs" | cut -d' ' -f1)" ] && grep -q '  neu.ffpfs$' "$H/neu.ffpfs.sha256" \
    && ok "neu.ffpfs.sha256 im Format von sha256sum, Summe stimmt" || bad "Datei" "$(cat "$H/neu.ffpfs.sha256" 2>&1)"
[ ! -e "$H/neu.ffpfs.sha256.tmp" ] && ok "keine Zwischendatei übrig" || bad "tmp übrig"
library
[ "$(post "{\"keys\":\"$KN\"}" game/verify)" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && ok "danach besteht die Prüfung gegen die neue Datei" || bad "Prüfung" "$(cat "$W/st")"
[ "$(post "{\"keys\":\"$KN\"}" game/checksum)" = 202 ] && wait_job && [ "$(q 'd["result"]["have"]' <"$W/st")" = 1 ] && ok "erneut: nichts überschrieben (schon vorhanden)" || bad "erneut" "$(cat "$W/st")"

# ---- Spiel-Einzelheiten
[ "$(post "{\"key\":\"$KO\"}" game/info)" = 200 ] && [ "$(q 'd["version"], d["id"], d["entries"][0]["name"], d["size"] >= 701000' <"$W/body")" = "('01.002.000', 'PPSA00009', 'gross.bin', True)" ] \
    && ok "Einzelheiten Ordner-Spiel: Version, Titel-ID, größter Eintrag zuerst, Gesamtgröße" || bad "info Ordner" "$(cat "$W/body")"
[ "$(post "{\"key\":\"$KN\"}" game/info)" = 200 ] && [ "$(q 'd["ext"], d["checksum"], d["size"]' <"$W/body")" = "('ffpfs', True, 2000000)" ] \
    && ok "Einzelheiten Abbild: Endung, Prüfsumme vorhanden, Größe" || bad "info Abbild" "$(cat "$W/body")"
[ "$(post '{"key":"zzzz"}' game/info)" = 400 ] && [ "$(post '{"key":"deadbeef"}' game/info)" = 404 ] && ok "Einzelheiten: ungültiger und unbekannter Schlüssel abgelehnt" || bad "info Fehler"

# ---- Teilkopien
[ "$(q 'len(d["partials"])' <"$W/lib")" = 0 ] && ok "ohne Teilkopie: leere Liste" || bad "partials vorab" "$(q 'd["partials"]' <"$W/lib")"
mkdir -p "$W/mnt/usb1/.usbfmt-move/deadbeef/sce_sys"; head -c 300000 /dev/zero >"$W/mnt/usb1/.usbfmt-move/deadbeef/teil.bin"
library
[ "$(q '[(p["device"], p["bytes"]) for p in d["partials"]]' <"$W/lib")" = "[('da1', 300000)]" ] && ok "Teilkopie auf da1 gemeldet (300000 Bytes)" || bad "partials" "$(q 'd["partials"]' <"$W/lib")"
[ "$(post '{"device":"da0"}' partials/discard)" = 404 ] && ok "Laufwerk ohne Teilkopie: 404" || bad "discard da0"
[ "$(post '{"device":"da1"}' partials/discard)" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && [ ! -e "$W/mnt/usb1/.usbfmt-move" ] && ok "Teilkopie verworfen: Ordner weg, Auftrag ok" || bad "discard" "$(cat "$W/st") $(ls -a "$W/mnt/usb1")"
library
[ "$(q 'len(d["partials"])' <"$W/lib")" = 0 ] && ok "danach keine Teilkopie mehr gemeldet" || bad "partials danach"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Teilkopie-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
