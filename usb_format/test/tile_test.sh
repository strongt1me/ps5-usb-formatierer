#!/usr/bin/env bash
# App-Kachel neu anmelden (GET/POST /api/tile) gegen die ShadowMountPlus-Attrappe (test/smp_mock.py): veraltete
# Anmeldung erkannt, Uninstall + Scan, danach neue Version; laufendes Spiel, geöffnete App und fehlendes ShadowMountPlus
# abgelehnt; ein eingehängtes Spiel wird beim Namen genannt.
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID= MPID=
trap 'kill $PID $MPID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18099 || exit 1
U=http://127.0.0.1:18099
J='Content-Type: application/json'
SMP_PORT=18100
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
wait_job() { for _ in $(seq 1 600); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.1; done; return 1; }
smp() {   # smp <games.json-Inhalt>: Attrappe (neu) starten
    [ -n "$MPID" ] && { kill $MPID 2>/dev/null; wait $MPID 2>/dev/null; }
    printf '%s' "$1" >"$W/games.json"
    : >"$W/smp.log"
    python3 test/smp_mock.py $SMP_PORT "$W/games.json" "$W/smp.log" &
    MPID=$!
    sleep 0.5
}

A="$W/data/homebrew/PPSA86920"
mkdir -p "$A/sce_sys" "$W/sandbox" "$W/cfg"
printf '{\n  "contentVersion": "02.005.007",\n  "titleId": "PPSA86920"\n}\n' >"$A/sce_sys/param.json"
truncate -s 64M "$W/a.img"
MINE=$(python3 -c 'import sys; a,b,c=open(sys.argv[1]).read().split("."); print(f"{int(a):02}.{int(b):03}.{int(c):03}")' ../VERSION)
env USBFMT_IMAGES="$W/a.img" USBFMT_DATA="$W/cfg" USBFMT_SANDBOX="$W/sandbox" USBFMT_SMP_PORT=$SMP_PORT \
    "$W/srv" >>"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && break; sleep 0.1; done

# ---------------------------------------------------------------- ohne ShadowMountPlus
curl -s "$U/api/tile" >"$W/t"
[ "$(q 'd["smp"]' <"$W/t")" = False ] && ok "ohne ShadowMountPlus: smp false" || bad "ohne SMP: Zustand" "$(cat "$W/t")"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d '{}' "$U/api/tile")
[ "$code" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'nicht erreichbar' "$W/st" \
    && ok "ohne ShadowMountPlus: Auftrag scheitert verständlich" || bad "ohne SMP: Auftrag" "$code $(cat "$W/st")"

# ---------------------------------------------------------------- veraltete Anmeldung
smp "[{\"title_id\":\"PPSA86920\",\"path\":\"$A\",\"version\":\"02.005.006\",\"rescan_version\":\"02.005.007\"}]"
curl -s "$U/api/tile" >"$W/t"
[ "$(q '(d["smp"], d["found"], d["installed"], d["registered"], d["folder"], d["formatter"])' <"$W/t")" \
    = "(True, True, True, '02.005.006', '02.005.007', '$MINE')" ] && ok "Zustand: angemeldet 02.005.006, Ordner 02.005.007" \
    || bad "Zustand" "$(cat "$W/t")"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d '{}' "$U/api/tile")
[ "$code" = 202 ] && wait_job && grep -q '"ok":true' "$W/st" && grep -q '"kind":"tile"' "$W/st" \
    && grep -q '02.005.007' "$W/st" && ok "neu angemeldet: $(q 'd["message"]' <"$W/st")" || bad "neu anmelden" "$code $(cat "$W/st")"
[ "$(tr '\n' '|' <"$W/smp.log")" = "uninstall PPSA86920|scan reset_attempts=true|" ] \
    && ok "ShadowMountPlus: erst uninstall, dann scan" || bad "Reihenfolge" "$(cat "$W/smp.log")"
curl -s "$U/api/tile" >"$W/t"
[ "$(q 'd["registered"]' <"$W/t")" = 02.005.007 ] && ok "danach als 02.005.007 angemeldet" || bad "danach" "$(cat "$W/t")"

# ---------------------------------------------------------------- laufendes Spiel / geöffnete App
mkdir -p "$W/sandbox/PPSA01325_000"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d '{}' "$U/api/tile")
[ "$code" = 409 ] && grep -q PPSA01325 "$W/body" && ok "Spiel läuft: abgelehnt" || bad "Spiel läuft" "$code $(cat "$W/body")"
rmdir "$W/sandbox/PPSA01325_000"
smp "[{\"title_id\":\"PPSA86920\",\"path\":\"$A\",\"version\":\"02.005.006\",\"busy\":true}]"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d '{}' "$U/api/tile")
[ "$code" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'noch geöffnet' "$W/st" \
    && ! grep -q scan "$W/smp.log" && ok "App geöffnet (ShadowMountPlus 409): abgelehnt, kein Scan" \
    || bad "App geöffnet" "$code $(cat "$W/st") $(cat "$W/smp.log")"
smp "[{\"title_id\":\"PPSA86920\",\"path\":\"$A\",\"version\":\"02.005.006\"},
      {\"title_id\":\"PPSA18089\",\"path\":\"/x\",\"name\":\"Matchbox™ Driving Adventures\",\"mounted\":true}]"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d '{}' "$U/api/tile")
[ "$code" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'Matchbox™ Driving Adventures“ ist noch eingehängt' "$W/st" \
    && ! grep -q scan "$W/smp.log" && ok "Spiel eingehängt (runtime_prepared): Name genannt, kein Scan" \
    || bad "Spiel eingehängt" "$code $(cat "$W/st") $(cat "$W/smp.log")"

# ---------------------------------------------------------------- nicht in der Liste
smp "[]"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d '{}' "$U/api/tile")
[ "$code" = 202 ] && wait_job && grep -q '"ok":false' "$W/st" && grep -q 'PPSA86920' "$W/st" \
    && ok "App unbekannt: verständliche Meldung" || bad "App unbekannt" "$code $(cat "$W/st")"

[ $fails -eq 0 ] && echo "Alle Kachel-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
