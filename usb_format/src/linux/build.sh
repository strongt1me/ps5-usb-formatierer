#!/usr/bin/env bash
# Baut den Linux-Helfer usbfmt_linux (statisch: läuft auf jeder Linux-Verteilung ohne passende Bibliotheken).
#   src/linux/build.sh <ausgabe> [-DUSBFMT_TEST]   (aus usb_format/ heraus; -DUSBFMT_TEST: mit --image für Tests)
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${1:-usbfmt_linux}
shift || true
STATIC=-static
# ohne statische glibc (z. B. in manchen Entwicklungsumgebungen) dynamisch bauen
PROBE=$(mktemp)
echo 'int main(void){return 0;}' | ${CC:-gcc} -static -x c - -o "$PROBE" 2>/dev/null || STATIC=
rm -f "$PROBE"
# zstd (Image-Sicherung) ohne die strengen Warnungen
ZOBJ=$(mktemp --suffix=.o)
trap 'rm -f "$ZOBJ"' EXIT
${CC:-gcc} -O2 -w -DNDEBUG -isystem third_party/zstd/lib -c src/zstd_all.c -o "$ZOBJ"
${CC:-gcc} -O2 -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE \
    -include src/linux/linuxcompat.h -isystem third_party/zstd/lib "$@" \
    src/linux/usbfmt_linux.c src/pc/helper.c src/mkfs.c src/hfsplus.c src/upcase.c src/devtest.c src/vollabel.c \
    src/fsdetect.c src/fsread.c src/extfs.c src/i18n.c src/i18n_table.c src/sha256.c src/imgbak.c "$ZOBJ" \
    $STATIC -pthread -o "$OUT"
echo "Fertig: $OUT ${STATIC:+(statisch)}"
