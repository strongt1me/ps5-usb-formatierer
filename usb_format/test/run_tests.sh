#!/usr/bin/env bash
# Prueft den Formatierungskern gegen fsck.exfat / fsck.fat / mtools (Linux bzw. WSL).
# Werkzeuge ohne root: apt-get download exfatprogs dosfstools mtools && dpkg -x ... ~/fstools/root
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
export MTOOLS_SKIP_CHECK=1
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/geom_xml_test" test/geom_xml_test.c src/geom_xml.c || exit 1

fails=0
echo "== Gerätenamen aus GEOM =="
"$W/geom_xml_test" || fails=$((fails + 1))

ok()   { echo "OK      $1"; }
bad()  { echo "FEHLER  $1"; sed 's/^/        /' "$W/log"; fails=$((fails + 1)); }
run()  { local n=$1; shift; if "$@" >"$W/log" 2>&1; then ok "$n"; else bad "$n"; fi; }
size() { numfmt --from=iec "$1"; }
mk()   { local n=$1; shift; if "$W/mkfs_test" "$@" >"$W/log" 2>&1; then ok "$n: $(cat "$W/log")"; else bad "$n"; return 1; fi; }

echo "== Dateisystem allein (Partition) =="
for spec in "exfat 64M 512" "exfat 1G 512" "exfat 40G 512" "exfat 3T 512" "exfat 2G 4096" \
            "fat32 40M 512" "fat32 300M 512" "fat32 8G 512" "fat32 31G 512" "fat32 64G 512" \
            "fat32 1T 512" "fat32 2G 4096"; do
    set -- $spec
    img="$W/p.img"
    rm -f "$img"
    if ! mk "$1 $2 ss=$3" part "$img" "$1" "$(size "$2")" "$3" PS5USB; then continue; fi
    if [ "$1" = exfat ]; then
        run "  fsck.exfat" fsck.exfat -n -v "$img"
        run "  Label" sh -c "exfatlabel '$img' | grep -q PS5USB"
    else
        run "  fsck.fat" fsck.fat -n -v "$img"
        run "  Label" sh -c "fatlabel '$img' | grep -q PS5USB"
    fi
done

echo "== Freie Zuordnungseinheit (Bereich \"nicht für die PS5\") =="
for spec in "exfat 1G 512 512" "exfat 2G 512 4096" "exfat 2G 512 131072" "exfat 2G 512 1048576" \
            "exfat 2G 512 33554432" "exfat 40G 512 4096" "exfat 2G 4096 4096" \
            "fat32 2G 512 512" "fat32 8G 512 65536" "fat32 64G 512 4096" "fat32 2G 4096 8192"; do
    set -- $spec
    img="$W/c.img"
    rm -f "$img"
    if ! mk "$1 $2 ss=$3 cluster=$4" part "$img" "$1" "$(size "$2")" "$3" FREI "$4"; then continue; fi
    if [ "$1" = exfat ]; then
        run "  fsck.exfat" fsck.exfat -n "$img"
        run "  Clustergröße im Bootsektor = $4" sh -c "test \$(( (1 << \$(od -An -tu1 -j108 -N1 '$img')) * (1 << \$(od -An -tu1 -j109 -N1 '$img')) )) -eq $4"
    else
        run "  fsck.fat" fsck.fat -n "$img"
        run "  Clustergröße im Bootsektor = $4" sh -c "test \$(( \$(od -An -tu2 -j11 -N2 '$img') * \$(od -An -tu1 -j13 -N1 '$img') )) -eq $4"
    fi
done
for spec in "fat32 1G 512 32768" "fat32 1T 512 2048" "exfat 1G 512 3000" "exfat 1G 4096 2048" "fat32 1G 512 131072"; do
    set -- $spec
    if "$W/mkfs_test" part "$W/c.img" "$1" "$(size "$2")" "$3" "" "$4" >"$W/log" 2>&1; then
        bad "$1 $2 cluster=$4 hätte abgelehnt werden müssen"
    else
        ok "$1 $2 cluster=$4 abgelehnt: $(cat "$W/log")"
    fi
done

echo "== Zu klein / ungueltig (muss fehlschlagen) =="
for spec in "fat32 20M 512" "exfat 4M 512"; do
    set -- $spec
    if "$W/mkfs_test" disk "$W/s.img" "$1" "$(size "$2")" "$3" >"$W/log" 2>&1; then
        bad "$1 $2 haette abgelehnt werden muessen"
    else
        ok "$1 $2 abgelehnt: $(cat "$W/log")"
    fi
done
if "$W/mkfs_test" disk "$W/s.img" exfat "$(size 1G)" 512 "VIEL_ZU_LANG_X" >"$W/log" 2>&1; then
    bad "zu langes Label haette abgelehnt werden muessen"
else
    ok "zu langes Label abgelehnt"
fi

echo "== Komplettes Laufwerk mit MBR =="
for fs in exfat fat32; do
    img="$W/d.img"
    rm -f "$img"
    if ! mk "$fs 512M" disk "$img" $fs "$(size 512M)" 512 PS5USB; then continue; fi
    want=$([ $fs = exfat ] && echo 7 || echo c)
    run "  MBR: Start 2048, Typ $want" sh -c "sfdisk -d '$img' | grep -Eq 'start= *2048, size= *1046528, type=$want\$'"
    dd if="$img" of="$W/part.img" bs=1M skip=1 status=none
    if [ $fs = exfat ]; then
        run "  fsck.exfat (Partition)" fsck.exfat -n "$W/part.img"
    else
        run "  fsck.fat (Partition)" fsck.fat -n "$W/part.img"
        head -c 3000000 /dev/urandom >"$W/datei.bin"
        run "  mtools: Datei + Ordner anlegen" sh -c "mcopy -i '$img@@1M' '$W/datei.bin' ::/DATEI.BIN && mmd -i '$img@@1M' ::/PS5"
        run "  mtools: Datei zuruecklesen" sh -c "mcopy -n -i '$img@@1M' ::/DATEI.BIN '$W/zurueck.bin' && cmp '$W/datei.bin' '$W/zurueck.bin'"
        dd if="$img" of="$W/part.img" bs=1M skip=1 status=none
        run "  fsck.fat nach Schreiben" fsck.fat -n "$W/part.img"
    fi
done

echo
if [ $fails -eq 0 ]; then echo "Alle Tests bestanden."; else echo "$fails Test(s) fehlgeschlagen."; fi
exit $((fails > 0))
