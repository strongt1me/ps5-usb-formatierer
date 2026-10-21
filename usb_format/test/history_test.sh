#!/usr/bin/env bash
# Auftragsverlauf (GET /api/history, POST /api/history/clear): Formatieren, Umbenennen und ein abgebrochener Auftrag
# landen mit Ergebnis darin, neueste zuerst; der Verlauf übersteht einen Neustart, ist auf 100 Einträge begrenzt,
# überspringt kaputte Zeilen und lässt sich leeren.
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18101 || exit 1
U=http://127.0.0.1:18101
J='Content-Type: application/json'
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 600); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.1; done; return 1; }
start() {
    env USBFMT_IMAGES="$W/a.img" USBFMT_MODELS="Teststick" USBFMT_DATA="$W/cfg" USBFMT_SANDBOX="$W/sandbox" USBFMT_SLOW=1 \
        "$W/srv" >>"$W/log" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
}
stop() { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }

mkdir -p "$W/cfg" "$W/sandbox"
truncate -s 256M "$W/a.img"
start
size=$(curl -s "$U/api/devices" | q 'd["devices"][0]["size"]')

curl -s "$U/api/history" >"$W/h"
[ "$(q 'd["entries"]' <"$W/h")" = "[]" ] && ok "anfangs leer" || bad "anfangs" "$(cat "$W/h")"
# Update-Hinweis der Seite: nach außen darf sie nur die GitHub-API fragen
csp=$(curl -sI "$U/" | tr -d '\r' | grep -i '^content-security-policy:')
echo "$csp" | grep -q "connect-src 'self' https://api.github.com;" && ok "CSP erlaubt nur die GitHub-API zusätzlich" \
    || bad "CSP" "$csp"

curl -s -o /dev/null -H "$J" -d "{\"device\":\"da0\",\"fs\":\"exfat\",\"label\":\"VERLAUF\",\"size\":$size}" "$U/api/format"
wait_job
curl -s -o /dev/null -H "$J" -d "{\"device\":\"da0\",\"label\":\"NEU\",\"size\":$size}" "$U/api/label"
wait_job
curl -s -o /dev/null -H "$J" -d "{\"device\":\"da0\",\"size\":$size}" "$U/api/speed"
sleep 0.5   # Tempomessung läuft (verlangsamt) - abbrechen
curl -s -o /dev/null -H "$J" -d '{}' "$U/api/cancel"
wait_job
curl -s "$U/api/history" >"$W/h"
[ "$(q '[e["kind"] for e in d["entries"]]' <"$W/h")" = "['speed', 'label', 'format']" ] \
    && ok "drei Aufträge, neueste zuerst" || bad "Reihenfolge" "$(cat "$W/h")"
[ "$(q '(d["entries"][2]["ok"], d["entries"][2]["fs"], d["entries"][2]["label"], d["entries"][2]["device"], d["entries"][2]["model"], d["entries"][2]["size"] > 0)' <"$W/h")" \
    = "(True, 'exFAT', 'VERLAUF', 'da0', 'Teststick', True)" ] && ok "Formatieren: Dateisystem, Bezeichnung, Laufwerk" \
    || bad "Formatieren-Eintrag" "$(q 'd["entries"][2]' <"$W/h")"
ident=$(curl -s "$U/api/devices" | q 'd["devices"][0]["ident"]')
[ -n "$ident" ] && [ "$(q 'd["entries"][2].get("ident")' <"$W/h")" = "$ident" ] \
    && ok "Seriennummer im Eintrag (für \"Zuletzt\" am Laufwerk)" || bad "Seriennummer" "$ident / $(q 'd["entries"][2]' <"$W/h")"
[ "$(q '(d["entries"][1]["ok"], d["entries"][1]["label"])' <"$W/h")" = "(True, 'NEU')" ] \
    && ok "Umbenennen: neue Bezeichnung" || bad "Umbenennen-Eintrag" "$(q 'd["entries"][1]' <"$W/h")"
[ "$(q '(d["entries"][0]["ok"], d["entries"][0]["cancelled"], len(d["entries"][0]["msg"]) > 0, isinstance(d["entries"][0]["t"], int), d["entries"][0]["secs"] >= 0)' <"$W/h")" \
    = "(False, True, True, True, True)" ] && ok "abgebrochener Auftrag mit Meldung: $(q 'd["entries"][0]["msg"][:60]' <"$W/h")" \
    || bad "Fehler-Eintrag" "$(q 'd["entries"][0]' <"$W/h")"

# Neustart: Verlauf bleibt; kaputte Zeile wird übersprungen
stop
printf '{"t":1,"kind":"format"\nkaputt\n' >>"$W/cfg/history.jsonl"
start
curl -s "$U/api/history" >"$W/h"
[ "$(q 'len(d["entries"])' <"$W/h")" = 3 ] && ok "nach Neustart noch da, kaputte Zeilen übersprungen" \
    || bad "Neustart" "$(cat "$W/h")"

# höchstens 100 Einträge
python3 - "$W/cfg/history.jsonl" <<'EOF'
import json, sys
with open(sys.argv[1], "w") as f:
    for i in range(120):
        f.write(json.dumps({"t": i, "kind": "speed", "ok": True, "msg": f"alt {i}"}) + "\n")
EOF
curl -s -o /dev/null -H "$J" -d "{\"device\":\"da0\",\"label\":\"X\",\"size\":$size}" "$U/api/label"
wait_job
curl -s "$U/api/history" >"$W/h"
[ "$(q '(len(d["entries"]), d["entries"][0]["kind"], d["entries"][-1]["msg"])' <"$W/h")" = "(100, 'label', 'alt 98')" ] \
    && ok "auf 100 Einträge begrenzt, älteste fallen weg" || bad "Begrenzung" "$(q '(len(d["entries"]), d["entries"][-1])' <"$W/h")"

code=$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d '{}' "$U/api/history/clear")
curl -s "$U/api/history" >"$W/h"
[ "$code" = 200 ] && [ "$(q 'd["entries"]' <"$W/h")" = "[]" ] && ok "geleert" || bad "leeren" "$code $(cat "$W/h")"
code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Content-Type: text/plain' -d '{}' "$U/api/history/clear")
[ "$code" = 415 ] && ok "Leeren nur mit JSON (kein Formular-Trick)" || bad "Leeren ohne JSON" "$code"

[ $fails -eq 0 ] && echo "Alle Verlaufs-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
