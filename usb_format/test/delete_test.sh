#!/usr/bin/env bash
# Spiele löschen (POST /api/delete) gegen eine ShadowMountPlus-Attrappe (test/smp_mock.py): verwaltete Spiele über
# ShadowMountPlus samt Kachel, eine zweite Kopie selbst, laufendes Spiel abgelehnt, interne Spiele und fehlendes
# ShadowMountPlus geschützt.
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
W=$(mktemp -d)
PID= MPID=
trap 'kill $PID $MPID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18096 || exit 1

U=http://127.0.0.1:18096
J='Content-Type: application/json'
SMP_PORT=18098
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
post() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$1" "$U/api/$2"; }
wait_job() { for _ in $(seq 1 300); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.1; done; return 1; }
game() {   # game <ordner> <titel-id>
    mkdir -p "$1/sce_sys"
    printf '{"titleId":"%s","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"%s"}}}' "$2" "$2" \
        >"$1/sce_sys/param.json"
}
key() { q "[t['key'] for t in d['titles'] if t['path'] == '$1'][0]" <"$W/lib"; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
start() {   # start [weitere Umgebung]
    [ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }
    env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_INTERNAL="$W/data/homebrew" \
        USBFMT_DATA="$W/cfg" "$@" "$W/srv" >>"$W/log" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && return; sleep 0.1; done
}

H="$W/mnt/usb0/homebrew"
game "$H/Astro" PPSA01325
game "$H/Crazy" PPSA03117
game "$H/Crazy Kopie" PPSA03117
game "$H/Laeuft" PPSA09999
game "$W/data/homebrew/Intern" PPSA05555
truncate -s 64M "$W/a.img"
cat >"$W/games.json" <<EOF
[{"title_id":"PPSA01325","path":"$H/Astro"},{"title_id":"PPSA03117","path":"$H/Crazy"},
 {"title_id":"PPSA09999","path":"$H/Laeuft","busy":true},{"title_id":"PPSA05555","path":"$W/data/homebrew/Intern"},
 {"title_id":"PPSA07777","path":"$W/mnt/usb0/Viele-Original"}]
EOF
game "$H/Unbekannt" PPSA06666   # kennt ShadowMountPlus nicht (z. B. noch nicht erfasst)
python3 test/smp_mock.py $SMP_PORT "$W/games.json" "$W/smp.log" &
MPID=$!
sleep 0.5

# ---------------------------------------------------------------- ohne ShadowMountPlus: nichts löschen
start
library
code=$(post "{\"keys\":\"$(key "$H/Crazy Kopie")\",\"uninstall\":true}" delete)
[ "$code" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'nicht erreichbar' "$W/st" && [ -d "$H/Crazy Kopie" ] \
    && ok "ohne ShadowMountPlus: abgelehnt, nichts gelöscht" || bad "ohne ShadowMountPlus" "$code $(cat "$W/body" "$W/st")"

# ---------------------------------------------------------------- mit ShadowMountPlus
start USBFMT_SMP_PORT=$SMP_PORT
library
[ "$(q 'all(len(t["key"]) == 8 for t in d["titles"])' <"$W/lib")" = True ] && ok "Bibliothek liefert Schlüssel" \
    || bad "Schlüssel" "$(cat "$W/lib")"
code=$(post "{\"keys\":\"$(key "$W/data/homebrew/Intern")\"}" delete)
[ "$code" = 404 ] && [ -d "$W/data/homebrew/Intern" ] && ok "internes Spiel (/data/homebrew) nicht löschbar" \
    || bad "internes Spiel" "$code $(cat "$W/body")"
code=$(post '{"keys":"","uninstall":true}' delete)
[ "$code" = 400 ] && ok "leere Auswahl abgelehnt" || bad "leere Auswahl" "$code"

# verwaltete Kopie absichtlich VOR der zweiten Kopie: der Server muss die zweite trotzdem zuerst (sicher) löschen
code=$(post "{\"keys\":\"$(key "$H/Astro"),$(key "$H/Crazy"),$(key "$H/Crazy Kopie")\",\"uninstall\":true}" delete)
[ "$code" = 202 ] && wait_job || bad "Löschen starten" "$code $(cat "$W/body")"
[ "$(q 'd["ok"]' <"$W/st")" = True ] && [ "$(q 'd["result"]["deleted"]' <"$W/st")" = 3 ] \
    && [ "$(q 'd["result"]["tiles"]' <"$W/st")" = 2 ] && ok "3 Spiele gelöscht, 2 Kacheln entfernt" || bad "Ergebnis" "$(cat "$W/st")"
[ ! -e "$H/Astro" ] && [ ! -e "$H/Crazy" ] && [ ! -e "$H/Crazy Kopie" ] && ok "Ordner sind weg" || bad "Ordner noch da" "$(ls "$H")"
grep -qx 'delete PPSA01325' "$W/smp.log" && grep -qx 'uninstall PPSA01325' "$W/smp.log" \
    && [ "$(grep -c 'PPSA03117' "$W/smp.log")" = 2 ] \
    && ok "verwaltete über ShadowMountPlus (löschen + Kachel), die Kopie selbst gelöscht" || bad "Aufrufe" "$(cat "$W/smp.log")"
grep -q '3 Spiele gelöscht: .*2 Kacheln von der Startseite entfernt' "$W/st" && ok "Meldung" || bad "Meldung" "$(cat "$W/st")"

library
code=$(post "{\"keys\":\"$(key "$H/Laeuft")\",\"uninstall\":true}" delete)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'läuft gerade' "$W/st" && [ -d "$H/Laeuft" ] \
    && ok "laufendes Spiel: abgelehnt, bleibt erhalten" || bad "laufendes Spiel" "$code $(cat "$W/st")"
library
code=$(post "{\"keys\":\"$(key "$H/Unbekannt")\"}" delete)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q 'kennt dieses Spiel noch nicht' "$W/st" \
    && [ -d "$H/Unbekannt" ] && ok "unbekanntes Spiel (evtl. eingebunden): nicht selbst gelöscht" || bad "unbekanntes Spiel" "$code $(cat "$W/st")"
library
[ "$(q 'len([t for t in d["titles"] if t["device"] == "da0"])' <"$W/lib")" = 2 ] && ok "Bibliothek danach aktualisiert" \
    || bad "Bibliothek danach" "$(cat "$W/lib")"

# ---------------------------------------------------------------- Fortschritt beim Selbst-Löschen (zweite Kopie)
game "$H/Viele" PPSA07777
for i in $(seq 1 60); do head -c 200000 /dev/zero >"$H/Viele/datei$i.bin"; done
start USBFMT_SMP_PORT=$SMP_PORT USBFMT_SLOW=1
library
post "{\"keys\":\"$(key "$H/Viele")\"}" delete >/dev/null
seen=""
for _ in $(seq 1 300); do
    curl -s "$U/api/status" >"$W/st"
    p=$(q 'd["percent"] if d["busy"] else -1' <"$W/st")
    [ "$p" = -1 ] && break
    [ "$p" -gt 0 ] && [ "$p" -lt 100 ] && seen="$seen $p"
    sleep 0.05
done
[ -n "$seen" ] && [ ! -e "$H/Viele" ] && ok "Fortschritt steigt beim Löschen:$(echo $seen | tr ' ' '\n' | sort -nu | head -5 | tr '\n' ' ')…" \
    || bad "Fortschritt beim Löschen" "Zwischenwerte: '$seen' $(cat "$W/st")"

# mehrdeutiger Schlüssel (passt zu zwei Spielen): nichts löschen - per Hand nachgestellt mit zwei gleichen Pfaden ist
# nicht möglich, daher Gegenprobe: ein gültiger Schlüssel mit Unsinn dahinter wird als nicht gefunden behandelt
code=$(post '{"keys":"zzzzzzzz"}' delete)
[ "$code" = 404 ] && ok "unbekannter Schlüssel: nichts gelöscht" || bad "unbekannter Schlüssel" "$code $(cat "$W/body")"

echo
[ $fails -eq 0 ] && echo "Alle Lösch-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
