#!/usr/bin/env bash
# Ordner-Vorlage, Dateien, GPT und Größenbegrenzung (gefälschte Sticks) des Formatierers sowie das Lesemodul
# fsread (exFAT/UFS2) - geprüft mit fsck.exfat, fsck.fat, mtools, sgdisk und makefs.
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
export MTOOLS_SKIP_CHECK=1
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/upcase.c || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/fsread_test" test/fsread_test.c src/fsread.c || exit 1

fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; sed 's/^/        /' "$W/log" 2>/dev/null; fails=$((fails + 1)); }
run() { local n=$1; shift; if "$@" >"$W/log" 2>&1; then ok "$n"; else bad "$n"; fi; }
mk()  { local n=$1; shift; if "$W/mkfs_test" "$@" >"$W/log" 2>&1; then ok "$n: $(cat "$W/log")"; else bad "$n"; return 1; fi; }
part() { dd if="$1" of="$W/part.img" bs=1M skip=1 status=none; }   # Partition ab 1 MiB herauslösen
MIB=1048576
VORLAGE=(--dir=homebrew --dir=etaHEN/games --dir=pkg)

echo "== Ordner-Vorlage exFAT =="
for cl in "" 4096 512; do
    img="$W/e.img"
    mk "exFAT 1G Cluster ${cl:-64K} mit Vorlage" disk "$img" exfat $((1024 * MIB)) 512 PS5USB ${cl:-0} "${VORLAGE[@]}" || continue
    part "$img"
    run "  fsck.exfat" fsck.exfat -n "$W/part.img"
    for d in homebrew etaHEN etaHEN/games pkg ETAHEN/GAMES; do
        run "  Ordner $d" "$W/fsread_test" "$img" $MIB isdir "$d"
    done
    run "  Bezeichnung PS5USB" sh -c "test \"\$('$W/fsread_test' '$img' $MIB label)\" = PS5USB"
done

echo "== Ordner-Vorlage FAT32 =="
img="$W/f.img"
if mk "FAT32 1G mit Vorlage" disk "$img" fat32 $((1024 * MIB)) 512 PS5USB 0 "${VORLAGE[@]}"; then
    part "$img"
    run "  fsck.fat" fsck.fat -n "$W/part.img"
    run "  mdir zeigt etaHEN (langer Name)" sh -c "mdir -i '$img@@1M' ::/ | grep -q 'etaHEN'"
    run "  mdir etaHEN/games" mdir -i "$img@@1M" ::/etaHEN/games
    run "  mdir homebrew, pkg" sh -c "mdir -i '$img@@1M' ::/homebrew && mdir -i '$img@@1M' ::/pkg"
    head -c 200000 /dev/urandom >"$W/rnd.bin"
    run "  Datei in etaHEN/games schreiben und lesen" sh -c "mcopy -i '$img@@1M' '$W/rnd.bin' ::/etaHEN/games/X.BIN && \
        mcopy -n -i '$img@@1M' ::/etaHEN/games/X.BIN '$W/back.bin' && cmp '$W/rnd.bin' '$W/back.bin'"
    part "$img"
    run "  fsck.fat nach Schreiben" fsck.fat -n "$W/part.img"
fi

echo "== Dateien im exFAT (Spiel-Abbild für die Bibliothek) =="
printf '{"titleId":"PPSA01234","localizedParameters":{"defaultLanguage":"de-DE","de-DE":{"titleName":"Testspiel"}}}' >"$W/param.json"
head -c 300000 /dev/urandom >"$W/gross.bin"
: >"$W/leer.bin"
img="$W/game.exfat"
if mk "exFAT-Abbild mit Spiel" part "$img" exfat $((64 * MIB)) 512 "" 4096 \
       --file=sce_sys/param.json="$W/param.json" --file=eboot.bin="$W/gross.bin" --file=sce_sys/leer.dat="$W/leer.bin"; then
    run "  fsck.exfat" fsck.exfat -n "$img"
    run "  param.json lesen" sh -c "'$W/fsread_test' '$img' 0 cat sce_sys/param.json | cmp - '$W/param.json'"
    run "  300-KB-Datei über mehrere Cluster" sh -c "'$W/fsread_test' '$img' 0 cat eboot.bin | cmp - '$W/gross.bin'"
    run "  leere Datei" sh -c "test \$('$W/fsread_test' '$img' 0 cat sce_sys/leer.dat | wc -c) -eq 0"
    run "  Groß-/Kleinschreibung egal" "$W/fsread_test" "$img" 0 cat SCE_SYS/PARAM.JSON
    if "$W/fsread_test" "$img" 0 cat sce_sys/fehlt.json >"$W/log" 2>&1; then bad "  fehlende Datei gefunden"; else ok "  fehlende Datei: nicht gefunden"; fi
fi

echo "== UFS2-Abbild (.ffpkg) =="
mkdir -p "$W/ufs/sce_sys"
cp "$W/param.json" "$W/ufs/sce_sys/param.json"
head -c 100000 /dev/urandom >"$W/ufs/eboot.bin"
if makefs -t ffs -o version=2,bsize=65536,fsize=8192 -s 16m "$W/game.ffpkg" "$W/ufs" >"$W/log" 2>&1 &&
   [ -s "$W/game.ffpkg" ]; then
    run "  als UFS2 erkannt" sh -c "test \"\$('$W/fsread_test' '$W/game.ffpkg' 0 type)\" = UFS2"
    run "  param.json lesen" sh -c "'$W/fsread_test' '$W/game.ffpkg' 0 cat sce_sys/param.json | cmp - '$W/param.json'"
    run "  eboot.bin lesen" sh -c "'$W/fsread_test' '$W/game.ffpkg' 0 cat eboot.bin | cmp - '$W/ufs/eboot.bin'"
    run "  sce_sys ist Ordner" "$W/fsread_test" "$W/game.ffpkg" 0 isdir sce_sys
else
    bad "makefs"
fi

echo "== GPT =="
for fs in exfat fat32; do
    img="$W/g.img"
    mk "$fs 2G GPT" disk "$img" $fs $((2048 * MIB)) 512 PS5USB 0 --gpt "${VORLAGE[@]}" || continue
    run "  sgdisk -v ohne Befund" sh -c "sgdisk -v '$img' | grep -q 'No problems found'"
    run "  Partition 1 ab Sektor 2048, Typ 0700" sh -c "sgdisk -i 1 '$img' | grep -q 'First sector: 2048 ' && sgdisk -i 1 '$img' | grep -q 'Microsoft basic data'"
    part "$img"
    if [ $fs = exfat ]; then run "  fsck.exfat" fsck.exfat -n "$W/part.img"; else run "  fsck.fat" fsck.fat -n "$W/part.img"; fi
done
img="$W/g4k.img"
if mk "exFAT 2G GPT 4K-Sektoren" disk "$img" exfat $((2048 * MIB)) 4096 "" 0 --gpt; then
    run "  sgdisk -v (4K)" sh -c "sgdisk -v '$img' 2>&1 | grep -q 'No problems found' || sgdisk -v '$img'"
    part "$img"
    run "  fsck.exfat (4K)" fsck.exfat -n "$W/part.img"
fi
img="$W/auto.img"
truncate -s 3T "$img"
if mk "exFAT 3 TB automatisch -> GPT" disk "$img" exfat $((3 * 1024 * 1024 * MIB)) 512 PS5USB; then
    cp "$W/log" "$W/mklog"
    run "  Ausgabe meldet GPT" grep -q GPT "$W/mklog"
    run "  sgdisk -v (3 TB)" sh -c "sgdisk -v '$img' | grep -q 'No problems found'"
fi
rm -f "$img"

echo "== Größenbegrenzung (gefälschter Stick) =="
img="$W/l.img"
if mk "exFAT 1G, nur 256 MB nutzen" disk "$img" exfat $((1024 * MIB)) 512 FAKE 0 --limit=$((256 * MIB)); then
    run "  MBR endet bei 256 MB" sh -c "sfdisk -d '$img' | grep -Eq 'start= *2048, size= *522240, type=7\$'"
    dd if="$img" of="$W/part.img" bs=1M skip=1 count=255 status=none
    run "  fsck.exfat" fsck.exfat -n "$W/part.img"
    run "  hinter 256 MB unberührt" sh -c "test -z \"\$(dd if='$img' bs=1M skip=256 status=none | tr -d '\\0' | head -c1)\""
fi

echo "== Bezeichnung ändern ohne Formatieren =="
gcc -Wall -Wextra -Werror -O2 -o "$W/label_test" test/label_test.c src/vollabel.c src/fsdetect.c src/fsread.c \
    src/mkfs.c src/upcase.c || exit 1
SZ=$((512 * MIB))
lab() { "$W/label_test" get "$1" $SZ | sed -n 's/.*bezeichnung=//p'; }
for fs in exfat fat32; do
    for scheme in --mbr --gpt; do
        img="$W/lab.img"
        "$W/mkfs_test" disk "$img" $fs $SZ 512 ALT 0 $scheme "${VORLAGE[@]}" >/dev/null || { bad "$fs $scheme anlegen"; continue; }
        run "  $fs $scheme: erkannt (Bezeichnung ALT)" sh -c "'$W/label_test' get '$img' $SZ | grep -q 'bezeichnung=ALT\$'"
        run "  $fs $scheme: umbenennen in Neu_Name-1" "$W/label_test" set "$img" $SZ "Neu_Name-1"
        want=$([ $fs = fat32 ] && echo NEU_NAME-1 || echo Neu_Name-1)
        [ "$(lab "$img")" = "$want" ] && ok "  $fs $scheme: liest $want" || { echo "ist: $(lab "$img")" >"$W/log"; bad "  $fs $scheme: Bezeichnung"; }
        part "$img"
        if [ $fs = exfat ]; then
            run "    exfatlabel bestätigt" sh -c "exfatlabel '$W/part.img' | grep -q 'Neu_Name-1'"
            run "    fsck.exfat sauber" fsck.exfat -n "$W/part.img"
        else
            run "    fatlabel bestätigt" sh -c "fatlabel '$W/part.img' | grep -q 'NEU_NAME-1'"
            run "    fsck.fat sauber" fsck.fat -n "$W/part.img"
        fi
        run "  $fs $scheme: Bezeichnung entfernen" "$W/label_test" set "$img" $SZ ""
        [ -z "$(lab "$img")" ] && ok "  $fs $scheme: leer" || bad "  $fs $scheme: nicht entfernt"
        run "  $fs $scheme: wieder setzen (neuer Eintrag)" "$W/label_test" set "$img" $SZ "WIEDER"
        [ "$(lab "$img")" = WIEDER ] && ok "  $fs $scheme: WIEDER" || bad "  $fs $scheme: WIEDER"
        part "$img"
        if [ $fs = exfat ]; then run "    fsck.exfat sauber" fsck.exfat -n "$W/part.img"; else run "    fsck.fat sauber" fsck.fat -n "$W/part.img"; fi
    done
done
"$W/mkfs_test" disk "$W/ohne.img" exfat $SZ 512 "" >/dev/null
run "exFAT ohne Bezeichnung: neue anlegen" "$W/label_test" set "$W/ohne.img" $SZ "NEU"
[ "$(lab "$W/ohne.img")" = NEU ] && ok "  liest NEU" || bad "  liest NEU"
if "$W/label_test" set "$W/ohne.img" $SZ "ZU_LANG_FUER_FAT" >"$W/log" 2>&1; then bad "zu lange Bezeichnung angenommen"
else ok "zu lange Bezeichnung abgelehnt"; fi

echo
[ $fails -eq 0 ] && echo "Alle Layout-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
