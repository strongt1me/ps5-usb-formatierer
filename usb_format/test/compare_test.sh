#!/usr/bin/env bash
# Zwei Kopien desselben Spiels vergleichen (Ordner, Abbilder, Abbild gegen Ordner), fehlende Pflichtdateien bei Ordner-Spielen,
# Lesetempo und Verschleiß-Hinweis im Verlauf. Das UFS2-Abbild (makefs) mit kleinen Blöcken: große Dateien brauchen
# einfach und zweifach indirekte Blöcke.
# Aufruf: test/compare_test.sh   (baut selbst, Port 18131)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18131 || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c || exit 1
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
head -c 9000000 /dev/urandom >"$H/Eins/data/gross.bin"   # UFS2 mit 4-KiB-Blöcken: über 2 MiB zweifach indirekt
mkdir -p "$H/Kopie" && cp -a "$H/Eins/." "$H/Kopie/"
mkdir -p "$H/Anders" && cp -a "$H/Eins/." "$H/Anders/"
printf 'y' >"$H/Anders/data/b.txt"; rm "$H/Anders/data/a.bin"; printf 'neu' >"$H/Anders/data/c.txt"; printf 'neu' >"$H/Anders/d.txt"
# dasselbe Spiel als exFAT-Abbild (eigener Formatierer) und als UFS2-Abbild (makefs)
files=()
while IFS= read -r f; do files+=("--file=${f#"$H/Eins/"}=$f"); done < <(find "$H/Eins" -type f)
"$W/mkfs_test" part "$H/Eins.exfat" exfat $((64 * 1048576)) 512 "" 4096 "${files[@]}" >/dev/null || bad "exFAT-Abbild anlegen"
UFS=0
if command -v makefs >/dev/null; then
    makefs -t ffs -o version=2,bsize=4096,fsize=512 -s 32m "$H/Eins.ffpkg" "$H/Eins" >/dev/null && UFS=1 || bad "UFS2-Abbild anlegen"
else
    echo "HINWEIS makefs fehlt: UFS2-Abbild übersprungen"
fi
game Kaputt PPSA00012
rm "$H/Kaputt/eboot.bin" "$H/Kaputt/sce_sys/icon0.png"
game Zwei PPSA00013 && mv "$H/Zwei/eboot.bin" "$H/Zwei/eboot2.bin"   # eboot2.bin genügt (ShadowMountPlus 1.7)
env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
library
cmp() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "{\"keys\":\"$1\"}" "$U/api/game/compare"; }

[ "$(cmp "$(key Eins),$(key Kopie)")" = 202 ] && wait_job && [ "$(q 'd["result"]["same"], d["ok"]' <"$W/st")" = "(True, True)" ] && grep -q 'Inhalt gleich' "$W/st" \
    && ok "gleiche Ordner-Kopien: als Byte für Byte gleich erkannt" || bad "gleiche Kopien" "$(cat "$W/st" "$W/body")"
[ "$(cmp "$(key Eins),$(key Anders)")" = 202 ] && wait_job \
    && [ "$(q 'd["result"]["same"], d["result"]["changed"], d["result"]["only_first"], d["result"]["only_second"]' <"$W/st")" = "(False, 1, 1, 2)" ] \
    && ok "verschiedene Kopien: 1 geändert, 1 nur in der ersten, 2 nur in der zweiten" || bad "verschiedene Kopien" "$(cat "$W/st")"
RES='d["result"]["same"], d["result"]["changed"], d["result"]["only_first"], d["result"]["only_second"]'
[ "$(cmp "$(key Eins),$(key Eins.exfat)")" = 202 ] && wait_job && [ "$(q "$RES" <"$W/st")" = "(True, 0, 0, 0)" ] && grep -q 'Abbild und Ordner' "$W/st" \
    && ok "Ordner gegen exFAT-Abbild mit denselben Dateien: gleich" || bad "Ordner/exFAT-Abbild" "$(cat "$W/st" "$W/body")"
[ "$(cmp "$(key Eins.exfat),$(key Anders)")" = 202 ] && wait_job && [ "$(q "$RES" <"$W/st")" = "(False, 1, 1, 2)" ] \
    && ok "exFAT-Abbild gegen geänderten Ordner: 1 geändert, 1 nur im Abbild, 2 nur im Ordner" || bad "exFAT-Abbild/Anders" "$(cat "$W/st")"
[ "$(cmp "$(key Anders),$(key Eins.exfat)")" = 202 ] && wait_job && [ "$(q "$RES" <"$W/st")" = "(False, 1, 2, 1)" ] \
    && ok "umgekehrte Reihenfolge: erste und zweite Kopie richtig zugeordnet" || bad "Anders/exFAT-Abbild" "$(cat "$W/st")"
if [ "$UFS" = 1 ]; then
    [ "$(cmp "$(key Eins.ffpkg),$(key Kopie)")" = 202 ] && wait_job && [ "$(q "$RES" <"$W/st")" = "(True, 0, 0, 0)" ] \
        && ok "UFS2-Abbild (indirekte Blöcke) gegen Ordner: gleich" || bad "UFS2-Abbild/Kopie" "$(cat "$W/st" "$W/body")"
    [ "$(cmp "$(key Eins.ffpkg),$(key Anders)")" = 202 ] && wait_job && [ "$(q "$RES" <"$W/st")" = "(False, 1, 1, 2)" ] \
        && ok "UFS2-Abbild gegen geänderten Ordner: Abweichungen erkannt" || bad "UFS2-Abbild/Anders" "$(cat "$W/st")"
fi
[ ! -e "$W/cfg/compare.manifest.tmp" ] && [ ! -e "$H/Eins.manifest.sha256" ] && ok "keine Reste: weder Zwischenliste noch Prüfsummenliste angelegt" || bad "Reste" "$(ls -a "$W/cfg" "$H")"
[ "$(cmp "$(key Eins),$(key Kaputt)")" = 400 ] && ok "verschiedene Spiele: abgelehnt" || bad "verschiedene Spiele" "$(cat "$W/body")"
[ "$(cmp "$(key Eins)")" = 400 ] && ok "nur ein Spiel: abgelehnt" || bad "ein Spiel"
[ "$(cmp "$(key Eins),$(key Eins)")" = 400 ] && ok "zweimal dasselbe Spiel: abgelehnt (nicht mit sich selbst vergleichen)" || bad "dasselbe Spiel" "$(cat "$W/body")"
[ "$(q "[t.get('missing','') for t in d['titles'] if t['path'].endswith('/Kaputt')][0]" <"$W/lib")" = "eboot.bin, sce_sys/icon0.png" ] \
    && [ "$(q "[t.get('missing','') for t in d['titles'] if t['path'].endswith('/Eins')][0]" <"$W/lib")" = "" ] \
    && [ "$(q "[t.get('missing','') for t in d['titles'] if t['path'].endswith('/Zwei')][0]" <"$W/lib")" = "" ] \
    && ok "fehlende Pflichtdateien gemeldet, vollständiges Spiel (auch mit eboot2.bin) ohne Meldung" || bad "fehlende Dateien" "$(grep -o '"missing":"[^"]*"' "$W/lib")"

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
grep -q 'langsamer' "$W/st" && bad "erste Messung: kein Verschleiß-Hinweis erwartet" "$(cat "$W/st")"
# früher viel schneller (zwei Einträge im Verlauf nachgetragen): die nächste Messung warnt vor Verschleiß
python3 - "$W/cfg/history.jsonl" <<'PY'
import json, sys
lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
e = json.loads(next(l for l in lines if '"kind":"speed"' in l))
e["mbps"] = 900000
open(sys.argv[1], "w", encoding="utf-8").write("\n".join([json.dumps(e, ensure_ascii=False, separators=(",", ":"))] * 2 + lines) + "\n")
PY
curl -s -o /dev/null -H "$J" -d "$(python3 -c 'import sys; n, s = eval(sys.argv[1]); print("{\"device\":\"%s\",\"size\":%d}" % (n, s))' "$DEV")" "$U/api/speed"
wait_job && grep -q 'langsamer als bei früheren Messungen (900000 MB/s)' "$W/st" && ok "Tempo deutlich unter früheren Messungen: Hinweis auf Verschleiß" || bad "Verschleiß-Hinweis" "$(cat "$W/st")"
curl -s "$U/api/history" | q '[e["kind"] for e in d["entries"]].count("game_compare") >= 4' | grep -q True && ok "Vergleiche im Verlauf" || bad "Verlauf"
kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle Vergleichs-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
