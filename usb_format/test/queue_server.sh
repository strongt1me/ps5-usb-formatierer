#!/usr/bin/env bash
# Testserver für windows/tests/gui_queue_test.py: da0 mit den Ordner-Spielen Alpha und Beta, da1 leer (Port 18129).
# Aufruf: test/queue_server.sh <leerer-ordner>     (der Server läuft danach im Hintergrund; beenden: pkill -x queue_srv)
set -eu
cd "$(dirname "$0")/.."
B=$1
rm -rf "$B"; mkdir -p "$B/mnt/usb0/homebrew" "$B/mnt/usb1"
bash test/host_build.sh "$B/queue_srv" -DSERVER_PORT=18129
for n in Alpha Beta; do
    mkdir -p "$B/mnt/usb0/homebrew/$n/sce_sys"
    printf '{"titleId":"PPSA0000%s","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"%s"}}}' "$([ $n = Alpha ] && echo 1 || echo 2)" "$n" >"$B/mnt/usb0/homebrew/$n/sce_sys/param.json"
    head -c 2000000 /dev/urandom >"$B/mnt/usb0/homebrew/$n/data.bin"
done
truncate -s 64M "$B/a.img" "$B/b.img"
USBFMT_IMAGES="$B/a.img:$B/b.img" USBFMT_MOUNTED=1,1 USBFMT_MOUNTROOT="$B/mnt" USBFMT_DATA="$B/cfg" setsid "$B/queue_srv" >"$B/log" 2>&1 &
sleep 1
curl -s http://127.0.0.1:18129/api/info
