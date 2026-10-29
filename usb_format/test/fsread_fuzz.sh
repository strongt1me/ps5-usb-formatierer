#!/usr/bin/env bash
# Fuzzt den Abbild-Leser (fsr_walk/fsr_fread, siehe fsread_fuzz.c) unter ASAN/UBSAN an einem exFAT-Abbild (eigener
# Formatierer) und - wenn makefs da ist - an einem UFS2-Abbild mit kleinen Blöcken (indirekte Blöcke).
# Aufruf: test/fsread_fuzz.sh [Durchläufe je Abbild, Standard 1500] [Seed]
set -eu
cd "$(dirname "$0")/.."
N=${1:-1500}
SEED=${2:-1}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror \
    -o "$W/fuzz" test/fsread_fuzz.c src/fsread.c
gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror -o "$W/rd" test/fsread_test.c src/fsread.c
G="$W/game"
mkdir -p "$G/sce_sys" "$G/data/a/b/c" "$G/data/schleife"
printf '{"titleId":"PPSA00011"}' >"$G/sce_sys/param.json"
printf 'ELF' >"$G/eboot.bin"
head -c 3000000 /dev/urandom >"$G/data/gross.bin"
for i in $(seq 1 20); do printf '%s' "$i" >"$G/data/a/datei-mit-langem-namen-$i.txt"; done
printf 'tief' >"$G/data/a/b/c/tief.txt"
printf 'x' >"$G/data/schleife/inhalt.txt"
{ printf '\x89PNG\r\n\x1a\n'; head -c 500000 /dev/urandom; } >"$G/sce_sys/icon0.png"   # 500 KB: in UFS2 mit 4-KiB-Blöcken indirekt
files=()
while IFS= read -r f; do files+=("--file=${f#"$G/"}=$f"); done < <(find "$G" -type f)
"$W/mkfs_test" part "$W/x.exfat" exfat $((16 * 1048576)) 512 "" 4096 "${files[@]}" >/dev/null || { echo "FEHLER  exFAT-Abbild anlegen"; exit 1; }
fails=0
# gezielte Fallen: Ordner "schleife" zeigt auf die Wurzel zurück, gross.bin mit 1 TB Länge und FAT-Schleife
python3 test/fsread_craft.py exfat-loop "$W/x.exfat" "$W/loop.exfat"
python3 test/fsread_craft.py exfat-fatloop "$W/x.exfat" "$W/fatloop.exfat"
out=$(timeout 20 "$W/rd" "$W/loop.exfat" 0 walk) && echo "$out" | grep -qx "rc -1" \
    && echo "OK      exFAT: Ordnerschleife erkannt (fsr_walk -1, sofort)" || { echo "FEHLER  exFAT-Ordnerschleife: $(echo "$out" | tail -3)"; fails=$((fails + 1)); }
out=$(timeout 20 "$W/rd" "$W/fatloop.exfat" 0 walk) && echo "$out" | grep -q "(Lesefehler) data/gross.bin" \
    && echo "OK      exFAT: FAT-Schleife mit 1 TB Länge sofort als Lesefehler" || { echo "FEHLER  exFAT-FAT-Schleife: $(echo "$out" | grep gross)"; fails=$((fails + 1)); }
echo "exFAT:"
"$W/fuzz" "$W/x.exfat" "$N" "$SEED"
if command -v makefs >/dev/null; then
    makefs -t ffs -o version=2,bsize=4096,fsize=512 -s 16m "$W/x.ffpkg" "$G" >/dev/null
    python3 test/fsread_craft.py ufs-loop "$W/x.ffpkg" "$W/loop.ffpkg"
    out=$(timeout 20 "$W/rd" "$W/loop.ffpkg" 0 walk) && echo "$out" | grep -qx "rc -1" \
        && echo "OK      UFS2: Ordnerschleife erkannt (fsr_walk -1, sofort)" || { echo "FEHLER  UFS2-Ordnerschleife: $(echo "$out" | tail -3)"; fails=$((fails + 1)); }
    [ "$("$W/rd" "$W/x.ffpkg" 0 cat sce_sys/icon0.png | wc -c)" = 500008 ] \
        && echo "OK      UFS2: 500-KB-Datei über indirekte Blöcke ganz gelesen (fsr_read_file)" || { echo "FEHLER  große Datei in UFS2"; fails=$((fails + 1)); }
    echo "UFS2:"
    "$W/fuzz" "$W/x.ffpkg" "$N" "$SEED"
else
    echo "UFS2: übersprungen (makefs fehlt)"
fi
[ "$fails" = 0 ] || { echo "$fails Fehler"; exit 1; }
echo "Abbild-Leser-Fuzz bestanden."
