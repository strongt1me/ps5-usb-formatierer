#!/usr/bin/env bash
# Linux-Helfer an einem echten Blockgerät (Loop-Gerät, gilt in der Testfassung mit USBFMT_TEST_LOOP=1 als USB):
# Aushängen, exklusives Öffnen, Partitionen neu einlesen, NTFS/FAT16/ext über die Systemwerkzeuge, Umbenennen von NTFS,
# Echtheitsprüfung am Zwischenspeicher vorbei, Weigerung bei belegtem Laufwerk. Braucht Root (läuft in der CI).
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "Nur als root (sudo) - ändert Loop-Geräte"; exit 1; }
W=$(mktemp -d)
LOOP=
cleanup() {
    umount "$W/mnt" 2>/dev/null
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT
bash src/linux/build.sh "$W/h" -DUSBFMT_TEST >/dev/null || { echo "FEHLER  Bau"; exit 1; }
export USBFMT_TEST_LOOP=1
M=$((1024 * 1024))
SIZE=$((1200 * M))
truncate -s $SIZE "$W/disk.img"
LOOP=$(losetup -fP --show "$W/disk.img")
D=${LOOP#/dev/}
mkdir -p "$W/mnt"
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; fails=$((fails + 1)); }
h() { local name=$1; shift; rm -f "$W/$name.txt"; "$W/h" --out "$W/$name.txt" "$@" >/dev/null 2>&1; }
expect() {
    local r
    r=$(grep -P '^R\t' "$W/$1.txt" | tail -n 1)
    if [[ "$r" == "R	$2	"* ]]; then ok "$3"; else bad "$3: $r"; fi
}
p() { echo "${LOOP}p$1"; }

"$W/h" list >"$W/list.json"
python3 - "$W/list.json" "$D" "$SIZE" <<'EOF' && ok "list: Loop-Gerät mit Größe" || bad "list: $(cat "$W/list.json")"
import json, sys
d = [x for x in json.load(open(sys.argv[1])) if x["disk"] == sys.argv[2]]
sys.exit(0 if d and d[0]["size"] == int(sys.argv[3]) else 1)
EOF

# exFAT mit Ordnern (PS5-Weg), danach einhängen und nachsehen
h exfat format "$D" $SIZE exfat "PS5 USB" folders
expect exfat 1 "exFAT mit Ordnern"
fsck.exfat -n "$(p 1)" >/dev/null 2>&1 && ok "exFAT: fsck.exfat" || bad "exFAT: fsck.exfat"
mount "$(p 1)" "$W/mnt" && [ -d "$W/mnt/etaHEN/games" ] && ok "Ordner etaHEN/games vorhanden" || bad "Ordner fehlen"

# eingehängt: der Helfer hängt selbst aus
h mounted format "$D" $SIZE fat32 DATEN plain
expect mounted 1 "eingehängtes Laufwerk wird ausgehängt und formatiert"
mountpoint -q "$W/mnt" && bad "noch eingehängt" || ok "danach nicht mehr eingehängt"
fsck.fat -n "$(p 1)" >/dev/null 2>&1 && ok "FAT32: fsck.fat" || bad "FAT32: fsck.fat"

# belegt (Programm mit offener Datei): abgelehnt, nichts verändert
mount "$(p 1)" "$W/mnt"
( exec 3>"$W/mnt/offen.txt"; sleep 30 ) &
BUSY=$!
sleep 0.5
before=$(sha256sum "$W/disk.img" | cut -c1-16)
h busy format "$D" $SIZE exfat X plain
expect busy 0 "belegtes Laufwerk: abgelehnt"
grep -q "wird gerade benutzt" "$W/busy.txt" && ok "Meldung: wird gerade benutzt" || bad "Meldung: $(cat "$W/busy.txt")"
kill $BUSY 2>/dev/null; wait $BUSY 2>/dev/null
umount "$W/mnt"
[ "$(sha256sum "$W/disk.img" | cut -c1-16)" = "$before" ] && ok "belegt: Datenträger unverändert" || bad "belegt: Datenträger verändert"

# NTFS + FAT16 + ext4 + HFS+ (MBR) über die Systemwerkzeuge
h mixed layout "$D" $SIZE scheme=mbr plain "part=ntfs,$((300 * M)),4096,auto,0,WIN DATEN" \
    "part=fat,$((200 * M)),0,auto,1,ALT" "part=ext4,$((300 * M)),0,auto,0,linux" "part=hfsplus,rest,0,auto,0,Mac"
expect mixed 1 "MBR: NTFS, FAT16 (startfähig), ext4, HFS+"
ntfsfix -n "$(p 1)" >/dev/null 2>&1 && ok "NTFS: ntfsfix -n" || bad "NTFS: ntfsfix -n"
[ "$(ntfslabel "$(p 1)" 2>/dev/null)" = "WIN DATEN" ] && ok "NTFS: Bezeichnung" || bad "NTFS: Bezeichnung $(ntfslabel "$(p 1)" 2>&1)"
fsck.fat -n "$(p 2)" >/dev/null 2>&1 && ok "FAT16: fsck.fat" || bad "FAT16: fsck.fat"
blkid -o value -s VERSION "$(p 2)" | grep -q FAT16 && ok "FAT16: wirklich FAT16" || bad "FAT16: $(blkid "$(p 2)")"
e2fsck -fn "$(p 3)" >/dev/null 2>&1 && ok "ext4: e2fsck" || bad "ext4: e2fsck"
fsck.hfsplus -fn "$(p 4)" 2>&1 | grep -q "appears to be OK" && ok "HFS+: fsck.hfsplus" || bad "HFS+: fsck.hfsplus"
grep -q "NTFS, 4,0 KB Cluster\|NTFS, 4 KB Cluster" "$W/mixed.txt" && ok "Meldung: NTFS mit 4 KB" || bad "Meldung: $(grep -P '^R' "$W/mixed.txt")"
[ "$(sfdisk -d "$LOOP" | grep '^/' | sed -n 2p | grep -c bootable)" = 1 ] && ok "FAT16 startfähig" || bad "FAT16 nicht startfähig"

# NTFS umbenennen (ntfslabel)
h label label "$D" $SIZE "NEUER NAME"
expect label 1 "NTFS umbenennen"
[ "$(ntfslabel "$(p 1)" 2>/dev/null)" = "NEUER NAME" ] && ok "NTFS: neue Bezeichnung" || bad "NTFS: $(ntfslabel "$(p 1)" 2>&1)"

# Echtheitsprüfung (O_DIRECT) und Tempo am echten Gerät
h verify format "$D" $SIZE exfat TEST verify plain
expect verify 1 "Echtheitsprüfung am Blockgerät"
grep -q "echt" "$W/verify.txt" && ok "Prüfung: echt" || bad "Prüfung: $(grep -P '^R' "$W/verify.txt")"
h speed speed "$D" $SIZE plain
expect speed 1 "Tempo messen"

# Größe passt nicht (anderer Stick): nichts angefasst
before=$(sha256sum "$W/disk.img" | cut -c1-16)
h size format "$D" $((SIZE - M)) exfat X
expect size 0 "andere Größe: abgelehnt"
[ "$(sha256sum "$W/disk.img" | cut -c1-16)" = "$before" ] && ok "andere Größe: unverändert" || bad "andere Größe: verändert"

# Nicht-USB-Gerät (die Systemplatte der CI): abgelehnt, bevor irgendetwas geöffnet wird
ROOTDEV=$(lsblk -no PKNAME "$(findmnt -no SOURCE /)" 2>/dev/null | head -n 1)
if [ -n "$ROOTDEV" ]; then
    RS=$(cat "/sys/block/$ROOTDEV/size")
    h sys format "$ROOTDEV" $((RS * 512)) exfat X
    expect sys 0 "Systemplatte ($ROOTDEV): abgelehnt"
fi

echo
[ $fails -eq 0 ] && echo "Alle Loop-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
