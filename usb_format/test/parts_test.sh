#!/bin/bash
# HFS+ (eigene Umsetzung) und Expertenmodus (mehrere Partitionen) - geprüft mit fsck.hfsplus (hfsprogs),
# fsck.exfat, fsck.fat und sfdisk. Werkzeuge wie die übrigen Tests aus ~/fstools/root.
cd "$(dirname "$0")/.." || exit 1
T=$HOME/fstools/root
export PATH="$T/usr/sbin:$T/sbin:$T/usr/bin:$PATH"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
MIB=$((1024 * 1024))
fail=0
run() { local name=$1; shift; if "$@" >"$W/out" 2>&1; then echo "OK      $name"; else echo "FEHLER  $name"; tail -15 "$W/out"; fail=1; fi; }
expect_fail() { local name=$1; shift; if "$@" >"$W/out" 2>&1; then echo "FEHLER  $name (hätte abgelehnt werden müssen)"; fail=1; else echo "OK      $name: $(tail -1 "$W/out")"; fi; }

gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/parts_test" test/parts_test.c src/mkfs.c src/hfsplus.c src/upcase.c || exit 1

# Partition <nr> aus <image> nach $W/p.img holen (Sektorgröße ss)
extract() { local img=$1 nr=$2 ss=$3; read -r s c < <(awk -v n="$nr" '$1=="P" && $2==n {print $3, $4}' "$W/layout.txt")
            dd if="$img" of="$W/p.img" bs=$ss skip="$s" count="$c" status=none; }
hfsck() { fsck.hfsplus -fn "$1" && ! grep -qiE "invalid|error|incorrect" "$W/out"; }

echo "== HFS+ allein (eine Partition, wie Standard) =="
for spec in "64 512 4096 KLEIN" "1024 512 4096 MAC DATEN" "1024 512 512 Bloecke512" "1024 512 65536 Bloecke64K" \
            "1024 4096 4096 Sektor4K" "3000 512 8192 Ungerade"; do
    read -r mb ss bs label <<<"$spec"
    img="$W/hfs.img"
    bytes=$((mb * MIB + 7 * 512))   # absichtlich krumme Größe
    run "HFS+ $mb MB, Sektor $ss, Block $bs: anlegen" "$W/mkfs_test" disk "$img" hfsplus $bytes "$ss" "$label" "$bs"
    s=$(sfdisk -d "$img" 2>/dev/null | grep -o 'start= *[0-9]*' | head -1 | tr -dc 0-9)
    c=$(sfdisk -d "$img" 2>/dev/null | grep -o 'size= *[0-9]*' | head -1 | tr -dc 0-9)
    t=$(sfdisk -d "$img" 2>/dev/null | grep -o 'type=[0-9a-fA-F-]*' | head -1)
    run "  Partitionstyp HFS+ ($t)" sh -c "test '$t' = 'type=af'"
    dd if="$img" of="$W/p.img" bs="$ss" skip="$s" count="$c" status=none
    run "  fsck.hfsplus" sh -c "fsck.hfsplus -fn '$W/p.img' > '$W/fsck.txt' 2>&1; rc=\$?; cat '$W/fsck.txt'; [ \$rc = 0 ] && grep -q 'appears to be OK' '$W/fsck.txt'"
done
mkfs.hfsplus -v "Vergleich" "$W/ref.img" >/dev/null 2>&1 || true

echo "== Mehrere Partitionen (GPT) =="
img="$W/multi.img"
run "GPT: exFAT + FAT32 + HFS+ + leer + exFAT-Rest" sh -c "'$W/parts_test' '$img' $((2048 * MIB)) 512 gpt \
    exfat:300M:EINS:32768 fat32:400M:ZWEI:4096 hfsplus:500M:Mac\ Daten:4096 none:100M exfat:rest:REST > '$W/layout.txt'"
cat "$W/layout.txt"
run "  sfdisk: 5 Partitionen, Typen" sh -c "sfdisk -d '$img' | tee '$W/sf.txt' | grep -c '^/' | grep -qx 5 &&
    grep -q 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7' '$W/sf.txt' && grep -q '48465300-0000-11AA-AA11-00306543ECAC' '$W/sf.txt'"
run "  sfdisk --verify" sfdisk --verify "$img"
extract "$img" 1 512; run "  1 exFAT fsck" fsck.exfat -n "$W/p.img"
extract "$img" 2 512; run "  2 FAT32 fsck" fsck.fat -n "$W/p.img"
extract "$img" 3 512; run "  3 HFS+ fsck" sh -c "fsck.hfsplus -fn '$W/p.img' 2>&1 | tee /dev/stderr | grep -q 'appears to be OK'"
extract "$img" 4 512; run "  4 leer (ohne Signatur)" sh -c "test -z \"\$(head -c $MIB '$W/p.img' | tr -d '\\0' | head -c1)\""
extract "$img" 5 512; run "  5 exFAT-Rest fsck" fsck.exfat -n "$W/p.img"
run "  Partitionen an MiB-Grenzen" sh -c "awk '\$1==\"P\" && \$3 % 2048 {exit 1}' '$W/layout.txt'"

echo "== MBR =="
img="$W/mbr.img"
run "MBR: 4 Partitionen, Typen, aktiv" sh -c "'$W/parts_test' '$img' $((1024 * MIB)) 512 mbr \
    fat32:200M:BOOT:0:efi:aktiv ext4:300M:LINUX hfsplus:200M:MAC exfat:rest:DATEN > '$W/layout.txt'"
run "  sfdisk: Typen ef/83/af/7, Boot-Kennung" sh -c "sfdisk -d '$img' | tee '$W/sf.txt' >/dev/null &&
    grep -q 'type=ef, bootable' '$W/sf.txt' && grep -q 'type=83' '$W/sf.txt' && grep -q 'type=af' '$W/sf.txt' && grep -q 'type=7' '$W/sf.txt'"
extract "$img" 1 512; run "  1 FAT32 fsck" fsck.fat -n "$W/p.img"
extract "$img" 3 512; run "  3 HFS+ fsck" sh -c "fsck.hfsplus -fn '$W/p.img' 2>&1 | grep -q 'appears to be OK'"
expect_fail "MBR mit 5 Partitionen" "$W/parts_test" "$img" $((1024 * MIB)) 512 mbr exfat:100M exfat:100M exfat:100M exfat:100M exfat:rest
expect_fail "zu groß" "$W/parts_test" "$img" $((1024 * MIB)) 512 gpt exfat:900M exfat:200M
expect_fail "Rest nicht zuletzt" "$W/parts_test" "$img" $((1024 * MIB)) 512 gpt exfat:rest exfat:100M

echo "== 4K-Sektoren, GPT =="
img="$W/4k.img"
run "4K: HFS+ + exFAT" sh -c "'$W/parts_test' '$img' $((1024 * MIB)) 4096 gpt hfsplus:400M:VIERK exfat:rest:REST > '$W/layout.txt'"
extract "$img" 1 4096; run "  1 HFS+ fsck (4K)" sh -c "fsck.hfsplus -fn '$W/p.img' 2>&1 | grep -q 'appears to be OK'"

[ $fail = 0 ] && echo "Alle Partitions-Tests bestanden." || { echo "Partitions-Tests: Fehler"; exit 1; }
