#!/usr/bin/env bash
# Formaterkennung (/api/devices "format") nach exFAT, FAT32, NTFS, ext4, Erfolgsmeldung und die gespeicherte
# Größe der Oberfläche (/api/settings scale_tv/scale_pc, auch nach einem Neustart).
# NTFS/ext4 brauchen mkntfs/mke2fs im PATH (z. B. ~/fstools/root/usr/sbin).
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18094 || exit 1
truncate -s 1G "$W/a.img"
start() { USBFMT_IMAGES="$W/a.img" USBFMT_DATA="$W/data" "$W/srv" >>"$W/log" 2>&1 & PID=$!; sleep 0.5; }
start
U=http://127.0.0.1:18094
fails=0
ok()   { echo "OK      $1"; }
bad()  { echo "FEHLER  $1"; fails=$((fails + 1)); }
post() { curl -s -o "$W/body" -w '%{http_code}' -H 'Content-Type: application/json' -d "$1" "$U/api/$2"; }
format_of() { curl -s "$U/api/devices" | python3 -c 'import json,sys; print(json.load(sys.stdin)["devices"][0]["format"])'; }

[ "$(format_of)" = "leer" ] && ok "leeres Image: leer" || bad "leeres Image: $(format_of)"

for pair in exfat:exFAT fat32:FAT32 ntfs:NTFS ext4:ext4; do
    fs=${pair%%:*}; want=${pair#*:}
    code=$(post "{\"device\":\"da0\",\"fs\":\"$fs\",\"label\":\"TEST\",\"size\":1073741824}" format)
    [ "$code" = 202 ] || { bad "$fs nicht angenommen ($code): $(cat "$W/body")"; continue; }
    for _ in $(seq 1 100); do curl -s "$U/api/status" >"$W/st"; grep -q '"done":true' "$W/st" && break; sleep 0.2; done
    grep -q '"ok":true' "$W/st" || { bad "$fs fehlgeschlagen: $(cat "$W/st")"; continue; }
    grep -q 'erfolgreich formatiert' "$W/st" && ok "$fs: Meldung \"erfolgreich formatiert\"" || bad "$fs: Meldung $(cat "$W/st")"
    got=$(format_of)
    [ "$got" = "$want" ] && ok "$fs erkannt als $got" || bad "$fs erkannt als '$got' statt $want"
done

# Einstellungen: Standardwerte, Ändern, Prüfen, Speichern über einen Neustart
curl -s "$U/api/settings" >"$W/s"
grep -q '"scale_tv":160' "$W/s" && grep -q '"scale_pc":100' "$W/s" && ok "Standardgrößen 160/100" || bad "Standardgrößen: $(cat "$W/s")"
[ "$(post '{"scale_tv":120}' settings)" = 200 ] && grep -q '"scale_tv":120' "$W/body" && ok "scale_tv 120 gesetzt" \
    || bad "scale_tv setzen: $(cat "$W/body")"
[ "$(post '{"theme":"dark"}' settings)" = 200 ] && grep -q '"scale_tv":120' "$W/body" && grep -q '"theme":"dark"' "$W/body" \
    && ok "nur theme ändern lässt die Größe" || bad "theme ändern: $(cat "$W/body")"
[ "$(post '{"scale_pc":300}' settings)" = 400 ] && ok "Größe 300 abgelehnt" || bad "Größe 300 nicht abgelehnt: $(cat "$W/body")"
[ "$(post '{"scale_pc":"abc"}' settings)" = 400 ] && ok "Größe abc abgelehnt" || bad "Größe abc nicht abgelehnt"
[ "$(post '{}' settings)" = 400 ] && ok "leere Einstellung abgelehnt" || bad "leere Einstellung nicht abgelehnt"

# Sprache: Meldungen nach ?lang=, sonst gespeicherter Wahl, sonst Accept-Language, sonst Deutsch
curl -s "$U/api/settings" >"$W/s"
grep -q '"lang":""' "$W/s" && ok "Sprache anfangs nicht gewählt" || bad "Sprache anfangs: $(cat "$W/s")"
post '{"scale_pc":300}' settings >/dev/null
grep -q 'Größe muss' "$W/body" && ok "ohne Angabe: Deutsch" || bad "ohne Angabe: $(cat "$W/body")"
post '{"scale_pc":300}' 'settings?lang=en' >/dev/null
grep -q 'Size must be' "$W/body" && ok "?lang=en: Englisch" || bad "?lang=en: $(cat "$W/body")"
curl -s -o "$W/body" -H 'Content-Type: application/json' -H 'Accept-Language: pt-BR,es;q=0.8,en;q=0.5' \
    -d '{"scale_pc":300}' "$U/api/settings"
grep -q 'El tamaño' "$W/body" && ok "Accept-Language: erste unterstützte (es)" || bad "Accept-Language: $(cat "$W/body")"
[ "$(post '{"lang":"xx"}' settings)" = 400 ] && ok "Sprache xx abgelehnt" || bad "Sprache xx nicht abgelehnt: $(cat "$W/body")"
[ "$(post '{"lang":"fr"}' settings)" = 200 ] && grep -q '"lang":"fr"' "$W/body" && ok "Sprache fr gespeichert" \
    || bad "Sprache fr: $(cat "$W/body")"
curl -s -o "$W/body" -H 'Content-Type: application/json' -H 'Accept-Language: es' -d '{"scale_pc":300}' "$U/api/settings"
grep -q 'La taille' "$W/body" && ok "gespeicherte Sprache vor Accept-Language" || bad "gespeichert vs. Browser: $(cat "$W/body")"
post '{"scale_pc":300}' 'settings?lang=it' >/dev/null
grep -q 'La dimensione' "$W/body" && ok "?lang= vor gespeicherter Sprache" || bad "?lang= vs. gespeichert: $(cat "$W/body")"
code=$(curl -s -o "$W/help" -w '%{http_code}' "$U/help?lang=en")
[ "$code" = 200 ] && [ "$(grep -c '<article' "$W/help")" = 5 ] && ok "Anleitung /help mit fünf Sprachen" \
    || bad "Anleitung: $code, $(grep -c '<article' "$W/help") Sprachen"
curl -s "$U/api/status" | grep -q '"cancelled":false' && ok "Status meldet cancelled" || bad "Status ohne cancelled"
V=$(cat ../VERSION)
curl -s "$U/api/info" | grep -q "\"version\":\"$V\"" && ok "/api/info meldet Version $V" || bad "Version: $(curl -s "$U/api/info")"

kill $PID; wait $PID 2>/dev/null
start
curl -s "$U/api/settings" >"$W/s"
grep -q '"scale_tv":120' "$W/s" && grep -q '"theme":"dark"' "$W/s" && grep -q '"lang":"fr"' "$W/s" \
    && ok "nach Neustart noch gespeichert: $(cat "$W/s")" || bad "nach Neustart: $(cat "$W/s")"
[ "$(post '{"lang":""}' settings)" = 200 ] && grep -q '"lang":""' "$W/body" && ok "Sprache zurück auf Browser" \
    || bad "Sprache zurücksetzen: $(cat "$W/body")"

echo
[ $fails -eq 0 ] && echo "Alle Format-/Einstellungs-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
