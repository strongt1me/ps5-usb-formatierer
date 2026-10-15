#!/usr/bin/env bash
# Parallele Verbindungen: ein langsamer Client blockiert andere nicht, viele gleichzeitige Anfragen werden beantwortet,
# ein laufender Auftrag bleibt währenddessen bedienbar, "Beenden" wirkt sofort. Mit HOST_CFLAGS=-fsanitize=thread
# zusätzlich auf Datenrennen prüfbar.
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18099 || exit 1

U=http://127.0.0.1:18099
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }

truncate -s 512M "$W/a.img"
USBFMT_IMAGES="$W/a.img" USBFMT_DATA="$W/cfg" USBFMT_SLOW=1 "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done

# 1. langsamer Client (1 Byte alle 2 s) - andere Anfragen müssen trotzdem sofort beantwortet werden
python3 - <<'EOF' &
import socket, time
s = socket.create_connection(("127.0.0.1", 18099))
try:
    for ch in b"GET /api/status HTTP/1.1\r\n":
        s.send(bytes([ch])); time.sleep(2)
except OSError:
    pass
EOF
DRIP=$!
sleep 0.5
t0=$(date +%s.%N)
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$U/api/status")
dt=$(echo "$(date +%s.%N) - $t0" | bc)
[ "$code" = 200 ] && [ "$(echo "$dt < 1" | bc)" = 1 ] && ok "langsamer Client blockiert nicht (Antwort nach ${dt:0:4} s)" \
    || bad "langsamer Client blockiert" "HTTP $code nach $dt s"

# 2. viele gleichzeitige Anfragen (unter der Grenze von 8 Verbindungen) - alle beantwortet
CURLS=
for i in $(seq 1 6); do curl -s -o /dev/null -w '%{http_code}\n' "$U/api/devices" >>"$W/codes" & CURLS="$CURLS $!"; done
wait $CURLS
[ "$(grep -c '^200$' "$W/codes")" = 6 ] && ok "6 gleichzeitige Anfragen beantwortet" || bad "gleichzeitige Anfragen" "$(cat "$W/codes")"

# 3. während eines Auftrags (verlangsamt): Status und Abbrechen erreichbar, zweiter Auftrag abgelehnt
curl -s -o /dev/null -H 'Content-Type: application/json' \
    -d "{\"device\":\"da0\",\"size\":$((512 * 1048576)),\"fs\":\"exfat\",\"verify\":true}" "$U/api/format"
sleep 0.5
st=$(curl -s --max-time 2 "$U/api/status")
echo "$st" | grep -q '"busy":true' && ok "Status während des Auftrags" || bad "Status während des Auftrags" "$st"
code=$(curl -s -o "$W/b" -w '%{http_code}' -H 'Content-Type: application/json' \
    -d "{\"device\":\"da0\",\"size\":$((512 * 1048576)),\"fs\":\"exfat\"}" "$U/api/format")
[ "$code" = 409 ] && ok "zweiter Auftrag abgelehnt (409)" || bad "zweiter Auftrag" "$code $(cat "$W/b")"
code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' -d '{}' "$U/api/quit")
[ "$code" = 409 ] && ok "Beenden während des Auftrags abgelehnt" || bad "Beenden während Auftrag" "$code"
curl -s -o /dev/null -H 'Content-Type: application/json' -d '{}' "$U/api/cancel"
for _ in $(seq 1 100); do curl -s "$U/api/status" | grep -q '"busy":false' && break; sleep 0.1; done
curl -s "$U/api/status" | grep -q '"busy":false' && ok "Abbrechen wirkt" || bad "Abbrechen"

# 4. Beenden wirkt sofort (Server-Prozess endet binnen 2 s), obwohl noch ein langsamer Client hängt
code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' -d '{}' "$U/api/quit")
for _ in $(seq 1 100); do kill -0 $PID 2>/dev/null || break; sleep 0.1; done   # wartet bis 5 s auf offene Antworten
if [ "$code" = 200 ] && ! kill -0 $PID 2>/dev/null; then ok "Beenden: Server endet"; PID=; else bad "Beenden" "HTTP $code, läuft noch"; fi
kill $DRIP 2>/dev/null
grep -E "WARNING: ThreadSanitizer|ERROR: AddressSanitizer" "$W/log" >/dev/null && bad "Sanitizer-Bericht" "$(head -30 "$W/log")"

echo
[ $fails -eq 0 ] && echo "Alle Parallel-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
