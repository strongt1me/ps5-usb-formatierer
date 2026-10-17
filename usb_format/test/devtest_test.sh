#!/usr/bin/env bash
# Laufwerksprüfungen (src/devtest.c): echter Stick, gefälschte Sticks (Adresse läuft über / Daten verschwinden),
# vollständiges Überschreiben.
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gcc -Wall -Wextra -Werror -O2 -DUSBFMT_HOST -o "$W/dt" test/devtest_test.c src/devtest.c src/i18n.c src/i18n_table.c || exit 1

fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; sed 's/^/        /' "$W/out"; fails=$((fails + 1)); }
val() { sed -n "s/.*$1=\([0-9]*\).*/\1/p" "$W/out"; }
MIB=1048576

truncate -s 1G "$W/echt.img"
"$W/dt" verify "$W/echt.img" $((1024 * MIB)) >"$W/out" 2>&1
[ "$(val fake)" = 0 ] && [ "$(val bad)" = 0 ] && [ "$(val write)" -gt 0 ] && [ "$(val read)" -gt 0 ] \
    && ok "echter Stick: $(cat "$W/out")" || bad "echter Stick"

# Schnelltest (Stichproben): Überlauf an einer Zweierpotenz (typisch) muss er genau treffen, verschwindende Daten
# auf 1 MB genau, nie zu groß. Vollständig (jeder Block) findet jede Grenze genau.
expect_real() {   # <Name> <erwartete MB> <Toleranz MB nach unten>
    local r; r=$(val real)
    if [ "$(val fake)" = 1 ] && [ -n "$r" ] && [ $((r / MIB)) -le $2 ] && [ $((r / MIB)) -ge $(($2 - $3)) ]; then
        ok "$1: erkannt, echt $((r / MIB)) MB"
    else
        bad "$1"
    fi
}
for real in 256 512; do
    truncate -s ${real}M "$W/fake.img"
    USBFMT_FAKE_REAL=$((real * MIB)) "$W/dt" verify "$W/fake.img" $((2048 * MIB)) >"$W/out" 2>&1
    expect_real "Schnelltest, Adresse läuft bei $real MB über" $real 0
    rm -f "$W/fake.img"
done
truncate -s 256M "$W/drop.img"
USBFMT_FAKE_REAL=$((256 * MIB)) USBFMT_FAKE_MODE=drop "$W/dt" verify "$W/drop.img" $((2048 * MIB)) >"$W/out" 2>&1
expect_real "Schnelltest, Daten hinter 256 MB verschwinden" 256 1
rm -f "$W/drop.img"
# echte Größe kleiner als das Stichproben-Raster (128 GB gemeldet: Raster 1 GB) - darf nicht das Raster melden
truncate -s 512M "$W/fake.img"
USBFMT_FAKE_REAL=$((512 * MIB)) "$W/dt" verify "$W/fake.img" $((131072 * MIB)) >"$W/out" 2>&1
expect_real "Schnelltest, 128 GB gemeldet, Überlauf bei 512 MB (kleiner als das Raster)" 512 0
rm -f "$W/fake.img"
# Grenze weit zwischen zwei Rasterproben (64 GB gemeldet: Raster 512 MB) - nie größer als die echte Größe
truncate -s 8448M "$W/drop.img"
USBFMT_FAKE_REAL=$((8448 * MIB)) USBFMT_FAKE_MODE=drop "$W/dt" verify "$W/drop.img" $((65536 * MIB)) >"$W/out" 2>&1
expect_real "Schnelltest, 64 GB gemeldet, Daten hinter 8448 MB verschwinden" 8448 1
rm -f "$W/drop.img"

# Stick wird mitten in der Prüfung abgezogen: Fehler "nicht mehr erreichbar", NIE "gefälscht" (würde gespeichert)
truncate -s 2G "$W/weg.img"
for mode in verify full; do
    USBFMT_GONE_AT=$((1200 * MIB)) "$W/dt" $mode "$W/weg.img" $((2048 * MIB)) >"$W/out" 2>&1
    grep -q "nicht mehr erreichbar" "$W/out" && ! grep -q "fake=1" "$W/out" \
        && ok "abgezogen während der Prüfung ($mode): Fehler statt Fälschung" || bad "abgezogen ($mode)"
done
rm -f "$W/weg.img"

truncate -s 300M "$W/fake.img"
USBFMT_FAKE_REAL=$((300 * MIB)) "$W/dt" full "$W/fake.img" $((1024 * MIB)) >"$W/out" 2>&1
expect_real "Vollständig, Adresse läuft bei 300 MB über (keine Zweierpotenz)" 300 0
rm -f "$W/fake.img"
truncate -s 300M "$W/drop.img"
USBFMT_FAKE_REAL=$((300 * MIB)) USBFMT_FAKE_MODE=drop "$W/dt" full "$W/drop.img" $((1024 * MIB)) >"$W/out" 2>&1
expect_real "Vollständig, Daten hinter 300 MB verschwinden" 300 0
rm -f "$W/drop.img"
# Fälschung, die hinter der echten Grenze mit Ein-/Ausgabefehlern antwortet: gefälscht, nicht "defekt"
for mode in verify full; do
    truncate -s 300M "$W/eio.img"
    USBFMT_FAKE_REAL=$((300 * MIB)) USBFMT_FAKE_MODE=eio "$W/dt" $mode "$W/eio.img" $((1024 * MIB)) >"$W/out" 2>&1
    expect_real "$mode, hinter 300 MB Ein-/Ausgabefehler" 300 1
    rm -f "$W/eio.img"
done
# toter Stick (nimmt nichts an, liest nur Nullen): defekt ab dem Anfang, nie "gefälscht, echt 0 Bytes"
truncate -s 64M "$W/tot.img"
for mode in verify full; do
    USBFMT_FAKE_MODE=dead "$W/dt" $mode "$W/tot.img" $((64 * MIB)) >"$W/out" 2>&1
    [ "$(val fake)" = 0 ] && [ "$(val bad)" = 1 ] && ok "toter Stick ($mode): defekt, nicht gefälscht" || bad "toter Stick ($mode)"
done
rm -f "$W/tot.img"

head -c $((64 * MIB)) /dev/urandom >"$W/voll.img"
cp "$W/voll.img" "$W/vorher.img"
"$W/dt" full "$W/voll.img" $((64 * MIB)) >"$W/out" 2>&1
[ "$(val bad)" = 0 ] && [ "$(val fake)" = 0 ] && ! cmp -s "$W/voll.img" "$W/vorher.img" \
    && ok "vollständig beschrieben und geprüft (echter Stick): $(cat "$W/out")" || bad "vollständig, echter Stick"

"$W/dt" read "$W/echt.img" $((1024 * MIB)) >"$W/out" 2>&1
[ "$(val read)" -gt 0 ] && ok "Lesetempo: $(cat "$W/out")" || bad "Lesetempo"

echo
[ $fails -eq 0 ] && echo "Alle Prüf-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
