#!/usr/bin/env bash
# Startet den Formatierer auf Linux/WSL mit zwei Image-Dateien als Test-"USB-Sticks".
#   test/run_server.sh          normal
#   SLOW=1 test/run_server.sh   verlangsamt, damit der Fortschritt sichtbar ist
set -e
cd "$(dirname "$0")/.."
B=${BUILD:-$HOME/usbfmt_test}
mkdir -p "$B"
bash test/host_build.sh "$B/usb_format_host"
[ -f "$B/stick0.img" ] || truncate -s 16G "$B/stick0.img"
[ -f "$B/stick1.img" ] || truncate -s 64G "$B/stick1.img"
export USBFMT_IMAGES="$B/stick0.img:$B/stick1.img"
[ -n "${SLOW:-}" ] && export USBFMT_SLOW=1
exec "$B/usb_format_host"
