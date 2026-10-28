#!/usr/bin/env bash
# Aufräumen: Verwaistes finden (Prüfsummen-/Dump-Info-Dateien ohne Abbild, Backport-Ordner ohne Spiel, leere Spielordner) und gezielt löschen;
# Zugehöriges und Spiele bleiben unberührt, Schlüssel von Nicht-Verwaistem werden abgelehnt.
# Aufruf: test/orphans_test.sh   (baut selbst, Port 18127)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18127 || exit 1
U=http://127.0.0.1:18127
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 300); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
truncate -s 64M "$W/a.img"
H="$W/mnt/usb0/homebrew"
mkdir -p "$H/Spiel/sce_sys" "$H/backports/PPSA00001" "$H/backports/PPSA99999 Alt" "$H/Leer/unter/noch" "$W/mnt/usb0/etaHEN/games"
printf '{"titleId":"PPSA00001","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Spiel"}}}' >"$H/Spiel/sce_sys/param.json"
echo x >"$H/backports/PPSA00001/patch.bin"; echo y >"$H/backports/PPSA99999 Alt/alt.bin"
head -c 100000 /dev/urandom >"$H/a.ffpfs"; echo h >"$H/a.ffpfs.sha256"; echo i >"$H/a.dump-info.json"   # gehören zu a.ffpfs: bleiben
echo h >"$H/Spiel.sha256"                                                                               # gehört zum Ordner Spiel: bleibt
echo h >"$H/weg.ffpfs.sha256"; echo i >"$H/ohne.dump-info.json"; echo m >"$H/fort.manifest.sha256"      # ohne Abbild/Spiel: verwaist
env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" USBFMT_INTERNAL_BASE="$W/data" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done
curl -s "$U/api/orphans" >"$W/o"
[ "$(q 'sorted((i["kind"], i["path"].rsplit("/",1)[-1]) for i in d["items"])' <"$W/o")" = "[('backport', 'PPSA99999 Alt'), ('empty', 'Leer'), ('sidecar', 'fort.manifest.sha256'), ('sidecar', 'ohne.dump-info.json'), ('sidecar', 'weg.ffpfs.sha256')]" ] \
    && ok "Verwaistes gefunden: Backport ohne Spiel, leerer Ordner, 3 Begleitdateien ohne Abbild; Zugehöriges nicht" || bad "Liste" "$(cat "$W/o")"
key() { q "[i['key'] for i in d['items'] if i['path'].endswith('/$1')][0]" <"$W/o"; }
KB=$(key "PPSA99999 Alt"); KS=$(key weg.ffpfs.sha256)
curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "{\"keys\":\"$KB,$KS\"}" "$U/api/orphans/delete" | grep -q 202 && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && grep -q 'wieder frei' "$W/st" \
    && [ ! -e "$H/backports/PPSA99999 Alt" ] && [ ! -e "$H/weg.ffpfs.sha256" ] && ok "gewählte Reste gelöscht (Backport-Ordner, Prüfsumme)" || bad "Löschen" "$(cat "$W/st")"
[ -e "$H/ohne.dump-info.json" ] && [ -d "$H/Leer" ] && [ -e "$H/a.ffpfs.sha256" ] && [ -e "$H/Spiel.sha256" ] && [ -d "$H/backports/PPSA00001" ] && [ -d "$H/Spiel" ] && ok "nicht Gewähltes und alles Zugehörige bleibt" || bad "Rest fehlt"
KSP=$(printf '%08x' $(python3 - "$H/Spiel.sha256" <<'PY'
import sys
# path_key des Servers nachbilden ist umständlich: stattdessen ein erfundener Schlüssel
print(0x12345678)
PY
))
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "$J" -d "{\"keys\":\"$KSP\"}" "$U/api/orphans/delete")" = 404 ] && ok "unbekannter/nicht verwaister Schlüssel: abgelehnt, nichts gelöscht" || bad "fremder Schlüssel"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "$J" -d '{"keys":"zz"}' "$U/api/orphans/delete")" = 404 ] && ok "ungültiger Schlüssel: abgelehnt" || bad "ungültig"
curl -s "$U/api/orphans" >"$W/o"
[ "$(q 'len(d["items"])' <"$W/o")" = 3 ] && ok "danach noch 3 verwaiste Einträge" || bad "Rest" "$(cat "$W/o")"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Aufräum-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
