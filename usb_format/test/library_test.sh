#!/usr/bin/env bash
# Spielebibliothek (src/library.c): nachgebautes USB-Laufwerk mit Spiele-Ordnern (PS5 param.json, PS4 param.sfo),
# Abbildern (.exfat aus dem eigenen Formatierer, .ffpkg aus makefs), PKG-Dateien und einem doppelten Spiel.
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gcc -Wall -Wextra -Werror -O2 -o "$W/lib" test/library_test.c src/library.c src/fsread.c || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c || exit 1

fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; sed 's/^/        /' "$W/out"; fails=$((fails + 1)); }
has() { grep -qF -- "$2" "$W/out" && ok "$1" || bad "$1"; }

U="$W/usb1"
mkdir -p "$U/homebrew/Astro/sce_sys" "$U/etaHEN/games/CUSA00001/sce_sys" "$U/pkg" "$U/System Volume Information" "$U/Musik"
# PS5-Spiel als Ordner, deutscher Name vor der Standardsprache, \u-Escapes
cat >"$U/homebrew/Astro/sce_sys/param.json" <<'EOF'
{"contentId":"UP9000-PPSA01325_00-ASTROBOT00000000","localizedParameters":{"defaultLanguage":"en-US",
 "en-US":{"titleName":"Astro Bot"},"de-DE":{"titleName":"Astro Bot – Deutsch"}},"titleId":"PPSA01325"}
EOF
# PS4-Spiel mit param.sfo
python3 - "$U/etaHEN/games/CUSA00001/sce_sys/param.sfo" <<'EOF'
import struct, sys
entries = [(b"TITLE", "Altes Spiel".encode() + b"\0"), (b"TITLE_ID", b"CUSA00001\0")]
keys = b"".join(k + b"\0" for k, _ in entries)
keys += b"\0" * ((4 - len(keys) % 4) % 4)
kt = 20 + 16 * len(entries); dt = kt + len(keys)
idx = b""; data = b""; ko = 0
for k, v in entries:
    idx += struct.pack("<HHIII", ko, 0x0204, len(v), len(v), len(data))
    ko += len(k) + 1; data += v
open(sys.argv[1], "wb").write(b"\0PSF" + struct.pack("<IIII", 0x101, kt, dt, len(entries)) + idx + keys + data)
EOF
# exFAT-Abbild mit eigenem Formatierer, UFS2-Abbild mit makefs
printf '{"titleId":"PPSA03117","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Crazy Chicken"}}}' >"$W/cc.json"
"$W/mkfs_test" part "$U/homebrew/Crazy Chicken Bundle.exfat" exfat $((64 * 1048576)) 512 "" 4096 \
    --file=sce_sys/param.json="$W/cc.json" >/dev/null
mkdir -p "$W/ufs/sce_sys" && cp "$W/cc.json" "$W/ufs/sce_sys/param.json"
makefs -t ffs -o version=2,bsize=65536,fsize=8192 -s 16m "$U/homebrew/Crazy Chicken.ffpkg" "$W/ufs" >/dev/null
printf 'kein Abbild' >"$U/homebrew/Kaputt.ffpfs"
head -c 1000 /dev/zero >"$U/pkg/a.pkg"; head -c 1000 /dev/zero >"$U/b.pkg"; head -c 10 /dev/zero >"$U/homebrew/c.PKG"

"$W/lib" "$U" usb >"$W/out" 2>&1
has "PS5-Ordner: Titel-ID und deutscher Name (\\u-Escape)" "PPSA01325|Astro Bot – Deutsch|Ordner|"
has "PS4-Ordner (param.sfo)" "CUSA00001|Altes Spiel|Ordner|"
has "exFAT-Abbild" "PPSA03117|Crazy Chicken|exFAT|$U/homebrew/Crazy Chicken Bundle.exfat"
has "UFS2-Abbild" "PPSA03117|Crazy Chicken|UFS2|$U/homebrew/Crazy Chicken.ffpkg"
has "unlesbares PFS-Abbild mit Dateinamen" "|Kaputt|PFS|"
[ "$(grep -c '|PFS|' "$W/out")" = 1 ] || bad "nur ein PFS-Abbild erwartet"
has "3 PKG-Dateien (Wurzel, pkg/, homebrew/)" "pkgs=3"
grep -q "ordner=.*etaHEN" "$W/out" && grep -q "ordner=.*homebrew" "$W/out" && ! grep -q "System Volume" "$W/out" \
    && ok "Ordner der Wurzel ohne Systemordner" || bad "Ordner der Wurzel"
[ "$(grep -c '^PPSA03117|' "$W/out")" = 2 ] && ok "doppeltes Spiel zweimal gefunden" || bad "doppeltes Spiel"

# .ffpfs/.ffpfsc: Titel-ID aus dem Protokoll von ShadowMountPlus, Name aus seiner API
head -c 100000 /dev/urandom >"$U/homebrew/Returnal.ffpfs"
head -c 100000 /dev/urandom >"$U/etaHEN/games/GT7 Bundle.ffpfsc"
cat >"$W/debug.log" <<EOF
[21:53:24]   [LINK] mount_img.lnk created: /user/app/PPSA01284/mount_img.lnk -> $U/homebrew/Returnal.ffpfs layers=1
[21:53:25]   [LINK] mount_img.lnk created: /user/app/PPSA01316/mount_img.lnk -> $U/etaHEN/games/GT7 Bundle.ffpfsc layers=2
[21:53:26]   [IMG] etwas anderes ohne Zuordnung
EOF
printf '{"status":0,"count":2,"games":[{"path":"%s","title_id":"PPSA01284","title_name":"Returnal","source_type":"image"},{"path":"/x","title_id":"PPSA01316","title_name":"Gran Turismo\\u2122 7","x":[1,{"y":2}]}]}' \
    "$U/homebrew/Returnal.ffpfs" >"$W/games.json"
"$W/lib" "$U" usb --log="$W/debug.log" --games="$W/games.json" >"$W/out" 2>&1
has ".ffpfs: Titel-ID und Name von ShadowMountPlus" "PPSA01284|Returnal|PFS|$U/homebrew/Returnal.ffpfs"
has ".ffpfsc: Titel-ID aus dem Protokoll, Name über die Titel-ID (\\u-Escape)" "PPSA01316|Gran Turismo™ 7|PFS|$U/etaHEN/games/GT7 Bundle.ffpfsc"
has ".ffpkg weiterhin aus dem Abbild" "PPSA03117|Crazy Chicken|UFS2|"
"$W/lib" "$U" usb >"$W/out" 2>&1
has "ohne ShadowMountPlus: Dateiname, Art PFS" "|Returnal|PFS|"

"$W/lib" "$W/fehlt" usb >"$W/out" 2>&1
has "fehlende Wurzel: leer, kein Absturz" "pkgs=0 ordner="

echo
[ $fails -eq 0 ] && echo "Alle Bibliotheks-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
