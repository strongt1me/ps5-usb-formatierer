#!/usr/bin/env bash
# Spiele verschieben (POST /api/move): Kopieren auf ein zweites Laufwerk mit Prüfsummen, Original bleibt, nichts Halbfertiges,
# Abbrechen räumt auf, Schutz vor Überschreiben / gleichem Laufwerk / laufendem Spiel.
# Aufruf: test/move_test.sh   (baut selbst, Port 18118)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18118 || exit 1

U=http://127.0.0.1:18118
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
post() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$1" "$U/api/$2"; }
wait_job() { for _ in $(seq 1 600); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
game() {   # game <ordner> <titel-id>
    mkdir -p "$1/sce_sys"
    printf '{"titleId":"%s","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"%s"}}}' "$2" "$2" >"$1/sce_sys/param.json"
}
key() { q "[t['key'] for t in d['titles'] if t['path'] == '$1'][0]" <"$W/lib"; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
start() {   # start [weitere Umgebung]
    [ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }
    env USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_INTERNAL="$W/data/homebrew" \
        USBFMT_DATA="$W/cfg" "$@" "$W/srv" >>"$W/log" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && return; sleep 0.1; done
}

truncate -s 64M "$W/a.img" "$W/b.img"
mkdir -p "$W/mnt/usb0" "$W/mnt/usb1" "$W/data/homebrew"
A="$W/mnt/usb0/homebrew"
B="$W/mnt/usb1"
game "$A/Astro" PPSA01325
head -c 3000000 /dev/urandom >"$A/Astro/data.bin"
mkdir -p "$A/Astro/sub/tief"
head -c 777 /dev/urandom >"$A/Astro/sub/x.bin"
: >"$A/Astro/sub/tief/leer.bin"
touch -d '2024-03-04 05:06:07' "$A/Astro/data.bin"
game "$A/Zwei" PPSA02222
head -c 100000 /dev/urandom >"$A/Zwei/y.bin"
game "$A/Gross" PPSA03333
head -c 150000000 /dev/urandom >"$A/Gross/riesig.bin"

start
library
DEV1=$(q '[t["device"] for t in d["titles"] if t["path"] == "'"$A/Astro"'"][0]' <"$W/lib")
DEV2=$(q '[r["device"] for r in d["roots"] if r["device"] != "'"$DEV1"'"][0]' <"$W/lib")
[ -n "$DEV1" ] && [ -n "$DEV2" ] && [ "$DEV1" != "$DEV2" ] && ok "zwei Laufwerke ($DEV1 -> $DEV2)" || bad "Laufwerke" "$DEV1 $DEV2 $(cat "$W/lib" | head -c 400)"
K=$(key "$A/Astro")

# Pakete: .pkg-Dateien im Stamm und in Unterordnern (ps4/<Titel>, PS5-Updates/<Titel>), Kopf wie bei der LibProsperoPkg gelesen:
# PS4 (CNT): Titel-ID aus der Content-ID, Update an den Inhalts-Flags; PS5 (FIH/LIH): Byte 0x05 0x80 = PKG, sonst fPKG
mkdir -p "$W/mnt/usb0/ps4/CUSA00001" "$W/mnt/usb0/PS5-Updates/CUSA00002"
python3 - "$W/mnt/usb0" <<'PYEOF'
import sys
r = sys.argv[1]
def cnt(path, cid, flags):
    h = bytearray(0x200)
    h[0:4] = b"CNT"; h[0x40:0x40 + len(cid)] = cid.encode(); h[0x78:0x7c] = flags.to_bytes(4, "big")
    open(path, "wb").write(h)
def fih(path, magic, signed):
    h = bytearray(0x200); h[0:4] = magic; h[5] = signed
    open(path, "wb").write(h)
cnt(r + "/ps4/CUSA00001/app.pkg", "EP0001-CUSA00001_00-TESTGAME00000000", 0x0A000000)
cnt(r + "/PS5-Updates/CUSA00002/update.pkg", "UP0002-CUSA00002_00-TESTGAME00000000", 0x42240000)
fih(r + "/UP1018-PPSA16833_00-LEGO-4xx.pkg", b"LIH", 0x00)
fih(r + "/offiziell_PPSA00003.pkg", b"FIH", 0x80)
open(r + "/fremd.pkg", "wb").write(b"XXXX" * 64)
PYEOF
library
[ "$(q 'sorted((p["name"], p["platform"], p["type"], p["title_id"], p["update"]) for p in d["pkgfiles"])' <"$W/lib")" = "[('UP1018-PPSA16833_00-LEGO-4xx.pkg', 'PS5', 'fPKG', 'PPSA16833', False), ('app.pkg', 'PS4', '', 'CUSA00001', False), ('fremd.pkg', '', '', '', False), ('offiziell_PPSA00003.pkg', 'PS5', 'PKG', 'PPSA00003', False), ('update.pkg', 'PS4', '', 'CUSA00002', True)]" ]     && ok "PKG-Liste: Plattform, PKG/fPKG (PS5 an der Signatur), Titel-ID, Update (PS4 an den Flags)" || bad "PKG-Liste" "$(q 'd["pkgfiles"]' <"$W/lib")"
[ "$(q 'all("ext" in t for t in d["titles"])' <"$W/lib")" = True ] && ok "Titel tragen das Feld ext" || bad "ext fehlt"

# ---------------------------------------------------------------- Kopieren mit Prüfen
code=$(post "{\"keys\":\"$K\",\"device\":\"$DEV2\",\"verify\":true}" move)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"] and d["kind"] == "move"' <"$W/st")" = True ] \
    && ok "Verschieben (kopieren + prüfen): Auftrag ok" || bad "Verschieben" "$code $(cat "$W/body" "$W/st")"
diff -r "$A/Astro" "$B/homebrew/Astro" >/dev/null && ok "Kopie am Ziel stimmt mit dem Original überein" || bad "Kopie weicht ab"
[ -f "$A/Astro/data.bin" ] && ok "Original bleibt erhalten" || bad "Original weg"
[ "$(stat -c %Y "$B/homebrew/Astro/data.bin")" = "$(stat -c %Y "$A/Astro/data.bin")" ] && ok "Änderungszeit der Dateien übernommen" || bad "Zeiten"
[ ! -e "$B/.usbfmt-move" ] && ok "kein Zwischenordner übrig" || bad "Zwischenordner" "$(ls -a "$B")"
[ "$(q 'd["result"]["keys"]' <"$W/st")" = "$K" ] && ok "Ergebnis nennt den Schlüssel des Originals" || bad "Ergebnis" "$(cat "$W/st")"

# ---------------------------------------------------------------- nichts überschreiben
echo geheim >"$B/homebrew/Astro/eigen.txt"
code=$(post "{\"keys\":\"$K\",\"device\":\"$DEV2\",\"verify\":true}" move)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'schon vor' "$W/st" && [ -f "$B/homebrew/Astro/eigen.txt" ] \
    && ok "Ziel existiert: nichts überschrieben, Fehler gemeldet" || bad "Überschreiben" "$code $(cat "$W/st")"

# ---------------------------------------------------------------- ohne Prüfen, zwei Spiele
library
K2=$(key "$A/Zwei")
code=$(post "{\"keys\":\"$K2\",\"device\":\"$DEV2\",\"verify\":false}" move)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && diff -r "$A/Zwei" "$B/homebrew/Zwei" >/dev/null \
    && ok "Verschieben ohne Prüfen" || bad "ohne Prüfen" "$code $(cat "$W/st")"

# ---------------------------------------------------------------- Schutz
code=$(post "{\"keys\":\"$K\",\"device\":\"$DEV1\",\"verify\":true}" move)
[ "$code" = 404 ] && ok "gleiches Laufwerk abgelehnt" || bad "gleiches Laufwerk" "$code $(cat "$W/body")"
code=$(post "{\"keys\":\"$K\",\"device\":\"nope\",\"verify\":true}" move)
[ "$code" = 404 ] && ok "unbekanntes Ziel abgelehnt" || bad "unbekanntes Ziel" "$code"
code=$(post "{\"keys\":\"${K}XYZ\",\"device\":\"$DEV2\"}" move)
[ "$code" = 404 ] && ok "Schlüssel mit Anhang abgelehnt" || bad "Schlüssel mit Anhang" "$code"
code=$(post "{\"device\":\"$DEV2\"}" move)
[ "$code" = 400 ] && ok "ohne Schlüssel abgelehnt" || bad "ohne Schlüssel" "$code"
mkdir -p "$W/sandbox/PPSA01325_000"
start USBFMT_SANDBOX="$W/sandbox"
library
code=$(post "{\"keys\":\"$(key "$A/Gross")\",\"device\":\"$DEV2\"}" move)
[ "$code" = 409 ] && [ ! -e "$B/homebrew/Gross" ] && ok "laufendes Spiel: abgelehnt" || bad "laufendes Spiel" "$code"
rm -rf "$W/sandbox"

# ---------------------------------------------------------------- Abbrechen räumt auf
start USBFMT_SLOW=1
library
KG=$(key "$A/Gross")
code=$(post "{\"keys\":\"$KG\",\"device\":\"$DEV2\",\"verify\":true}" move)
for _ in $(seq 1 100); do curl -s "$U/api/status" >"$W/st"; [ "$(q 'd["percent"] > 1' <"$W/st")" = True ] && break; sleep 0.1; done
curl -s -H "$J" -d '{}' "$U/api/cancel" >/dev/null
wait_job
[ "$code" = 202 ] && [ "$(q 'd["ok"]' <"$W/st")" = False ] && [ ! -e "$B/homebrew/Gross" ] && [ ! -e "$B/.usbfmt-move" ] && [ -f "$A/Gross/riesig.bin" ] \
    && ok "Abbrechen: nichts am Ziel, Zwischenordner weg, Original unberührt" || bad "Abbrechen" "$code $(cat "$W/st") $(ls -a "$B" "$B/homebrew" 2>&1)"

# ---------------------------------------------------------------- beschädigte Kopie wird erkannt
start USBFMT_MOVE_CORRUPT=1
library
rm -rf "$B/homebrew/Zwei"
code=$(post "{\"keys\":\"$K2\",\"device\":\"$DEV2\",\"verify\":true}" move)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'weicht' "$W/st" && [ ! -e "$B/homebrew/Zwei" ] && [ ! -e "$B/.usbfmt-move" ]     && ok "beschädigte Kopie: Prüfung schlägt an, nichts übernommen, Zwischenordner weg" || bad "beschädigte Kopie" "$code $(cat "$W/st") $(ls -a "$B" "$B/homebrew" 2>&1)"

echo
[ $fails -eq 0 ] && echo "Alle Verschieben-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
