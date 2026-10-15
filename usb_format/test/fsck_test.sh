#!/usr/bin/env bash
# exFAT prüfen/reparieren im Formatierer (src/exfat_fsck.c mit exfatprogs): sauberes Abbild, gezielt beschädigte
# Abbilder (Bitmap-Bit gelöscht, Prüfsumme eines Eintragssatzes falsch), Reparatur, Gegenprobe mit fsck.exfat.
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
OBJS=$(bash test/build_exfatprogs.sh "$W/obj" | tail -1) || exit 1
gcc -O2 -Wall -Wextra -Werror -o "$W/fsck" test/fsck_test.c $OBJS || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c || exit 1

fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; sed 's/^/        /' "$W/out"; fails=$((fails + 1)); }
MIB=1048576
OFF=$MIB
SIZE=$((511 * MIB))

printf '{"titleId":"PPSA01234"}' >"$W/param.json"
head -c 200000 /dev/urandom >"$W/eboot.bin"
mkimg() {
    "$W/mkfs_test" disk "$1" exfat $((512 * MIB)) 512 PS5USB 0 --dir=homebrew --dir=etaHEN/games \
        --file=homebrew/Spiel/sce_sys/param.json="$W/param.json" --file=homebrew/Spiel/eboot.bin="$W/eboot.bin" >/dev/null
}

# Sauber
mkimg "$W/a.img"
"$W/fsck" "$W/a.img" $OFF $SIZE check >"$W/out" 2>&1
grep -q "lauf1 code=0 .*Keine Fehler gefunden (.*Ordner, .*Dateien)" "$W/out" && grep -q "lauf2 code=0" "$W/out" \
    && ok "sauberes Abbild: $(head -1 "$W/out" | cut -d'|' -f2)" || bad "sauberes Abbild"

# Beschädigung 1: Start-Cluster von eboot.bin ungültig (Prüfsumme des Eintragssatzes passend nachgerechnet)
mkimg "$W/b.img"
python3 - "$W/b.img" $OFF <<'EOF'
import struct, sys
f = open(sys.argv[1], 'r+b'); base = int(sys.argv[2])
data = bytearray(f.read())
i = data.find('eboot.bin'.encode('utf-16-le'), base)
start = i - 2 - 64
assert data[start] == 0x85 and data[start + 32] == 0xC0
struct.pack_into('<I', data, start + 32 + 20, 0x0FFFFFF0)
n = data[start + 1] + 1
s = 0
for k in range(32 * n):
    if k in (2, 3): continue
    s = ((0x8000 if s & 1 else 0) + (s >> 1) + data[start + k]) & 0xFFFF
struct.pack_into('<H', data, start + 2, s)
f.seek(start); f.write(data[start:start + 32 * n])
EOF
"$W/fsck" "$W/b.img" $OFF $SIZE check >"$W/out" 2>&1
grep -q "lauf1 code=4 .*Fehler gefunden" "$W/out" && ok "ungültiger Start-Cluster erkannt (nur prüfen)" \
    || bad "ungültigen Start-Cluster erkennen"
"$W/fsck" "$W/b.img" $OFF $SIZE repair >"$W/out" 2>&1
grep -q "lauf1 code=1 .*behoben" "$W/out" && grep -q "lauf2 code=0" "$W/out" \
    && ok "ungültiger Start-Cluster repariert, danach sauber" || bad "ungültigen Start-Cluster reparieren"
dd if="$W/b.img" of="$W/p.img" bs=1M skip=1 status=none
fsck.exfat -n "$W/p.img" >"$W/out" 2>&1 && ok "Gegenprobe fsck.exfat (Start-Cluster)" || bad "Gegenprobe fsck.exfat"

# Beschädigung 2: Prüfsumme des Eintragssatzes "homebrew" verfälschen
mkimg "$W/c.img"
python3 - "$W/c.img" $OFF <<'EOF'
import sys
f = open(sys.argv[1], 'r+b'); base = int(sys.argv[2])
data = f.read()
name = 'homebrew'.encode('utf-16-le')
i = data.find(name, base)
start = i - 2 - 64          # Name-Eintrag (0xC1) -> davor Stream (0xC0) und File (0x85)
assert data[start] == 0x85, hex(data[start])
f.seek(start + 2); f.write(bytes([data[start + 2] ^ 0xFF]))
EOF
"$W/fsck" "$W/c.img" $OFF $SIZE check >"$W/out" 2>&1
grep -q "lauf1 code=4" "$W/out" && ok "Prüfsummen-Fehler erkannt" || bad "Prüfsummen-Fehler erkennen"
"$W/fsck" "$W/c.img" $OFF $SIZE repair >"$W/out" 2>&1
grep -q "lauf1 code=1" "$W/out" && grep -q "lauf2 code=0" "$W/out" \
    && ok "Prüfsummen-Fehler repariert, danach sauber" || bad "Prüfsummen-Fehler reparieren"
dd if="$W/c.img" of="$W/p.img" bs=1M skip=1 status=none
fsck.exfat -n "$W/p.img" >"$W/out" 2>&1 && ok "Gegenprobe fsck.exfat (Prüfsumme)" || bad "Gegenprobe fsck.exfat (Prüfsumme)"

# Kein exFAT
head -c $((4 * MIB)) /dev/zero >"$W/z.img"
"$W/fsck" "$W/z.img" 0 $((4 * MIB)) check >"$W/out" 2>&1
grep -q "lauf1 code=8 .*nicht möglich" "$W/out" && ok "kein exFAT: sauber abgelehnt" || bad "kein exFAT"

echo
[ $fails -eq 0 ] && echo "Alle fsck-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
