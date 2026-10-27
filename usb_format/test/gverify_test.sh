#!/usr/bin/env bash
# Spiele prüfen: Abbild gegen die Prüfsummen-Datei daneben (<Datei>.sha256 bzw. <Name>.sha256) vergleichen; Ordner-Spiele und fehlende Dateien werden gemeldet.
# Aufruf: test/gverify_test.sh   (baut selbst, Port 18123)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18123 || exit 1
U=http://127.0.0.1:18123
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 300); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
key() { q "[t['key'] for t in d['titles'] if t['path'].endswith('/$1')][0]" <"$W/lib"; }
truncate -s 64M "$W/a.img"
H="$W/mnt/usb0/homebrew"
mkdir -p "$H/Ordner/sce_sys"
printf '{"titleId":"PPSA00009","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Ordnerspiel"}}}' >"$H/Ordner/sce_sys/param.json"
head -c 3000000 /dev/urandom >"$H/gut.ffpfs";      sha256sum "$H/gut.ffpfs" | cut -d' ' -f1 >"$H/gut.ffpfs.sha256"
head -c 3000000 /dev/urandom >"$H/falsch.ffpfs";   sha256sum "$H/falsch.ffpfs" | sed 's/^./0/' >"$H/falsch.sha256"
head -c 3000000 /dev/urandom >"$H/ohne.ffpfs"
env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done
run() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "{\"keys\":\"$1\"}" "$U/api/game/verify"; wait_job; }
[ "$(run "$(key gut.ffpfs)")" = 202 ] && [ "$(q 'd["ok"]' <"$W/st")" = True ] && grep -q 'stimmt' "$W/st" && ok "passende Prüfsumme (<Datei>.sha256): in Ordnung" || bad "gut" "$(cat "$W/st")"
[ "$(run "$(key falsch.ffpfs)")" = 202 ] && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'Weicht ab' "$W/st" && grep -q 'falsch' "$W/st" && ok "<Name>.sha256 mit falscher Summe: Abweichung mit Namen gemeldet" || bad "falsch" "$(cat "$W/st")"
[ "$(run "$(key ohne.ffpfs),$(key Ordner)")" = 202 ] && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'Keine Pr' "$W/st" && ok "ohne Prüfsummen-Datei und Ordner-Spiel: gemeldet, nichts verändert" || bad "ohne" "$(cat "$W/st")"
[ "$(run "$(key gut.ffpfs),$(key falsch.ffpfs),$(key ohne.ffpfs)")" = 202 ] && [ "$(q 'd["result"]["ok"], d["result"]["bad"], d["result"]["none"]' <"$W/st")" = "(1, 1, 1)" ] && ok "gemischt: 1 stimmt, 1 weicht ab, 1 ohne Datei" || bad "gemischt" "$(cat "$W/st")"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "$J" -d '{"keys":"zzzz"}' "$U/api/game/verify")" = 404 ] && ok "ungültiger Schlüssel: abgelehnt" || bad "Schlüssel"
[ -e "$H/gut.ffpfs" ] && ok "Dateien unverändert" || bad "Datei fehlt"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Spiele-prüfen-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
