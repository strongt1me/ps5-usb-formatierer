#!/usr/bin/env bash
# Linux-Helfer (Testfassung mit --image) an Abbild-Dateien: alle eigenen Dateisysteme, ext2/3/4 über mke2fs, mehrere
# Partitionen (MBR/GPT), Zuordnungseinheiten, Umbenennen, Tempo, Echtheitsprüfung, Abbrechen, Fehlerfälle und die
# Ausgabedatei. Geprüft wird mit den Linux-Werkzeugen (sfdisk, fsck.exfat, fsck.fat, fsck.hfsplus, e2fsck).
# Läuft ohne Root. Werkzeuge außerhalb von PATH: z. B. FSTOOLS=~/fstools/root
set -u
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
if [ -n "${FSTOOLS:-}" ]; then
    export PATH="$FSTOOLS/usr/sbin:$FSTOOLS/sbin:$FSTOOLS/usr/bin:$PATH"
    export LD_LIBRARY_PATH="$FSTOOLS/usr/lib:$FSTOOLS/lib:$FSTOOLS/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
fi
export PATH="$PATH:/sbin:/usr/sbin"
bash src/linux/build.sh "$W/h" -DUSBFMT_TEST >/dev/null || { echo "FEHLER  Bau"; exit 1; }
H="$W/h"
M=$((1024 * 1024))
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; fails=$((fails + 1)); }

# run <name> <größe> <befehl> [angaben ...]: Abbild anlegen, Helfer laufen lassen; Ergebnis in $W/<name>.txt
run() {
    local name=$1 size cmd=$3
    size=$(numfmt --from=iec "$2")   # 1G -> Bytes (der Helfer vergleicht die Größe genau)
    shift 3
    rm -f "$W/$name.img" "$W/$name.txt"
    truncate -s "$size" "$W/$name.img"
    "$H" --out "$W/$name.txt" --image "$W/$name.img" "$cmd" image "$size" "$@" >/dev/null 2>&1
}
result() { grep -P '^R\t' "$W/$1.txt" | tail -n 1; }
expect() {   # expect <name> <1|0> <beschreibung>
    local r
    r=$(result "$1")
    if [[ "$r" == "R	$2	"* ]]; then ok "$3"; else bad "$3: $r"; fi
}
part() {   # part <image> <nr> -> $W/p.img (Partition herauskopieren)
    local s c
    read -r s c < <(sfdisk -d "$1" 2>/dev/null | grep '^/' | sed -n "${2}p" | sed 's/.*start= *\([0-9]*\).*size= *\([0-9]*\).*/\1 \2/')
    dd if="$1" of="$W/p.img" bs=4M iflag=skip_bytes,count_bytes skip=$((s * 512)) count=$((c * 512)) conv=sparse status=none
}
type_of() { sfdisk -d "$1" 2>/dev/null | grep '^/' | sed -n "${2}p" | sed 's/.*type= *\([^,]*\).*/\1/'; }
has() { command -v "$1" >/dev/null; }

# ------------------------------------------------------------------ eine Partition
run exfat 1G format exfat "PS5 USB" folders
expect exfat 1 "exFAT (PS5-Weg mit Ordnern)"
grep -q "Ordner homebrew" "$W/exfat.txt" && ok "Meldung nennt die Ordner" || bad "Ordner fehlen in der Meldung"
part "$W/exfat.img" 1
has fsck.exfat && { fsck.exfat -n "$W/p.img" >/dev/null 2>&1 && ok "exFAT: fsck.exfat" || bad "exFAT: fsck.exfat"; }

run fat32 1G format fat32 FAT32 plain cluster=4096 scheme=gpt
expect fat32 1 "FAT32, 4 KB, GPT"
[ "$(sfdisk -d "$W/fat32.img" 2>/dev/null | grep -c '^label: gpt')" = 1 ] && ok "FAT32: GPT" || bad "FAT32: kein GPT"
part "$W/fat32.img" 1
has fsck.fat && { fsck.fat -n "$W/p.img" >/dev/null 2>&1 && ok "FAT32: fsck.fat" || bad "FAT32: fsck.fat"; }

run hfs 1G format hfsplus "Mac Daten" plain scheme=gpt
expect hfs 1 "HFS+ allein"
part "$W/hfs.img" 1
[ "$(type_of "$W/hfs.img" 1)" = 48465300-0000-11AA-AA11-00306543ECAC ] && ok "HFS+: Typ Apple" || bad "HFS+: Typ $(type_of "$W/hfs.img" 1)"
has fsck.hfsplus && { fsck.hfsplus -fn "$W/p.img" 2>&1 | grep -q "appears to be OK" && ok "HFS+: fsck.hfsplus" || bad "HFS+: fsck.hfsplus"; }

for e in ext4 ext3 ext2; do
    blk=; [ $e = ext2 ] && blk=cluster=1024   # ext2: gewählte Blockgröße muss ankommen (-b)
    run "$e" 512M format "$e" "LINUX $e" plain $blk
    expect "$e" 1 "$e über mke2fs"
    part "$W/$e.img" 1
    e2fsck -fn "$W/p.img" >/dev/null 2>&1 && ok "$e: e2fsck" || bad "$e: e2fsck"
    dumpe2fs -h "$W/p.img" 2>/dev/null | grep -q "volume name: *LINUX $e" && ok "$e: Bezeichnung" || bad "$e: Bezeichnung"
done
grep -qP '^P\t\d+\t' "$W/ext4.txt" && ok "ext4: Fortschritt gemeldet" || bad "ext4: kein Fortschritt"
[ "$(type_of "$W/ext4.img" 1)" = 83 ] && ok "ext4: Typ Linux (83)" || bad "ext4: Typ $(type_of "$W/ext4.img" 1)"
dumpe2fs -h "$W/p.img" 2>/dev/null | grep -q "Block size: *1024" && ok "ext2: gewählte 1-KB-Blöcke" || bad "ext2: Blockgröße"

# ------------------------------------------------------------------ mehrere Partitionen
run gpt 2G layout scheme=gpt plain "part=exfat,$((300 * M)),32768,auto,0,EINS" "part=fat32,$((400 * M)),4096,auto,0,ZWEI" \
    "part=hfsplus,$((400 * M)),0,auto,0,Mac HD" "part=ext4,$((500 * M)),4096,auto,0,linux root" \
    "part=none,$((64 * M)),0,swap,0," "part=exfat,rest,0,auto,0,REST"
expect gpt 1 "GPT mit 6 Partitionen"
sfdisk --verify "$W/gpt.img" >/dev/null 2>&1 && ok "GPT: sfdisk --verify" || bad "GPT: sfdisk --verify"
want=(EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 48465300-0000-11AA-AA11-00306543ECAC
      0FC63DAF-8483-4772-8E79-3D69D8477DE4 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F EBD0A0A2-B9E5-4433-87C0-68B6B72699C7)
for i in 1 2 3 4 5 6; do
    [ "$(type_of "$W/gpt.img" $i)" = "${want[$((i - 1))]}" ] && ok "GPT: Typ Partition $i" || bad "GPT: Typ Partition $i = $(type_of "$W/gpt.img" $i)"
done
part "$W/gpt.img" 4
e2fsck -fn "$W/p.img" >/dev/null 2>&1 && ok "GPT: ext4 in Partition 4" || bad "GPT: ext4 in Partition 4"
part "$W/gpt.img" 3
has fsck.hfsplus && { fsck.hfsplus -fn "$W/p.img" 2>&1 | grep -q "appears to be OK" && ok "GPT: HFS+ in Partition 3" || bad "GPT: HFS+"; }
sfdisk -d "$W/gpt.img" | grep -q 'name="linux root"' && ok "GPT: Partitionsname" || bad "GPT: Partitionsname"

run mbr 1G layout scheme=mbr plain "part=fat32,$((200 * M)),0,efi,1,BOOT" "part=ext3,$((300 * M)),0,auto,0,root" \
    "part=exfat,rest,0,auto,0,DATEN"
expect mbr 1 "MBR mit 3 Partitionen"
sfdisk -d "$W/mbr.img" | grep '^/' | sed -n 1p | grep -q bootable && ok "MBR: Partition 1 startfähig" || bad "MBR: nicht startfähig"
[ "$(type_of "$W/mbr.img" 1)" = ef ] && ok "MBR: Typ EFI (ef)" || bad "MBR: Typ $(type_of "$W/mbr.img" 1)"
part "$W/mbr.img" 2
e2fsck -fn "$W/p.img" >/dev/null 2>&1 && ok "MBR: ext3 in Partition 2" || bad "MBR: ext3"

# ------------------------------------------------------------------ Umbenennen, Tempo, Echtheit, Abbrechen
"$H" --out "$W/label.txt" --image "$W/exfat.img" label image $((1024 * M)) "NEU NAME" >/dev/null 2>&1
expect label 1 "exFAT umbenennen"
part "$W/exfat.img" 1
if has exfatlabel; then
    lbl=$(exfatlabel "$W/p.img" 2>/dev/null | tail -n 1)
    lbl=${lbl#label: }   # ältere exfatprogs schreiben "label: <name>"
    [ "$lbl" = "NEU NAME" ] && ok "Bezeichnung geschrieben" || bad "Bezeichnung: $lbl"
fi
"$H" --out "$W/speed.txt" --image "$W/exfat.img" speed image $((1024 * M)) plain >/dev/null 2>&1
expect speed 1 "Tempo messen"
run verify 512M format exfat TEST verify plain
expect verify 1 "Echtheitsprüfung vor dem Formatieren (echtes Abbild)"
grep -q "echt" "$W/verify.txt" && ok "Prüfung: als echt gemeldet" || bad "Prüfung: Meldung"
touch "$W/cancel"
rm -f "$W/vc.txt"; truncate -s 512M "$W/vc.img"
"$H" --out "$W/vc.txt" --cancel "$W/cancel" --image "$W/vc.img" format image $((512 * M)) exfat X verify plain >/dev/null 2>&1
expect vc 1 "Abbrechen der Prüfung: wird trotzdem formatiert"
grep -q "abgebrochen" "$W/vc.txt" && ok "Abbrechen gemeldet" || bad "Abbrechen nicht gemeldet"

# ------------------------------------------------------------------ Fehlerfälle
run mbr5 1G layout scheme=mbr "part=exfat,$((100 * M)),0,auto,0,A" "part=exfat,$((100 * M)),0,auto,0,B" \
    "part=exfat,$((100 * M)),0,auto,0,C" "part=exfat,$((100 * M)),0,auto,0,D" "part=exfat,rest,0,auto,0,E"
expect mbr5 0 "MBR mit 5 Partitionen abgelehnt"
run blk8k 512M format ext4 X cluster=8192
expect blk8k 0 "ext4 mit 8-KB-Blöcken abgelehnt"
run lab17 512M format ext4 SIEBZEHN_ZEICHEN1
expect lab17 0 "ext-Bezeichnung mit 17 Zeichen abgelehnt"
run kaputt 512M layout "part=exfat,abc,0,auto,0,X"
expect kaputt 0 "kaputte Partitionsangabe abgelehnt"
run ntfs 512M layout "part=ntfs,rest,0,auto,0,X"
expect ntfs 0 "NTFS im Abbild-Test abgelehnt"
run fremd 512M format exfat X bogus
expect fremd 0 "unbekannte Angabe abgelehnt"
truncate -s 1G "$W/klein.img"
"$H" --out "$W/klein.txt" --image "$W/klein.img" format image $((512 * M)) exfat X >/dev/null 2>&1
expect klein 0 "falsche Größe (anderer Stick): nichts angefasst"
"$H" --lang en --out "$W/en.txt" --image "$W/klein.img" format image $((512 * M)) exfat X >/dev/null 2>&1
grep -q "Image does not match\|image" "$W/en.txt" && ok "Meldung auf Englisch (--lang en)" || bad "Englisch: $(result en)"

# ------------------------------------------------------------------ Ausgabedatei (als root angelegt: nur neu, nie umgeleitet)
"$H" --out "$W/exfat.txt" list >/dev/null 2>&1
[ $? = 10 ] && ok "vorhandene Ausgabedatei: abgelehnt (Exit 10)" || bad "vorhandene Ausgabedatei überschrieben"
mkdir -p "$W/echt"; ln -s "$W/echt" "$W/link"
"$H" --out "$W/link/x.txt" list >/dev/null 2>&1
[ $? = 10 ] && [ ! -e "$W/echt/x.txt" ] && ok "Ordner über Verknüpfung: abgelehnt" || bad "Verknüpfung gefolgt"
"$H" list >"$W/list.json" && python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$W/list.json" && ok "list: gültiges JSON" || bad "list: kein JSON"

echo
[ $fails -eq 0 ] && echo "Alle Linux-Helfer-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
