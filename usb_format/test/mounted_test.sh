#!/usr/bin/env bash
# Von der PS5 eingebundene Sticks: ohne "replug" abgelehnt, mit "replug" (abziehen, einstecken) formatiert,
# Warten darauf lässt sich abbrechen. Freie Sticks werden direkt formatiert.
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'kill $PID $PID2 2>/dev/null; rm -rf "$W"' EXIT
PID2=
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18091 || exit 1
truncate -s 1G "$W/a.img" "$W/b.img"
USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,0" "$W/srv" >"$W/log" 2>&1 &
PID=$!
sleep 0.5
U=http://127.0.0.1:18091
fails=0
post() { curl -s -o "$W/body" -w '%{http_code}' -H 'Content-Type: application/json' -d "$1" "$U/api/$2"; }
expect() { if [ "$2" = "$3" ]; then echo "OK      $1 ($2): $(head -c 110 "$W/body")"; else echo "FEHLER  $1: $2 statt $3: $(cat "$W/body")"; fails=$((fails+1)); fi; }
wait_done() {  # wartet, bis die Formatierung fertig ist, und prüft das Ergebnis
    for _ in $(seq 1 50); do curl -s "$U/api/status" >"$W/body"; grep -q '"done":true' "$W/body" && break; sleep 0.2; done
    grep -q "\"ok\":$2" "$W/body" && echo "OK      $1: $(head -c 140 "$W/body")" \
        || { echo "FEHLER  $1: $(cat "$W/body")"; fails=$((fails+1)); }
}

expect "eingebundener Stick ohne replug abgelehnt" "$(post '{"device":"da0","fs":"exfat","size":1073741824}' format)" 409
grep -q "replug" "$W/body" && echo "OK      Hinweis nennt replug" \
    || { echo "FEHLER  Hinweis ohne replug: $(cat "$W/body")"; fails=$((fails+1)); }
expect "freier Stick angenommen" "$(post '{"device":"da1","fs":"exfat","size":1073741824}' format)" 202
wait_done "freier Stick formatiert" true
expect "eingebundener Stick mit replug angenommen" \
    "$(post '{"device":"da0","fs":"exfat","size":1073741824,"replug":true}' format)" 202
wait_done "eingebundener Stick nach Abziehen/Einstecken formatiert" true

# Abbrechen, während auf das Abziehen gewartet wird: zweite Instanz auf demselben Port, wartet bis zum Abbruch
kill $PID; wait $PID 2>/dev/null
USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,0" USBFMT_REPLUG=wait "$W/srv" >"$W/log2" 2>&1 &
PID2=$!
sleep 0.5
expect "replug-Auftrag angenommen" "$(post '{"device":"da0","fs":"exfat","size":1073741824,"replug":true}' format)" 202
sleep 0.3
curl -s "$U/api/status" >"$W/body"
grep -q "abziehen" "$W/body" && echo "OK      Status fordert zum Abziehen auf: $(head -c 120 "$W/body")" \
    || { echo "FEHLER  Status ohne Aufforderung: $(cat "$W/body")"; fails=$((fails+1)); }
expect "Abbrechen angenommen" "$(post '{}' cancel)" 200
wait_done "Abbruch beendet den Auftrag" false
grep -q "Abgebrochen" "$W/body" || { echo "FEHLER  keine Abbruch-Meldung"; fails=$((fails+1)); }

echo
[ $fails -eq 0 ] && echo "Alle Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
