#!/usr/bin/env bash
# /api/open: öffnet den Browser nur auf eine gültige JSON-POST-Anfrage (so wie sie app/eboot.c schickt).
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18092 || exit 1
"$W/srv" >"$W/log" 2>&1 &
PID=$!
sleep 0.5
fails=0
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
chk() { if [ "$2" = "$3" ]; then echo "OK      $1 ($2)"; else echo "FEHLER  $1: $2 statt $3"; fails=$((fails+1)); fi; }

# exakt die Anfrage des App-Starters, als Rohbytes
REQ=$'POST /api/open HTTP/1.1\r\nHost: 127.0.0.1:8090\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}'
RESP=$(printf '%s' "$REQ" | sed 's/8090/18092/' | timeout 3 bash -c 'exec 3<>/dev/tcp/127.0.0.1/18092; cat >&3; cat <&3')
chk "Anfrage des App-Starters" "$(echo "$RESP" | head -1 | tr -d '\r')" "HTTP/1.1 200 OK"
chk "Browser geöffnet" "$(grep -c '\[Browser\] http://127.0.0.1:18092/' "$W/log")" 1
chk "fremde Webseite (Origin) abgelehnt" "$(code -H 'Content-Type: application/json' -H 'Origin: http://boese.example' -d '{}' http://127.0.0.1:18092/api/open)" 403
chk "Formular-POST abgelehnt" "$(code -d 'x=1' http://127.0.0.1:18092/api/open)" 415
chk "GET abgelehnt" "$(code http://127.0.0.1:18092/api/open)" 405
chk "Browser nur einmal geöffnet" "$(grep -c '\[Browser\]' "$W/log")" 1
echo
[ $fails -eq 0 ] && echo "Alle Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
