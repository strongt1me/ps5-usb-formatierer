#!/usr/bin/env bash
# Zusammenspiel mit ShadowMountPlus 1.7 gegen die Attrappe (test/smp_mock.py): Umgebung (/api/system: Firmware,
# Kernel-Erweiterungen, Version und Fähigkeiten), Laufwerk sicher auswerfen (Erfolg, belegt, nicht auswerfbar, ohne
# ShadowMountPlus), Spielbilder (Vorschau von ShadowMountPlus, sonst icon0.png des Spiels), debug.log nur ohne API,
# Spiel ein-/aushängen sowie Patch und DLC anzeigen und entfernen.
# Aufruf: test/smp_test.sh   (baut selbst, Ports 18152/18153)
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID= MPID=
trap 'kill $PID $MPID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18152 || exit 1
U=http://127.0.0.1:18152
J='Content-Type: application/json'
SMP_PORT=18153
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 300); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.1; done; return 1; }
library() { for _ in $(seq 1 50); do curl -s "$U/api/library?refresh=1" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && break; sleep 0.1; done; }
key() { q "[t['key'] for t in d['titles'] if t['path'].endswith('/$1')][0]" <"$W/lib"; }
smp() {   # smp [mounts.json-Inhalt]: Attrappe (neu) starten; ohne Angabe: keine Laufwerke
    [ -n "$MPID" ] && { kill $MPID 2>/dev/null; wait $MPID 2>/dev/null; }
    printf '%s' "${1:-[]}" >"$W/mounts.json"
    : >"$W/smp.log"
    python3 test/smp_mock.py $SMP_PORT "$W/games.json" "$W/smp.log" "$W/mounts.json" &
    MPID=$!
    for _ in $(seq 1 30); do curl -s -o /dev/null -H "$J" -d '{}' "http://127.0.0.1:$SMP_PORT/api/v1/games" && break; sleep 0.1; done
}
eject() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "{\"device\":\"da0\",\"size\":$((64 * 1048576))}" "$U/api/eject"; }

truncate -s 64M "$W/a.img"
H="$W/mnt/usb0/homebrew"
mkdir -p "$H/MitBild/sce_sys" "$H/Eigenes/sce_sys" "$H/Ohne/sce_sys"
for g in MitBild:PPSA00071 Eigenes:PPSA00072 Ohne:PPSA00073; do
    printf '{"titleId":"%s","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"%s"}}}' "${g#*:}" "${g%%:*}" \
        >"$H/${g%%:*}/sce_sys/param.json"
done
printf '\x89PNG\r\n\x1a\nEIGENES-BILD' >"$H/Eigenes/sce_sys/icon0.png"
printf 'kein Bild' >"$H/Ohne/sce_sys/icon0.png"
head -c 4096 /dev/zero >"$H/Spiel.ffpfs"
printf '[{"title_id":"PPSA00071","path":"%s","icon":true,"mounted":true,"patch":{"installed":true,"version":"01.050.000","size_bytes":1048576},"dlc":[{"content_id":"EP0001-PPSA00071_00-DLCEINS000000000","title_name":"Erweiterung","version":"01.000","size_bytes":2048},{"content_id":"EP0001-PPSA00071_00-DLCZWEI000000000","title_name":"Zweite","version":"01.000","size_bytes":4096}]},{"title_id":"PPSA00072","path":"%s"}]' "$H/MitBild" "$H/Eigenes" >"$W/games.json"
# debug.log einer älteren Fassung: Titel-ID des .ffpfs (die Attrappe kennt das Abbild nicht)
printf '[LINK] mount_img.lnk created: /user/app/PPSA03117/mount_img.lnk -> %s layers=1\n' "$H/Spiel.ffpfs" >"$W/debug.log"
env USBFMT_IMAGES="$W/a.img" USBFMT_MOUNTED="1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" USBFMT_FW="12.00" \
    USBFMT_KEXT="kstuff, etaHEN" USBFMT_SMP_PORT=$SMP_PORT USBFMT_SMP_LOG="$W/debug.log" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done
MP=$(curl -s "$U/api/devices" | q 'd["devices"][0]["mounts"][0]["path"]')

# ---------------------------------------------------------------- ohne ShadowMountPlus
curl -s "$U/api/system" >"$W/sys"
[ "$(q 'd["fw"], d["kext"], d["smp"]' <"$W/sys")" = "('12.00', 'kstuff, etaHEN', None)" ] \
    && ok "Umgebung ohne ShadowMountPlus: Firmware, Kernel-Erweiterungen, smp null" || bad "Umgebung ohne SMP" "$(cat "$W/sys")"
library
[ "$(q "[t['id'] for t in d['titles'] if t['path'].endswith('/Spiel.ffpfs')][0]" <"$W/lib")" = PPSA03117 ] \
    && ok "ohne API: Titel-ID des Abbilds aus debug.log (ShadowMountPlus vor 1.7)" || bad "debug.log ohne API" "$(cat "$W/lib" | head -c 600)"
[ "$(eject)" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'nicht erreichbar' "$W/st" \
    && ok "Auswerfen ohne ShadowMountPlus: verständlich abgelehnt" || bad "Auswerfen ohne SMP" "$(cat "$W/st" "$W/body")"

# ---------------------------------------------------------------- mit ShadowMountPlus
smp "[{\"source\":\"/dev/da0s1\",\"mount_point\":\"$MP\",\"filesystem\":\"exfatfs\",\"ejectable\":true,\"mount_id\":\"00112233aabbccdd\"}]"
curl -s "$U/api/system" >"$W/sys"
[ "$(q 'd["smp"]["version"], "storage_eject" in d["smp"]["caps"]' <"$W/sys")" = "('1.7', True)" ] \
    && ok "Umgebung: ShadowMountPlus 1.7 mit Fähigkeiten" || bad "Umgebung mit SMP" "$(cat "$W/sys")"
library
[ "$(q "[t['id'] for t in d['titles'] if t['path'].endswith('/Spiel.ffpfs')][0]" <"$W/lib")" = "" ] \
    && ok "mit API: debug.log wird nicht mehr gelesen" || bad "debug.log trotz API" "$(grep -o 'Spiel.ffpfs[^}]*' "$W/lib")"

code=$(curl -s -o "$W/icon" -w '%{http_code} %{content_type}' "$U/api/icon?key=$(key MitBild)")
[ "$code" = "200 image/png" ] && grep -q "icon PPSA00071" "$W/smp.log" && [ "$(head -c 4 "$W/icon" | tail -c 3)" = PNG ] \
    && ok "Spielbild: Vorschau von ShadowMountPlus" || bad "Bild von SMP" "$code"
code=$(curl -s -o "$W/icon" -w '%{http_code}' "$U/api/icon?key=$(key Eigenes)")
[ "$code" = 200 ] && grep -q EIGENES-BILD "$W/icon" && ok "Spielbild: ohne Vorschau das icon0.png des Spiels" || bad "eigenes Bild" "$code"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$U/api/icon?key=$(key Ohne)")" = 204 ] && ok "Spielbild: kein gültiges PNG - 204 (ohne Fehler im Browser)" || bad "kein Bild"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$U/api/icon?key=zz")" = 400 ] && [ "$(curl -s -o /dev/null -w '%{http_code}' "$U/api/icon?key=1")" = 404 ] \
    && ok "Spielbild: ungültiger bzw. unbekannter Schlüssel abgelehnt" || bad "Bild-Schlüssel"

[ "$(eject)" = 202 ] && wait_job && grep -q '"ok":true' "$W/st" && grep -q 'ausgeworfen' "$W/st" \
    && grep -q "eject $MP /dev/da0s1 00112233aabbccdd" "$W/smp.log" \
    && ok "Auswerfen: Einbindung samt mount_id aus der Liste von ShadowMountPlus, Auftrag verfolgt" || bad "Auswerfen" "$(cat "$W/st" "$W/smp.log")"
curl -s "$U/api/history" | q 'd["entries"][0]["kind"], d["entries"][0]["ok"]' | grep -q "('eject', True)" && ok "Auswerfen im Verlauf" || bad "Verlauf"

smp "[{\"source\":\"/dev/da0s1\",\"mount_point\":\"$MP\",\"ejectable\":true,\"mount_id\":\"00112233aabbccdd\",\"fail\":\"usb_mount_busy\"}]"
[ "$(eject)" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'noch benutzt' "$W/st" \
    && ok "Auswerfen: belegtes Laufwerk verständlich gemeldet" || bad "belegt" "$(cat "$W/st")"
smp "[{\"source\":\"/dev/da0s1\",\"mount_point\":\"$MP\",\"ejectable\":false}]"
[ "$(eject)" = 202 ] && wait_job && grep -q 'nicht auswerfen' "$W/st" && ! grep -q "^eject" "$W/smp.log" \
    && ok "Auswerfen: nicht auswerfbar laut ShadowMountPlus - nichts gesendet" || bad "nicht auswerfbar" "$(cat "$W/st")"
smp '[]'
[ "$(eject)" = 202 ] && wait_job && grep -q 'kennt' "$W/st" && ok "Auswerfen: Einbindung unbekannt" || bad "unbekannt" "$(cat "$W/st")"

# Spiel bei ShadowMountPlus: Zustand, ein-/aushängen, Patch und DLC entfernen
gsmp() { curl -s -H "$J" -d "{\"key\":\"$(key "$1")\"}" "$U/api/game/smp"; }
gpost() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$2" "$U/api/game/$1"; }
[ "$(gsmp MitBild | q 'd["this"], d["mounted"], d["patch"]["installed"], len(d["dlc"])')" = "(True, True, True, 2)" ] \
    && ok "Spiel bei ShadowMountPlus: verwaltet diese Kopie, eingehängt, Patch und 2 DLCs" || bad "game/smp" "$(gsmp MitBild)"
[ "$(gsmp Spiel.ffpfs | q 'd["smp"]')" = False ] && ok "Spiel ohne Titel-ID: ShadowMountPlus nicht gefragt" || bad "ohne ID" "$(gsmp Spiel.ffpfs)"
[ "$(gpost mount "{\"key\":\"$(key MitBild)\",\"mount\":false}")" = 202 ] && wait_job && grep -q 'ausgehängt' "$W/st" \
    && grep -q "unmount PPSA00071" "$W/smp.log" && [ "$(gsmp MitBild | q 'd["mounted"]')" = False ] \
    && ok "Spiel ausgehängt" || bad "aushängen" "$(cat "$W/st" "$W/body")"
[ "$(gpost mount "{\"key\":\"$(key MitBild)\",\"mount\":true}")" = 202 ] && wait_job && grep -q 'eingehängt' "$W/st" \
    && [ "$(gsmp MitBild | q 'd["mounted"]')" = True ] && ok "Spiel wieder eingehängt" || bad "einhängen" "$(cat "$W/st")"
[ "$(gpost content/uninstall "{\"key\":\"$(key MitBild)\",\"kind\":\"patch\"}")" = 202 ] && wait_job && grep -q 'Patch von' "$W/st" \
    && [ "$(gsmp MitBild | q 'd["patch"]["installed"]')" = False ] && ok "Patch entfernt" || bad "Patch" "$(cat "$W/st")"
[ "$(gpost content/uninstall "{\"key\":\"$(key MitBild)\",\"kind\":\"dlc\",\"content_id\":\"EP0001-PPSA00071_00-DLCEINS000000000\"}")" = 202 ] \
    && wait_job && grep -q '"ok":true' "$W/st" && grep -q "content PPSA00071 dlc EP0001-PPSA00071_00-DLCEINS000000000" "$W/smp.log" \
    && [ "$(gsmp MitBild | q 'len(d["dlc"])')" = 1 ] && ok "DLC entfernt (genau dieser)" || bad "DLC" "$(cat "$W/st")"
[ "$(gpost content/uninstall "{\"key\":\"$(key MitBild)\",\"kind\":\"dlc\",\"content_id\":\"../../x\"}")" = 400 ] \
    && [ "$(gpost content/uninstall "{\"key\":\"$(key MitBild)\",\"kind\":\"alles\"}")" = 400 ] \
    && ok "ungültige content_id bzw. Art: abgelehnt" || bad "ungültig" "$(cat "$W/body")"
[ "$(gpost content/uninstall "{\"key\":\"$(key MitBild)\",\"kind\":\"dlc\",\"content_id\":\"EP0001-PPSA00071_00-GIBTESNICHT00000\"}")" = 202 ] \
    && wait_job && grep -q '"ok":false' "$W/st" && ok "unbekannter DLC: Fehler von ShadowMountPlus gemeldet" || bad "unbekannter DLC" "$(cat "$W/st")"
curl -s "$U/api/history" | q '[e["kind"] for e in d["entries"]][:6]' | grep -q "game_mount" && ok "Ein-/Aushängen und Entfernen im Verlauf" || bad "Verlauf"

kill $MPID 2>/dev/null; wait $MPID 2>/dev/null
SMP_MOCK_NOVERSION=1 smp '[]'
curl -s "$U/api/system" >"$W/sys"
[ "$(q 'd["smp"]["version"], d["smp"]["caps"]' <"$W/sys")" = "('', [])" ] \
    && ok "Umgebung: ShadowMountPlus ohne /api/v1/version - Version leer" || bad "ohne Version" "$(cat "$W/sys")"

kill -0 $PID 2>/dev/null && ok "Server läuft weiter" || bad "Server abgestürzt"
[ "$fails" = 0 ] && echo "Alle ShadowMountPlus-Tests bestanden." || { echo "$fails Fehler"; exit 1; }
