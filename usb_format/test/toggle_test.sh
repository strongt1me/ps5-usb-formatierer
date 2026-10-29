#!/usr/bin/env bash
# Spiele deaktivieren/aktivieren (POST /api/game/toggle): ".off" an das Abbild hängen bzw. entfernen. Geprüft: nur
# umbenannt (Rechte, Inode und Zeiten unverändert), Bibliothek zeigt "off", deaktivierte zählen nicht als doppelt,
# Prüfsumme und Aufräumen behandeln die Dateien daneben richtig, Ordner abgelehnt, laufendes Spiel nicht deaktiviert,
# vorhandenes Ziel nie überschrieben, nach dem Aktivieren sucht ShadowMountPlus neu.
# Aufruf: test/toggle_test.sh   (baut selbst, Ports 18170/18171)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID= MPID=
trap 'kill $PID $MPID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18170 || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c || exit 1
U=http://127.0.0.1:18170
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 300); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.1; done; return 1; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
key() { q "[t['key'] for t in d['titles'] if t['path'].endswith('/$1')][0]" <"$W/lib"; }
toggle() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "{\"keys\":\"$1\"}" "$U/api/game/toggle"; }
info() { stat -c '%a %i %Y %s' "$1"; }

truncate -s 64M "$W/a.img"
H="$W/mnt/usb0/homebrew"
mkdir -p "$H/Ordner/sce_sys" "$W/sandbox" "$W/game"
printf '{"titleId":"PPSA00021"}' >"$H/Ordner/sce_sys/param.json"
printf '{"titleId":"PPSA00022","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Abbild"}}}' >"$W/game/param.json"
"$W/mkfs_test" part "$H/Spiel.exfat" exfat $((16 * 1048576)) 512 "" 4096 --file=sce_sys/param.json="$W/game/param.json" >/dev/null
cp "$H/Spiel.exfat" "$H/Kopie.exfat"                 # gleiche Titel-ID: doppelt
head -c 300000 /dev/urandom >"$H/pfs.ffpfs"
(cd "$H" && sha256sum pfs.ffpfs >pfs.ffpfs.sha256)    # Prüfsumme daneben, nach dem aktiven Namen
head -c 1000 /dev/urandom >"$H/y.ffpkg"
head -c 1000 /dev/urandom >"$H/y.ffpkg.off"          # beide Namen gibt es schon
chmod 640 "$H/Spiel.exfat"
chmod 604 "$H/pfs.ffpfs"
touch -d "2020-01-02 03:04:05" "$H/Spiel.exfat" "$H/pfs.ffpfs"
before_s=$(info "$H/Spiel.exfat")
before_p=$(info "$H/pfs.ffpfs")
echo '[]' >"$W/games.json"
python3 test/smp_mock.py 18171 "$W/games.json" "$W/smp.log" &
MPID=$!
env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" USBFMT_SANDBOX="$W/sandbox" \
    USBFMT_SMP_PORT=18171 "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
library
[ "$(q "len([x for x in d['duplicates'] if x['id'] == 'PPSA00022'])" <"$W/lib")" = 1 ] && ok "vorher: Spiel.exfat und Kopie.exfat doppelt" || bad "doppelt vorher"

[ "$(toggle "$(key Ordner)")" = 400 ] && grep -q "nur bei Abbildern" "$W/body" && ok "Ordner-Spiel: abgelehnt" || bad "Ordner" "$(cat "$W/body")"

# deaktivieren
[ "$(toggle "$(key Spiel.exfat),$(key pfs.ffpfs)")" = 202 ] && wait_job && grep -q '"ok":true' "$W/st" && grep -q '2 Spiele deaktiviert' "$W/st" \
    && ok "zwei Abbilder deaktiviert" || bad "deaktivieren" "$(cat "$W/st" "$W/body")"
[ -f "$H/Spiel.exfat.off" ] && [ -f "$H/pfs.ffpfs.off" ] && [ ! -e "$H/Spiel.exfat" ] && [ ! -e "$H/pfs.ffpfs" ] \
    && ok "umbenannt: .off angehängt" || bad "Dateinamen" "$(ls "$H")"
[ "$(info "$H/Spiel.exfat.off")" = "$before_s" ] && [ "$(info "$H/pfs.ffpfs.off")" = "$before_p" ] \
    && ok "Rechte, Inode, Änderungszeit und Größe unverändert ($before_s / $before_p)" || bad "Dateiattribute" "$(info "$H/Spiel.exfat.off") / $(info "$H/pfs.ffpfs.off")"
[ -f "$H/pfs.ffpfs.sha256" ] && ok "Prüfsumme daneben bleibt unter dem aktiven Namen" || bad "Prüfsumme verschoben"
library
[ "$(q "[(t['off'] if 'off' in t else False, t['id'], t['kind'], t['ext']) for t in d['titles'] if t['path'].endswith('/Spiel.exfat.off')][0]" <"$W/lib")" = "(True, 'PPSA00022', 'exFAT', 'exfat')" ] \
    && ok "Bibliothek: deaktiviert, Titel-ID, Art und Endung wie vorher" || bad "Bibliothek" "$(grep -o '[^{]*Spiel.exfat.off[^}]*' "$W/lib")"
[ "$(q "len([x for x in d['duplicates'] if x['id'] == 'PPSA00022'])" <"$W/lib")" = 0 ] && ok "deaktivierte Kopie zählt nicht als doppelt" || bad "doppelt nachher"

# Prüfen und Aufräumen am deaktivierten Spiel
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "$J" -d "{\"keys\":\"$(key pfs.ffpfs.off)\"}" "$U/api/game/verify")" = 202 ] && wait_job \
    && grep -q 'stimmt' "$W/st" && ok "Spiel prüfen: deaktiviertes Abbild mit Prüfsumme unter dem aktiven Namen" || bad "prüfen" "$(cat "$W/st")"
curl -s "$U/api/orphans" | grep -q 'pfs.ffpfs.sha256' && bad "Aufräumen hält die Prüfsumme für verwaist" || ok "Aufräumen: Prüfsumme gehört zum deaktivierten Abbild"

# vorhandenes Ziel: nichts überschreiben
[ "$(toggle "$(key y.ffpkg.off)")" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'Ziel gibt es schon' "$W/st" \
    && [ "$(stat -c %s "$H/y.ffpkg")" = 1000 ] && ok "Ziel vorhanden: abgelehnt, nichts überschrieben" || bad "Ziel vorhanden" "$(cat "$W/st")"

# laufendes Spiel nicht deaktivieren
library
mkdir -p "$W/sandbox/PPSA00022_000"
[ "$(toggle "$(key Kopie.exfat)")" = 202 ] && wait_job && grep -q 'läuft gerade' "$W/st" && [ -f "$H/Kopie.exfat" ] \
    && ok "laufendes Spiel: nicht deaktiviert" || bad "laufendes Spiel" "$(cat "$W/st")"
rmdir "$W/sandbox/PPSA00022_000"

# wieder aktivieren
: >"$W/smp.log"
[ "$(toggle "$(key Spiel.exfat.off),$(key pfs.ffpfs.off)")" = 202 ] && wait_job && grep -q '2 Spiele wieder aktiviert' "$W/st" \
    && [ -f "$H/Spiel.exfat" ] && [ -f "$H/pfs.ffpfs" ] && [ "$(info "$H/Spiel.exfat")" = "$before_s" ] && [ "$(info "$H/pfs.ffpfs")" = "$before_p" ] \
    && ok "wieder aktiviert: .off entfernt, Attribute unverändert" || bad "aktivieren" "$(cat "$W/st"; ls "$H")"
grep -q "scan reset_attempts=true" "$W/smp.log" && ok "nach dem Aktivieren: ShadowMountPlus sucht neu" || bad "kein Suchlauf"
curl -s "$U/api/history" | q 'd["entries"][0]["kind"]' | grep -qx game_toggle && ok "im Verlauf" || bad "Verlauf"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Umschalt-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
