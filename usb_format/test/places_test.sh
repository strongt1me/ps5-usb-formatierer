#!/usr/bin/env bash
# Spiele an festen Orten (intern: /data/homebrew, /data/etaHEN/games; extN) kopieren, verschieben und löschen, samt
# zugehörigen Dateien: Backport-Ordner backports/<Titel-ID>[ Zusatz] wie bei ShadowMountPlus (nur dieser) und Begleitdateien (<Name>.sha256, <Name>.dump-info.json).
# Aufruf: test/places_test.sh   (baut selbst, Port 18119; ShadowMountPlus-Attrappe test/smp_mock.py)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID= MPID=
trap 'kill $PID $MPID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18119 || exit 1

U=http://127.0.0.1:18119
J='Content-Type: application/json'
SMP_PORT=18120
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
post() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$1" "$U/api/$2"; }
wait_job() { for _ in $(seq 1 600); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
game() {
    mkdir -p "$1/sce_sys"
    printf '{"titleId":"%s","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"%s"}}}' "$2" "$2" >"$1/sce_sys/param.json"
    head -c 50000 /dev/urandom >"$1/data.bin"
}
key() { q "[t['key'] for t in d['titles'] if t['path'] == '$1'][0]" <"$W/lib"; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
start() {
    [ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }
    env USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_INTERNAL="$W/data/homebrew" \
        USBFMT_INTERNAL_BASE="$W/data" USBFMT_DATA="$W/cfg" USBFMT_SMP_PORT=$SMP_PORT "$@" "$W/srv" >>"$W/log" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && return; sleep 0.1; done
}

truncate -s 64M "$W/a.img" "$W/b.img"
mkdir -p "$W/mnt/usb1"
H="$W/mnt/usb0/homebrew"
IH="$W/data/homebrew"
mkdir -p "$H" "$IH"
game "$H/Astro" PPSA01325
echo summe >"$H/Astro.sha256"
echo info >"$H/Astro.dump-info.json"
game "$IH/Intern" PPSA05555
game "$IH/Fremd" PPSA07777
mkdir -p "$IH/backports/PPSA05555" "$IH/backports/PPSA07777" "$H/backports/PPSA01325 Astro Name"
echo patch >"$IH/backports/PPSA05555/a.bin"
echo fremd >"$IH/backports/PPSA07777/c.bin"
echo patch2 >"$H/backports/PPSA01325 Astro Name/b.bin"
game "$IH/PPSA86920" PPSA86920
game "$H/D1" PPSA09090
game "$H/D2" PPSA09090   # zweite Kopie derselben Titel-ID, teilt sich den Backport-Ordner
mkdir -p "$H/backports/PPSA09090"; echo b >"$H/backports/PPSA09090/x.bin"
cat >"$W/games.json" <<EOT
[{"title_id":"PPSA01325","path":"$H/Astro"},{"title_id":"PPSA05555","path":"$IH/Intern"},
 {"title_id":"PPSA07777","path":"$IH/Fremd"},{"title_id":"PPSA86920","path":"$IH/PPSA86920"},
 {"title_id":"PPSA09090","path":"$H/D1"},{"title_id":"PPSA09292","path":"$H/D3verwaltet"}]
EOT
python3 test/smp_mock.py $SMP_PORT "$W/games.json" "$W/smp.log" &
MPID=$!
sleep 0.5

start
library
DEV2=$(q '[r["device"] for r in d["roots"] if not r["place"]][1]' <"$W/lib")
[ "$(q '[r["device"] for r in d["roots"] if r["place"]]' <"$W/lib")" = "['intern']" ] && ok "interner Ort als Wurzel intern (place)" || bad "place" "$(q 'd["roots"]' <"$W/lib")"
[ "$(q '[t["device"] for t in d["titles"] if t["path"] == "'"$IH/Intern"'"][0]' <"$W/lib")" = intern ] && ok "internes Spiel trägt den Ort intern" || bad "Ort des Titels"
[ "$(q 'sorted(x.split("/")[-1] for x in [t for t in d["titles"] if t["path"] == "'"$IH/Intern"'"][0]["extras"])' <"$W/lib")" = "['PPSA05555']" ] \
    && ok "zugehörig: nur der Backport-Ordner mit der eigenen Titel-ID (backports/)" || bad "extras intern" "$(head -c 1500 "$W/lib")"
[ "$(q 'sorted(x.split("/")[-1] for x in [t for t in d["titles"] if t["path"] == "'"$H/Astro"'"][0]["extras"])' <"$W/lib")" = "['Astro.dump-info.json', 'Astro.sha256', 'PPSA01325 Astro Name']" ] \
    && ok "zugehörig: Prüfsumme, Dump-Info und Backport-Ordner des USB-Spiels" || bad "extras usb" "$(head -c 1500 "$W/lib")"

# ---------------------------------------------------------------- USB -> intern (mit zugehörigen Dateien, geprüft)
KA=$(key "$H/Astro")
code=$(post "{\"keys\":\"$KA\",\"device\":\"intern\",\"verify\":true,\"extras\":true}" move)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"] and d["result"]["copied"] == 1' <"$W/st")" = True ] && diff -r "$H/Astro" "$IH/Astro" >/dev/null \
    && ok "USB -> intern: Spiel liegt in /data/homebrew/Astro" || bad "USB -> intern" "$code $(cat "$W/body" "$W/st")"
[ -f "$IH/Astro.sha256" ] && [ -f "$IH/Astro.dump-info.json" ] && ok "Begleitdateien neben dem Spiel mitkopiert" || bad "Begleitdateien" "$(ls "$IH")"
[ -f "$IH/backports/PPSA01325 Astro Name/b.bin" ] && [ ! -e "$IH/backports/PPSA07777/b.bin" ] && ok "Backport-Ordner des Spiels mitkopiert, andere unberührt" || bad "Backport kopiert" "$(find "$IH/backport")"
[ -d "$H/Astro" ] && [ -f "$H/Astro.sha256" ] && ok "Originale bleiben" || bad "Originale"

# ---------------------------------------------------------------- intern -> USB ohne zugehörige Dateien
library
KI=$(key "$IH/Intern")
code=$(post "{\"keys\":\"$KI\",\"device\":\"$DEV2\",\"verify\":false,\"extras\":false}" move)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && diff -r "$IH/Intern" "$W/mnt/usb1/homebrew/Intern" >/dev/null && [ ! -e "$W/mnt/usb1/homebrew/backport" ]     && ok "intern -> USB: Spiel kopiert, ohne Haken keine zugehörigen Dateien" || bad "intern -> USB" "$code $(cat "$W/body" "$W/st"; find "$W/mnt/usb1")"
code=$(post "{\"keys\":\"$KI\",\"device\":\"intern\"}" move)
[ "$code" = 404 ] && ok "Quelle und Ziel gleich (intern -> intern) abgelehnt" || bad "intern -> intern" "$code $(cat "$W/body")"
rm -rf "$W/mnt/usb1/homebrew"

# ---------------------------------------------------------------- Löschen mit und ohne zugehörige Dateien
library
code=$(post "{\"keys\":\"$(key "$IH/Fremd")\",\"uninstall\":false,\"extras\":false}" delete)
[ "$code" = 202 ] && wait_job && [ ! -e "$IH/Fremd" ] && [ -d "$IH/backports/PPSA07777" ] \
    && ok "Löschen ohne Haken: Spiel weg, Backport-Ordner bleibt" || bad "Löschen ohne extras" "$code $(cat "$W/st")"
library
code=$(post "{\"keys\":\"$(key "$IH/Intern")\",\"extras\":true}" delete)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"] and d["result"]["extras"] == 1' <"$W/st")" = True ] && [ ! -e "$IH/Intern" ] && [ ! -e "$IH/backports/PPSA05555" ] \
    && [ -d "$IH/backports/PPSA07777" ] && ok "Löschen mit Haken: Spiel und eigener Backport-Ordner weg, fremde bleiben" || bad "Löschen mit extras" "$code $(cat "$W/st"; find "$IH/backport")"
grep -q 'mitgelöscht' "$W/st" && ok "Meldung nennt die mitgelöschten Dateien" || bad "Meldung" "$(cat "$W/st")"
library
code=$(post "{\"keys\":\"$(key "$H/Astro")\",\"extras\":true}" delete)
[ "$code" = 202 ] && wait_job && [ ! -e "$H/Astro" ] && [ ! -e "$H/Astro.sha256" ] && [ ! -e "$H/Astro.dump-info.json" ] && [ -d "$IH/Astro" ] \
    && ok "USB-Spiel: Spiel und Begleitdateien weg, die Kopie intern bleibt" || bad "USB löschen" "$code $(cat "$W/st"; ls "$H")"

# ---------------------------------------------------------------- Geteiltes bleibt stehen, doppelte Schlüssel
library
KD2=$(key "$H/D2")
code=$(post "{\"keys\":\"$KD2\",\"extras\":true}" delete)
[ "$code" = 202 ] && wait_job && [ ! -e "$H/D2" ] && [ -d "$H/backports/PPSA09090" ]     && ok "zweite Kopie gelöscht: der gemeinsame Backport-Ordner der bleibenden Kopie bleibt" || bad "geteilter Backport" "$code $(cat "$W/st"; ls "$H/backports")"
library
KD1=$(key "$H/D1")
code=$(post "{\"keys\":\"$KD1\",\"extras\":true}" delete)
[ "$code" = 202 ] && wait_job && [ ! -e "$H/D1" ] && [ ! -e "$H/backports/PPSA09090" ]     && ok "letzte Kopie gelöscht: der Backport-Ordner geht mit" || bad "letzter Backport" "$code $(cat "$W/st"; ls "$H/backports")"
game "$H/E1" PPSA09191
library
KE=$(key "$H/E1")
code=$(post "{\"keys\":\"$KE,$KE\",\"device\":\"intern\",\"verify\":true}" move)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"] and d["result"]["copied"] == 1 and d["result"]["failed"] == 0' <"$W/st")" = True ]     && ok "doppelter Schlüssel: nur einmal kopiert, kein Fehler" || bad "doppelter Schlüssel" "$code $(cat "$W/st")"
code=$(post "{\"keys\":\"$KE\",\"device\":\"ext9\"}" move)
[ "$code" = 404 ] && ok "Ziel extN ohne eingehängtes Laufwerk abgelehnt" || bad "ext9" "$code $(cat "$W/body")"

# ---------------------------------------------------------------- teilweise gelöscht wird gemeldet
game "$H/D3" PPSA09292   # zweite Kopie (ShadowMountPlus führt die ID woanders): wird selbst gelöscht
mkdir -p "$H/D3/gesperrt"; echo x >"$H/D3/gesperrt/f"; chmod 555 "$H/D3/gesperrt"
library
code=$(post "{\"keys\":\"$(key "$H/D3")\"}" delete)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'teilweise gelöscht' "$W/st" && [ -d "$H/D3" ]     && ok "Löschen bricht mittendrin ab: Meldung nennt \"nur teilweise gelöscht\"" || bad "teilweise gelöscht" "$code $(cat "$W/st")"
chmod 755 "$H/D3/gesperrt"

# ---------------------------------------------------------------- Pfad ohne Schrägstrich darf den Server nicht abstürzen lassen
code=$(post '{"paths":"a.zip"}' saves/restore)
[ "$code" = 400 ] && curl -s "$U/api/info" >/dev/null && ok "Sicherung ohne Schrägstrich im Pfad: abgelehnt, Server lebt" || bad "Pfad ohne Schrägstrich" "$code"
code=$(post '{"key":"x","mtime":"1700000000","key_mtime":""}' saves/times)
[ "$code" = 400 ] && ok "key_mtime leer: abgelehnt" || bad "key_mtime leer" "$code"

# ---------------------------------------------------------------- der Formatierer selbst
library
code=$(post "{\"keys\":\"$(key "$IH/PPSA86920")\"}" delete)
[ "$code" = 409 ] && [ -d "$IH/PPSA86920" ] && ok "Formatierer (PPSA86920) lässt sich nicht selbst löschen" || bad "Selbstschutz" "$code $(cat "$W/body")"

[ "$fails" = 0 ] && echo "Alle Orte-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
