#!/usr/bin/env bash
# Belastungstest der HTTP-API unter ASAN/UBSAN (siehe api_fuzz.py): zufällige und bösartige Anfragen an alle Routen,
# dazu eine ShadowMountPlus-Attrappe, die jede zweite Antwort verstümmelt. Erwartet: keine Antwort bleibt aus, kein
# Sanitizer-Fund, der Server läuft am Ende noch.
# Aufruf: test/api_fuzz.sh [Anfragen, Standard 500] [Seed]   (Ports 18160/18161)
set -u
cd "$(dirname "$0")/.."
N=${1:-500}
SEED=${2:-1}
W=$(mktemp -d)
PID= MPID=
trap 'kill $PID $MPID 2>/dev/null; rm -rf "$W"' EXIT
HOST_CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g" \
    bash test/host_build.sh "$W/srv" -DSERVER_PORT=18160 || exit 1
H="$W/mnt/usb0/homebrew"
mkdir -p "$H/Spiel/sce_sys" "$W/saves" "$W/sandbox" "$W/san"
printf '{"titleId":"PPSA00011"}' >"$H/Spiel/sce_sys/param.json"
printf '\x89PNG\r\n\x1a\nBILD' >"$H/Spiel/sce_sys/icon0.png"
head -c 100000 /dev/urandom >"$H/x.ffpfs"
truncate -s 64M "$W/a.img" "$W/b.img"
printf '[{"title_id":"PPSA00011","path":"%s/Spiel","icon":true}]' "$H" >"$W/games.json"
printf '[{"source":"/dev/da0s1","mount_point":"%s/mnt/usb0","ejectable":true,"mount_id":"00112233aabbccdd"}]' "$W" >"$W/mounts.json"
SMP_MOCK_GARBAGE=1 python3 test/smp_mock.py 18161 "$W/games.json" "$W/smp.log" "$W/mounts.json" &
MPID=$!
ASAN_OPTIONS="log_path=$W/san/asan:detect_leaks=0" UBSAN_OPTIONS="log_path=$W/san/ubsan:print_stacktrace=1" \
    USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_MOUNTED="1,0" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/cfg" \
    USBFMT_SAVES="$W/saves" USBFMT_SANDBOX="$W/sandbox" USBFMT_SMP_PORT=18161 USBFMT_FW="12.00" "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do curl -s http://127.0.0.1:18160/api/info >/dev/null 2>&1 && break; sleep 0.1; done
python3 test/api_fuzz.py 18160 "$N" "$SEED"
rc=$?
if ls "$W"/san/* >/dev/null 2>&1; then
    echo "FEHLER  Sanitizer-Fund:"; cat "$W"/san/*; rc=1
fi
kill -0 $PID 2>/dev/null || { echo "FEHLER  Server beendet"; tail -20 "$W/log"; rc=1; }
[ $rc = 0 ] && echo "API-Belastungstest bestanden." || echo "API-Belastungstest: FEHLER"
exit $rc
