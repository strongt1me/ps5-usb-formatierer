#!/usr/bin/env bash
# PS5-USB-Formatierer (windows/usb_formatierer.py), Modus "USB an der PS5": Image-Sicherung gegen den Testserver.
# Baut den Formatierer als Host-Programm, legt drei Abbilder als Sticks an (da0 exFAT mit Daten und da1 eingebunden,
# da2 frei), startet ihn auf 127.0.0.1:8090 und lässt windows/tests/ps5_image_flow_test.py das Programm bedienen.
# Ohne Bildschirm über xvfb-run. Braucht python3 mit tkinter.
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" || exit 1
gcc -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c || exit 1
mkdir -p "$W/mnt/usb0" "$W/mnt/usb1" "$W/data/usb_format" "$W/sandbox"
head -c $((3 * 1024 * 1024)) /dev/urandom >"$W/f1"
truncate -s 1G "$W/a.img"
"$W/mkfs_test" disk "$W/a.img" exfat $((1024 * 1024 * 1024)) 512 QUELLE 0 --mbr --dir=/a "--file=/a/eins.bin=$W/f1" \
    >/dev/null || exit 1
truncate -s 1G "$W/b.img"
truncate -s 2G "$W/c.img"
USBFMT_IMAGES="$W/a.img:$W/b.img:$W/c.img" USBFMT_MOUNTED="1,1,0" USBFMT_MOUNTROOT="$W/mnt" \
    USBFMT_DATA="$W/data/usb_format" USBFMT_MODELS="SanDisk Ultra|Kingston DT|Samsung T7" USBFMT_SANDBOX="$W/sandbox" \
    "$W/srv" >"$W/log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -s http://127.0.0.1:8090/api/info >/dev/null 2>&1 && break; sleep 0.1; done
RUN=(python3 ../windows/tests/ps5_image_flow_test.py "$W/sandbox")
if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null; then RUN=(xvfb-run -a "${RUN[@]}"); fi
"${RUN[@]}"
rc=$?
[ $rc = 0 ] || { echo "--- Server-Log:"; tail -n 30 "$W/log"; }
exit $rc
