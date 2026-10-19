#!/usr/bin/env bash
# Linux-Helfer an einem echten Blockgerät (Loop-Gerät, gilt in der Testfassung mit USBFMT_TEST_LOOP=1 als USB):
# Aushängen, exklusives Öffnen, Partitionen neu einlesen, NTFS/FAT16/ext über die Systemwerkzeuge, Umbenennen von NTFS,
# Echtheitsprüfung am Zwischenspeicher vorbei, Weigerung bei belegtem Laufwerk, Sichern mit Schreibschutzschalter.
# Braucht Root (läuft in der CI).
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "Nur als root (sudo) - ändert Loop-Geräte"; exit 1; }
W=$(mktemp -d)
LOOP=
LOOP2=
cleanup() {
    umount "$W/mnt" 2>/dev/null
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
    [ -n "$LOOP2" ] && losetup -d "$LOOP2" 2>/dev/null
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

# exFAT mit Ordnern (PS5-Weg); einhängen nur, wenn der Kernel exFAT kann (in der CI fehlt oft das Modul)
h exfat format "$D" $SIZE exfat "PS5 USB" folders
expect exfat 1 "exFAT mit Ordnern"
fsck.exfat -n "$(p 1)" >/dev/null 2>&1 && ok "exFAT: fsck.exfat" || bad "exFAT: fsck.exfat"
if mount -t exfat "$(p 1)" "$W/mnt" 2>/dev/null; then
    [ -d "$W/mnt/etaHEN/games" ] && ok "exFAT: Ordner etaHEN/games vorhanden" || bad "exFAT: Ordner fehlen"
    umount "$W/mnt"
else
    echo "        (Kernel ohne exFAT - Einhängen übersprungen)"
fi
# Ordnervorlage über FAT32 (derselbe Weg im Helfer, vfat kann jeder Kernel)
h fat32dirs format "$D" $SIZE fat32 "PS5 USB" folders
expect fat32dirs 1 "FAT32 mit Ordnern"
mount "$(p 1)" "$W/mnt" && [ -d "$W/mnt/etaHEN/games" ] && [ -d "$W/mnt/homebrew" ] && [ -d "$W/mnt/pkg" ] \
    && ok "Ordner homebrew, etaHEN/games, pkg vorhanden" || bad "Ordner fehlen: $(ls -R "$W/mnt" 2>&1 | head -5)"

# eingehängt: der Helfer hängt selbst aus
h mounted format "$D" $SIZE fat32 DATEN plain
expect mounted 1 "eingehängtes Laufwerk wird ausgehängt und formatiert"
mountpoint -q "$W/mnt" && bad "noch eingehängt" || ok "danach nicht mehr eingehängt"
fsck.fat -n "$(p 1)" >/dev/null 2>&1 && ok "FAT32: fsck.fat" || bad "FAT32: fsck.fat"

# belegt (Programm mit offener Datei): abgelehnt, nichts verändert. Verglichen werden Partitionstabelle und
# Dateisystem-Kennung - das Abbild als Ganzes ändert vfat schon selbst beim Einhängen (Schmutz-Kennzeichen).
mbr_sum() { head -c $M "$W/disk.img" | sha256sum | cut -c1-16; }
mount "$(p 1)" "$W/mnt"
sleep 60 3>"$W/mnt/offen.txt" &   # dieser Prozess selbst hält die Datei offen (lässt sich sauber beenden)
BUSY=$!
sleep 0.5
before=$(mbr_sum)
h busy format "$D" $SIZE exfat X plain
expect busy 0 "belegtes Laufwerk: abgelehnt"
grep -q "wird gerade benutzt" "$W/busy.txt" && ok "Meldung: wird gerade benutzt" || bad "Meldung: $(cat "$W/busy.txt")"
kill $BUSY 2>/dev/null; wait $BUSY 2>/dev/null
umount "$W/mnt" || { echo "        (noch belegt: $(fuser -vm "$W/mnt" 2>&1 | tail -n +2))"; umount -l "$W/mnt"; }
[ "$(mbr_sum)" = "$before" ] && [ "$(blkid -p -o value -s LABEL "$(p 1)")" = DATEN ] \
    && ok "belegt: Datenträger unverändert" || bad "belegt: Datenträger verändert ($(blkid -p "$(p 1)"))"

# NTFS + FAT16 + ext4 + HFS+ (MBR) über die Systemwerkzeuge
h mixed layout "$D" $SIZE scheme=mbr plain "part=ntfs,$((300 * M)),4096,auto,0,WIN DATEN" \
    "part=fat,$((200 * M)),0,auto,1,ALT" "part=ext4,$((300 * M)),0,auto,0,linux" "part=hfsplus,rest,0,auto,0,Mac"
expect mixed 1 "MBR: NTFS, FAT16 (startfähig), ext4, HFS+"
ntfsfix -n "$(p 1)" >/dev/null 2>&1 && ok "NTFS: ntfsfix -n" || bad "NTFS: ntfsfix -n"
[ "$(ntfslabel "$(p 1)" 2>/dev/null)" = "WIN DATEN" ] && ok "NTFS: Bezeichnung" || bad "NTFS: Bezeichnung $(ntfslabel "$(p 1)" 2>&1)"
fsck.fat -n "$(p 2)" >/dev/null 2>&1 && ok "FAT16: fsck.fat" || bad "FAT16: fsck.fat"
fat_type() { dd if="$1" bs=1 skip=54 count=8 2>/dev/null | tr -d ' '; }   # BS_FilSysType im Bootsektor (FAT12/16)
[ "$(fat_type "$(p 2)")" = FAT16 ] && ok "FAT16: wirklich FAT16" || bad "FAT16: '$(fat_type "$(p 2)")'"
e2fsck -fn "$(p 3)" >/dev/null 2>&1 && ok "ext4: e2fsck" || bad "ext4: e2fsck"
fsck.hfsplus -fn "$(p 4)" 2>&1 | grep -q "appears to be OK" && ok "HFS+: fsck.hfsplus" || bad "HFS+: fsck.hfsplus"
grep -q "NTFS, 4,0 KB Cluster\|NTFS, 4 KB Cluster" "$W/mixed.txt" && ok "Meldung: NTFS mit 4 KB" || bad "Meldung: $(grep -P '^R' "$W/mixed.txt")"
[ "$(sfdisk -d "$LOOP" | grep '^/' | sed -n 2p | grep -c bootable)" = 1 ] && ok "FAT16 startfähig" || bad "FAT16 nicht startfähig"

# Umbenennen bei NTFS und FAT16 zugleich: unklar, welches gemeint ist - abgelehnt (wie unter Windows)
h label2 label "$D" $SIZE "NEUER NAME"
expect label2 0 "zwei NTFS-/FAT-Laufwerke: Umbenennen abgelehnt"
[ "$(ntfslabel "$(p 1)" 2>/dev/null)" = "WIN DATEN" ] && ok "NTFS: Bezeichnung unverändert" || bad "NTFS: $(ntfslabel "$(p 1)" 2>&1)"

# NTFS über den ganzen Datenträger, dann umbenennen (ntfslabel)
h ntfs format "$D" $SIZE ntfs "WIN DATEN" plain
expect ntfs 1 "NTFS über den ganzen Datenträger"
ntfsfix -n "$(p 1)" >/dev/null 2>&1 && ok "NTFS: ntfsfix -n" || bad "NTFS: ntfsfix -n"
h label label "$D" $SIZE "NEUER NAME"
expect label 1 "NTFS umbenennen"
[ "$(ntfslabel "$(p 1)" 2>/dev/null)" = "NEUER NAME" ] && ok "NTFS: neue Bezeichnung" || bad "NTFS: $(ntfslabel "$(p 1)" 2>&1)"

# FAT16 über den ganzen Datenträger, dann umbenennen (fatlabel)
h fat16 format "$D" $SIZE fat ALT plain
expect fat16 1 "FAT16 über den ganzen Datenträger"
[ "$(fat_type "$(p 1)")" = FAT16 ] && fsck.fat -n "$(p 1)" >/dev/null 2>&1 && ok "FAT16: fsck.fat" || bad "FAT16: '$(fat_type "$(p 1)")'"
h label16 label "$D" $SIZE NEU16
expect label16 1 "FAT16 umbenennen"
[ "$(blkid -p -o value -s LABEL "$(p 1)")" = NEU16 ] && ok "FAT16: neue Bezeichnung" || bad "FAT16: $(blkid -p "$(p 1)")"

# Echtheitsprüfung (O_DIRECT) und Tempo am echten Gerät
h verify format "$D" $SIZE exfat TEST verify plain
expect verify 1 "Echtheitsprüfung am Blockgerät"
grep -q "echt" "$W/verify.txt" && ok "Prüfung: echt" || bad "Prüfung: $(grep -P '^R' "$W/verify.txt")"
h speed speed "$D" $SIZE plain
expect speed 1 "Tempo messen"

# Schreibschutzschalter (Loop-Gerät nur lesend, der Kernel meldet es wie einen Stick mit Schalter): Sichern geht -
# auch eingehängt (der Helfer hängt aus) -, Formatieren und Wiederherstellen werden abgelehnt, nichts verändert
losetup -d "$LOOP"
LOOP=$(losetup -r -fP --show "$W/disk.img")
D=${LOOP#/dev/}
[ "$(blockdev --getro "$LOOP")" = 1 ] && ok "Loop-Gerät schreibgeschützt" || bad "Loop-Gerät nicht schreibgeschützt"
before=$(sha256sum "$W/disk.img" | cut -c1-16)
mount -o ro "$(p 1)" "$W/mnt" 2>/dev/null || echo "        (Kernel ohne exFAT - Sichern ohne Einhängen)"
h robak backup "$D" $SIZE "$W/ro.img.zst"
expect robak 1 "schreibgeschützt: Sichern"
[ -s "$W/ro.img.zst" ] && [ -s "$W/ro.img.zst.sha256" ] && ok "Image und Prüfsumme angelegt" || bad "Image fehlt"
mountpoint -q "$W/mnt" && { bad "beim Sichern nicht ausgehängt"; umount "$W/mnt"; } || ok "beim Sichern ausgehängt"
(cd "$W" && sha256sum -c --quiet ro.img.zst.sha256) && ok "Prüfsumme der Image-Datei stimmt" || bad "Prüfsumme falsch"
h rofmt format "$D" $SIZE exfat X plain
expect rofmt 0 "schreibgeschützt: Formatieren abgelehnt"
grep -q "schreibgeschützt" "$W/rofmt.txt" && ok "Meldung: schreibgeschützt" || bad "Meldung: $(grep -P '^R' "$W/rofmt.txt")"
h rores restore "$D" $SIZE "$W/ro.img.zst"
expect rores 0 "schreibgeschützt: Wiederherstellen abgelehnt"
[ "$(sha256sum "$W/disk.img" | cut -c1-16)" = "$before" ] && ok "schreibgeschützt: unverändert" || bad "verändert"
# das Image taugt: auf ein zweites, leeres Gerät zurückschreiben (der Helfer liest dabei nach), dann dort prüfen
truncate -s $SIZE "$W/copy.img"
LOOP2=$(losetup -fP --show "$W/copy.img")
h rocopy restore "${LOOP2#/dev/}" $SIZE "$W/ro.img.zst"
expect rocopy 1 "Image des schreibgeschützten Geräts zurückgeschrieben und nachgelesen"
fsck.exfat -n "${LOOP2}p1" >/dev/null 2>&1 && [ "$(blkid -p -o value -s LABEL "${LOOP2}p1")" = TEST ] \
    && ok "Kopie: exFAT TEST, fsck.exfat" || bad "Kopie: $(blkid -p "${LOOP2}p1")"
losetup -d "$LOOP2"
LOOP2=
losetup -d "$LOOP"
LOOP=$(losetup -fP --show "$W/disk.img")
D=${LOOP#/dev/}

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
