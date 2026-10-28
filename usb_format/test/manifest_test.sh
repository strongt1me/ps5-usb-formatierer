#!/usr/bin/env bash
# Ordner-Spiele prüfen: Prüfsummenliste <Ordner>.manifest.sha256 neben dem Ordner anlegen (Format von sha256sum -c), später gegen den Inhalt prüfen
# (geändert / fehlt / neu), sie wird mit dem Spiel kopiert und gehört beim Aufräumen zum Spiel.
# Aufruf: test/manifest_test.sh   (baut selbst, Port 18128)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18128 || exit 1
U=http://127.0.0.1:18128
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
G="$H/Mein Spiel"
mkdir -p "$G/sce_sys" "$G/data/unter" "$W/mnt/usb1"
printf '{"titleId":"PPSA00033","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Mein Spiel"}}}' >"$G/sce_sys/param.json"
head -c 300000 /dev/urandom >"$G/eboot.bin"; head -c 200000 /dev/urandom >"$G/data/a.pak"; echo "mit Leerzeichen" >"$G/data/unter/zwei worte.txt"
env USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
library; K=$(key "Mein Spiel")

[ "$(post "{\"keys\":\"$K\"}" game/verify)" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'Keine Pr' "$W/st" && ok "Ordner ohne Liste: Hinweis, nichts verändert" || bad "ohne Liste" "$(cat "$W/st")"
[ "$(post "{\"keys\":\"$K\"}" game/checksum)" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && [ -f "$H/Mein Spiel.manifest.sha256" ] && ok "Prüfsumme anlegen: Liste neben dem Ordner" || bad "anlegen" "$(cat "$W/st")"
[ ! -e "$H/Mein Spiel.manifest.sha256.tmp" ] && [ ! -e "$G/Mein Spiel.manifest.sha256" ] && ok "keine Zwischendatei, nichts im Spielordner angelegt" || bad "tmp/Ort"
( cd "$G" && sha256sum -c "$H/Mein Spiel.manifest.sha256" >/dev/null 2>&1 ) && [ "$(wc -l <"$H/Mein Spiel.manifest.sha256")" = 4 ] && ok "Liste gilt für sha256sum -c (4 Dateien, auch mit Leerzeichen im Namen)" || bad "sha256sum -c" "$(cat "$H/Mein Spiel.manifest.sha256")"
sort -c -k2 "$H/Mein Spiel.manifest.sha256" 2>/dev/null; awk '{print $2}' "$H/Mein Spiel.manifest.sha256" | head -1 | grep -q '^data' && ok "Reihenfolge fest (sortiert nach Pfad)" || bad "Reihenfolge"
library; K=$(key "Mein Spiel")
[ "$(post "{\"keys\":\"$K\"}" game/verify)" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && grep -q 'stimmt' "$W/st" && ok "Prüfen: Ordner unverändert -> stimmt" || bad "stimmt" "$(cat "$W/st")"
[ "$(post "{\"keys\":\"$K\"}" game/checksum)" = 202 ] && wait_job && grep -q 'Schon vorhanden' "$W/st" && ok "erneut anlegen: Liste bleibt unberührt" || bad "erneut" "$(cat "$W/st")"
echo X >>"$G/data/a.pak"; rm "$G/data/unter/zwei worte.txt"; echo neu >"$G/neue-datei.bin"
[ "$(post "{\"keys\":\"$K\"}" game/verify)" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q '1 ge' "$W/st" && grep -q '1 fehlen' "$W/st" && grep -q '1 neu' "$W/st" \
    && ok "Änderungen erkannt: 1 geändert, 1 fehlt, 1 neu" || bad "Abweichung" "$(cat "$W/st")"
info=$(curl -s -H "$J" -d "{\"key\":\"$K\"}" "$U/api/game/info")
[ "$(echo "$info" | q 'd["checksum"]')" = True ] && ok "Einzelheiten melden die Prüfsummenliste" || bad "info" "$info"
curl -s "$U/api/library" >"$W/lib"
[ "$(post "{\"keys\":\"$K\",\"device\":\"da1\",\"verify\":true,\"extras\":true}" move)" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && [ -f "$W/mnt/usb1/homebrew/Mein Spiel.manifest.sha256" ] \
    && ok "Kopieren mit Zugehörigem: Liste liegt mit am Ziel" || bad "kopieren" "$(cat "$W/st") $(ls -R "$W/mnt/usb1" | head -20)"
curl -s "$U/api/orphans" >"$W/o"
[ "$(q 'len(d["items"])' <"$W/o")" = 0 ] && ok "Aufräumen: die Liste gehört zum Spiel, gilt nicht als verwaist" || bad "verwaist" "$(cat "$W/o")"
rm -rf "$G"; curl -s "$U/api/orphans" >"$W/o"
[ "$(q '[i["path"].rsplit("/",1)[-1] for i in d["items"]]' <"$W/o")" = "['Mein Spiel.manifest.sha256']" ] && ok "ohne den Ordner ist die Liste verwaist" || bad "verwaist nach Löschen" "$(cat "$W/o")"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Ordner-Prüfsummen-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
