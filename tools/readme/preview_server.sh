#!/usr/bin/env bash
# Lokaler Testserver auf Port 8090 mit nachgebauten Laufwerken für Vorschau-Bilder:
#  da0 "Realtek RTL9210C NVME" eingebunden, mit Spielen, PKG und einem Duplikat
#  da1 "VendorCo ProductCode" frei (No-Name-Stick)
set -e
P="$(cd "$(dirname "$0")/../../usb_format" && pwd)"
W=~/usbfmt_preview
mkdir -p "$W"
pkill -f usbfmt_preview/srv || true
cd "$P"
bash test/host_build.sh "$W/srv"
gcc -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c
rm -rf "$W/mnt" "$W/data"
mkdir -p "$W/mnt/usb0/homebrew" "$W/mnt/usb0/etaHEN/games" "$W/mnt/usb0/pkg"
game() {  # game <ordner> <id> <name>
    mkdir -p "$W/mnt/usb0/homebrew/$1/sce_sys"
    printf '{"titleId":"%s","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"%s"}}}' "$2" "$3" \
        >"$W/mnt/usb0/homebrew/$1/sce_sys/param.json"
}
game Astro PPSA01325 "Astro Bot"
game Crash PPSA02433 "Crash Bandicoot 4: It's About Time"
game Arkanoid PPSA06328 "Arkanoid Eternal Battle"
game GT7 PPSA01316 "Gran Turismo 7"
game Returnal PPSA01284 "Returnal"
printf '{"titleId":"PPSA03117","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Crazy Chicken Shooter"}}}' >"$W/cc.json"
"$W/mkfs_test" part "$W/mnt/usb0/homebrew/Crazy Chicken Shooter Bundle.exfat" exfat $((64*1048576)) 512 "" 4096 --file=sce_sys/param.json="$W/cc.json" >/dev/null
cp "$W/mnt/usb0/homebrew/Crazy Chicken Shooter Bundle.exfat" "$W/mnt/usb0/homebrew/Crazy Chicken Shooter.exfat"
for i in 1 2 3 4; do head -c 1000 /dev/zero >"$W/mnt/usb0/pkg/update$i.pkg"; done
[ -f "$W/ssd.img" ] || truncate -s 476G "$W/ssd.img"
"$W/mkfs_test" disk "$W/ssd.img" exfat $(stat -c %s "$W/ssd.img") 512 PS5SSD >/dev/null
[ -f "$W/udisk.img" ] || truncate -s 133169152000 "$W/udisk.img"
"$W/mkfs_test" disk "$W/udisk.img" fat32 $(stat -c %s "$W/udisk.img") 512 TEST1 >/dev/null
cd "$W"
pkill -f smp_mock.py || true
H="$W/mnt/usb0/homebrew"
printf '[{"title_id":"PPSA01325","path":"%s/Astro"},{"title_id":"PPSA02433","path":"%s/Crash"},{"title_id":"PPSA06328","path":"%s/Arkanoid"},{"title_id":"PPSA01316","path":"%s/GT7"},{"title_id":"PPSA01284","path":"%s/Returnal"},{"title_id":"PPSA03117","path":"%s/Crazy Chicken Shooter.exfat"}]' \
    "$H" "$H" "$H" "$H" "$H" "$H" >"$W/games.json"
rm -f "$W/smp.log"
nohup python3 "$P/test/smp_mock.py" 18097 "$W/games.json" "$W/smp.log" >/dev/null 2>&1 &
USBFMT_IMAGES="$W/ssd.img:$W/udisk.img" USBFMT_MODELS="Realtek RTL9210C NVME|General UDisk" \
USBFMT_SMP_PORT=18097 \
USBFMT_MOUNTED="1,1" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/data" nohup "$W/srv" >"$W/log" 2>&1 &
sleep 1
echo "Testserver läuft"
