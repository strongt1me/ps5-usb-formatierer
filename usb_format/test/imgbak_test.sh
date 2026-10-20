#!/usr/bin/env bash
# Prüft Image-Sicherung und -Wiederherstellung (src/imgbak.c) an Image-Dateien: exFAT/FAT32 mit MBR und GPT,
# Dateisystem ohne Tabelle, 4K-Sektoren, fremde Images (nur zstd), beschädigte Dateien, Nachlese-Fehler.
# Ist das Programm "zstd" vorhanden, wird zusätzlich geprüft, dass es die .img.zst ganz normal entpackt.
cd "$(dirname "$0")/.."
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
FAIL=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FEHLER $*"; FAIL=1; }

gcc -Wall -Wextra -Werror -O2 ${HOST_CFLAGS:-} -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c \
    src/i18n.c src/i18n_table.c || exit 1
gcc -c -O2 ${HOST_CFLAGS:-} -isystem third_party/zstd/lib -o "$W/zstd.o" src/zstd_all.c || exit 1
gcc -Wall -Wextra -Werror -O2 ${HOST_CFLAGS:-} -isystem third_party/zstd/lib -o "$W/ib" test/imgbak_test.c src/imgbak.c \
    src/sha256.c src/i18n.c src/i18n_table.c "$W/zstd.o" -lpthread || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/fsread_test" test/fsread_test.c src/fsread.c || exit 1
IB=$W/ib

head -c $((3 * 1024 * 1024 + 12345)) /dev/urandom >"$W/f1"
head -c $((700 * 1024)) /dev/urandom >"$W/f2"
FILES=(--dir=/a/b "--file=/a/b/eins.bin=$W/f1" "--file=/zwei.bin=$W/f2")

# Quelle -> Sichern -> in Müll wiederherstellen -> belegte Bereiche gleich, fsck sauber, Dateien gleich
roundtrip() {   # name modus fs bytes ss [optionen]
    local n=$1 mode=$2 fs=$3 bytes=$4 ss=$5
    shift 5
    rm -f "$W/s.img" "$W/t.img"
    truncate -s "$bytes" "$W/s.img"
    if [ "$fs" = exfat ]; then
        "$W/mkfs_test" "$mode" "$W/s.img" "$fs" "$bytes" "$ss" QUELLE 0 "$@" "${FILES[@]}" >/dev/null 2>&1 || { bad "$n: mkfs"; return; }
    else   # FAT32: Dateien gleich beim Formatieren gibt es nicht - 1000 Cluster als Kette belegen und füllen
        "$W/mkfs_test" "$mode" "$W/s.img" "$fs" "$bytes" "$ss" QUELLE 0 "$@" >/dev/null 2>&1 || { bad "$n: mkfs"; return; }
        python3 - "$W/s.img" "$ss" <<'EOF2' || { bad "$n: FAT32 füllen"; return; }
import os, struct, sys
f = open(sys.argv[1], 'r+b'); ss = int(sys.argv[2])
d = f.read(3 * ss)
base = 0
if d[82:90] != b'FAT32   ':
    base = struct.unpack('<Q', d[2 * ss + 32:2 * ss + 40])[0] * ss if d[450] == 0xEE else struct.unpack('<I', d[454:458])[0] * ss
f.seek(base); bs = f.read(512)
bps, spc, rsvd, nf, fsz = struct.unpack('<H', bs[11:13])[0], bs[13], struct.unpack('<H', bs[14:16])[0], bs[16], struct.unpack('<I', bs[36:40])[0]
heap = base + (rsvd + nf * fsz) * bps
for k in range(nf):
    fat = base + (rsvd + k * fsz) * bps
    for c in range(500, 1500):
        f.seek(fat + 4 * c); f.write(struct.pack('<I', c + 1 if c < 1499 else 0x0FFFFFFF))
f.seek(heap + (500 - 2) * bps * spc); f.write(os.urandom(1000 * bps * spc))
EOF2
    fi
    local u
    u=$("$IB" used "$W/s.img" "$ss")
    read -r used fbytes shaimg _ < <("$IB" backup "$W/s.img" "$W/b.img.zst" "$ss") || { bad "$n: Sichern"; return; }
    [ "$used" = "$u" ] || bad "$n: belegt $used <> $u"
    [ "$used" -lt $((bytes / 4)) ] && [ "$used" -gt $((3 * 1024 * 1024)) ] && ok "$n: belegt $((used / 1024)) KB von $((bytes / 1048576)) MB, Datei $((fbytes / 1024)) KB" \
        || bad "$n: belegt $used von $bytes"
    head -c "$bytes" /dev/urandom >"$W/t.img"
    read -r wrote shar < <("$IB" restore "$W/b.img.zst" "$W/t.img" "$ss") || { bad "$n: Wiederherstellen"; return; }
    [ "$wrote" = "$used" ] && [ "$shar" = "$shaimg" ] && ok "$n: wiederhergestellt ($((wrote / 1024)) KB geschrieben)" \
        || bad "$n: geschrieben $wrote / $shar"
    # Prüfen: belegte Blöcke gleich (Müll in freien Clustern bleibt), SHA-256 über sie wie gemeldet und im Endstück
    if python3 - "$W/s.img" "$W/t.img" "$W/b.img.zst" "$shaimg" <<'EOF'; then ok "$n: belegte Blöcke identisch, SHA-256 über sie stimmt"; else bad "$n: belegte Blöcke/SHA ($?)"; fi
import hashlib, sys
s, t, b = (open(p, 'rb') for p in sys.argv[1:4])
raw = b.read()
hl = int.from_bytes(raw[4:8], 'little')
head = raw[8:8 + hl]
nl = head.index(b'\n', len(b'USBFMT-IMAGE 1\n'))
bits = head[nl + 1:]
tail = raw[-(8 + 79):]
if int.from_bytes(tail[:4], 'little') != 0x184D2A5B or tail[8:] != b'USBFMT-SHA256 ' + sys.argv[4].encode() + b'\n': sys.exit(3)
blk = 65536
i = 0
h = hashlib.sha256()
while True:
    a, c = s.read(blk), t.read(blk)
    if not a: break
    if bits[i >> 3] >> (i & 7) & 1:
        if a != c: sys.exit(1)
        h.update(a)
    i += 1
sys.exit(0 if h.hexdigest() == sys.argv[4] else 2)
EOF
    [ "$fs" = exfat ] || return 0
    local off=0 f
    if [ "$mode" = disk ]; then
        off=$(python3 -c "
import struct, sys
d = open(sys.argv[1], 'rb').read(3 * $ss)
if d[450] == 0xEE: print(struct.unpack('<Q', d[2 * $ss + 32:2 * $ss + 40])[0] * $ss)
else: print(struct.unpack('<I', d[454:458])[0] * $ss)" "$W/t.img")
    fi
    for f in "/a/b/eins.bin:f1" "/zwei.bin:f2"; do
        "$W/fsread_test" "$W/t.img" "$off" cat "${f%%:*}" | cmp -s - "$W/${f##*:}" || { bad "$n: Datei ${f%%:*}"; return; }
    done
    ok "$n: Dateien nach dem Wiederherstellen gleich"
}

roundtrip "exFAT MBR 512" disk exfat $((256 * 1048576)) 512 --mbr
IB_THREADS=0 roundtrip "exFAT MBR 512, ohne Hilfsthreads" disk exfat $((256 * 1048576)) 512 --mbr
roundtrip "exFAT GPT 512" disk exfat $((256 * 1048576)) 512 --gpt
roundtrip "exFAT ohne Tabelle" part exfat $((256 * 1048576)) 512
roundtrip "exFAT 4K-Sektoren" disk exfat $((256 * 1048576)) 4096 --gpt
# Sektorgröße steht im Kopf: ein Image von 4K-Sektoren auf einen 512-Byte-Datenträger wird abgelehnt (unlesbar danach)
truncate -s 0 "$W/t.img"; truncate -s $((256 * 1048576)) "$W/t.img"
out=$("$IB" restore "$W/b.img.zst" "$W/t.img" 512 2>&1)
rc=$?
written=$(head -c 1048576 "$W/t.img" | tr -d '\000' | wc -c)
[ $rc -ne 0 ] && echo "$out" | grep -q "Sektoren" && [ "$written" = 0 ] \
    && ok "andere Sektorgröße: abgelehnt, nichts geschrieben" || bad "andere Sektorgröße" "$out"
# gefälschter Stick (nur die ersten 96 MB echt): dahinter wird nichts gelesen, außer der GPT-Kopie am Ende
roundtrip "exFAT GPT Fälschung" disk exfat $((256 * 1048576)) 512 --gpt --limit=$((96 * 1048576))
roundtrip "exFAT MBR Fälschung" disk exfat $((256 * 1048576)) 512 --mbr --limit=$((96 * 1048576))
roundtrip "FAT32 MBR 512" disk fat32 $((300 * 1048576)) 512 --mbr
roundtrip "FAT32 ohne Tabelle" part fat32 $((300 * 1048576)) 512

# Angaben im Kopf
"$IB" info "$W/b.img.zst" | grep -q 'size=314572800 .*map=1 model=Test "Stick" label=DATEN fs=exFAT ident=serial:ABC created=2' \
    && ok "Kopf: Angaben gelesen" || bad "Kopf: $("$IB" info "$W/b.img.zst")"

# Dateiname
[ "$("$IB" name 'SanDisk Ultra/3.0' 'MEIN STICK' '2026-10-17 14:05')" = "SanDisk_Ultra_3_0_MEIN_STICK_2026-10-17_14_05.img.zst" ] \
    && ok "Dateiname" || bad "Dateiname: $("$IB" name 'SanDisk Ultra/3.0' 'MEIN STICK' '2026-10-17 14:05')"
[ "$("$IB" name '' '' '2026-10-17 14:05')" = "USB_2026-10-17_14_05.img.zst" ] && ok "Dateiname ohne Angaben" || bad "Dateiname leer"

# Unbekanntes Dateisystem: alles belegt
truncate -s $((64 * 1048576)) "$W/u.img"
head -c $((1048576)) /dev/urandom | dd of="$W/u.img" bs=1M seek=10 conv=notrunc status=none
[ "$("$IB" used "$W/u.img")" = $((64 * 1048576)) ] && ok "unbekanntes Dateisystem: alles gilt als belegt" || bad "unbekannt"

# zu kleines Ziel
truncate -s $((100 * 1048576)) "$W/klein.img"
"$IB" restore "$W/b.img.zst" "$W/klein.img" 2>&1 | grep -q "größer als der Datenträger" && ok "zu kleines Ziel abgelehnt" || bad "zu kleines Ziel"

# beschädigt: Byte mitten im zstd-Strom, abgeschnitten, Prüfsumme hinten falsch, gar kein Image
FB=$(stat -c %s "$W/b.img.zst")
truncate -s $((300 * 1048576)) "$W/t.img"
cp "$W/b.img.zst" "$W/x.img.zst"
printf '\xff\x00\xff' | dd of="$W/x.img.zst" bs=1 seek=$((FB - 200)) conv=notrunc status=none
"$IB" restore "$W/x.img.zst" "$W/t.img" 2>&1 | grep -q "beschädigt" && ok "beschädigte Datei erkannt" || bad "beschädigt nicht erkannt"
head -c $((FB / 2)) "$W/b.img.zst" >"$W/x.img.zst"
"$IB" restore "$W/x.img.zst" "$W/t.img" 2>&1 | grep -q "unvollständig" && ok "abgeschnittene Datei erkannt" || bad "abgeschnitten nicht erkannt"
cp "$W/b.img.zst" "$W/x.img.zst"
printf '0000' | dd of="$W/x.img.zst" bs=1 seek=$((FB - 20)) conv=notrunc status=none
"$IB" restore "$W/x.img.zst" "$W/t.img" 2>&1 | grep -q "Prüfsumme des Images" && ok "falsche Prüfsumme hinten erkannt" || bad "Prüfsumme hinten"
head -c 100000 /dev/urandom >"$W/x.img.zst"
"$IB" info "$W/x.img.zst" 2>&1 | grep -q "Keine gültige Image-Datei" && ok "keine Image-Datei abgelehnt" || bad "Zufallsdaten"

# nur prüfen (ohne Datenträger): heil, Prüfsumme verglichen, SHA-256 und Größe der ganzen Datei wie sha256sum/stat
read -r vused vchk vsha vfile vbytes < <("$IB" verify "$W/b.img.zst")
read -r rused rsha < <("$IB" restore "$W/b.img.zst" "$W/t.img")
[ "$vchk" = 1 ] && [ "$vused" = "$rused" ] && [ "$vsha" = "$rsha" ] && [ "$vfile" = "$(sha256sum <"$W/b.img.zst" | cut -c1-64)" ] \
    && [ "$vbytes" = "$FB" ] && ok "Prüfen: heil, Prüfsumme verglichen, SHA-256 der Datei stimmt" || bad "Prüfen heil: $vused $vchk $vbytes"
cp "$W/b.img.zst" "$W/x.img.zst"
printf '\xff\x00\xff' | dd of="$W/x.img.zst" bs=1 seek=$((FB - 200)) conv=notrunc status=none
"$IB" verify "$W/x.img.zst" 2>&1 | grep -q "beschädigt" && ok "Prüfen: beschädigte Datei erkannt" || bad "Prüfen: beschädigt"
head -c $((FB / 2)) "$W/b.img.zst" >"$W/x.img.zst"
"$IB" verify "$W/x.img.zst" 2>&1 | grep -q "unvollständig" && ok "Prüfen: abgeschnittene Datei erkannt" || bad "Prüfen: abgeschnitten"
cp "$W/b.img.zst" "$W/x.img.zst"
printf '0000' | dd of="$W/x.img.zst" bs=1 seek=$((FB - 20)) conv=notrunc status=none
"$IB" verify "$W/x.img.zst" 2>&1 | grep -q "Prüfsumme des Images" && ok "Prüfen: falsche Prüfsumme hinten erkannt" || bad "Prüfen: Prüfsumme"
# genau die Prüfsumme hinten fehlt: Wiederherstellen ginge, Prüfen meldet die Datei als unvollständig
python3 - "$W/b.img.zst" "$W/x.img.zst" <<'EOF'
import sys
b = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(b[:b.rindex(bytes.fromhex('5b2a4d18'))])
EOF
"$IB" verify "$W/x.img.zst" 2>&1 | grep -q "unvollständig" && ok "Prüfen: fehlende Prüfsumme hinten = unvollständig" \
    || bad "Prüfen ohne Prüfsumme hinten"

# Nachlesen liefert andere Daten (gefälschter Stick)
truncate -s $((300 * 1048576)) "$W/fake.img"
"$IB" restore "$W/b.img.zst" "$W/t.img" 512 "$W/fake.img" 2>&1 | grep -q "Prüfung nach dem Schreiben" \
    && ok "Nachlese-Fehler (Fälschung) erkannt" || bad "Nachlese-Fehler nicht erkannt"

# Fassung 1 (Sicherung aus 2.5.0: alles in einem Datenrahmen) wird weiter gelesen
FX=test/fixtures/fassung1_exfat_64m.img.zst
truncate -s $((64 * 1048576)) "$W/v1.img"
"$IB" info "$FX" | grep -q "size=67108864 .*map=1 .*label=DATEN" && "$IB" restore "$FX" "$W/v1.img" >/dev/null \
    && [ "$(sha256sum <"$W/v1.img" | cut -c1-64)" = "$(cat test/fixtures/fassung1_exfat_64m.sha256)" ] \
    && "$W/fsread_test" "$W/v1.img" 1048576 cat /alt/zahlen.txt | cmp -s - <(seq 1 20000) \
    && ok "Sicherung der Fassung 1 (2.5.0) wiederhergestellt" || bad "Fassung 1"
"$IB" verify "$FX" | grep -q " 1 " && ok "Fassung 1 lässt sich prüfen" || bad "Fassung 1 prüfen"

# große, fast leere Platte: lange freie Strecken als Null-Rahmen - nicht gepackt, beim Wiederherstellen übersprungen
G=$((16 * 1024 * 1048576))
truncate -s $G "$W/g.img"
"$W/mkfs_test" disk "$W/g.img" exfat $G 512 GROSS 0 --gpt "--file=/zwei.bin=$W/f2" >/dev/null 2>&1 || bad "16 GiB: mkfs"
ms() { echo $(( $(date +%s%N) / 1000000 )); }
t0=$(ms)
read -r gused gbytes gsha _ < <("$IB" backup "$W/g.img" "$W/g.img.zst")
t1=$(ms)
nz=$(python3 -c "import sys; print(open(sys.argv[1], 'rb').read().count(bytes.fromhex('5c2a4d18')))" "$W/g.img.zst")
[ "$nz" -ge 1 ] && [ "$gbytes" -lt $((2 * 1048576)) ] && [ $((t1 - t0)) -lt 20000 ] \
    && ok "16 GiB fast leer gesichert: $nz Null-Rahmen, Datei $((gbytes / 1024)) KB, $((t1 - t0)) ms" \
    || bad "16 GiB sichern: $nz Null-Rahmen, $gbytes Bytes, $((t1 - t0)) ms"
truncate -s $G "$W/gt.img"
t0=$(ms)
read -r gw gs < <("$IB" restore "$W/g.img.zst" "$W/gt.img")
t1=$(ms)
[ "$gw" = "$gused" ] && [ "$gs" = "$gsha" ] && [ $((t1 - t0)) -lt 20000 ] \
    && ok "16 GiB wiederhergestellt: $((gw / 1024)) KB geschrieben, $((t1 - t0)) ms" || bad "16 GiB wiederherstellen: $gw $gs"
t0=$(ms)
read -r vw vchk vs _ < <("$IB" verify "$W/g.img.zst")
t1=$(ms)
[ "$vw" = "$gused" ] && [ "$vchk" = 1 ] && [ "$vs" = "$gsha" ] && [ $((t1 - t0)) -lt 20000 ] \
    && ok "16 GiB geprüft (Null-Rahmen übersprungen): $((t1 - t0)) ms" || bad "16 GiB prüfen: $vw $vchk"
"$W/fsread_test" "$W/gt.img" "$(python3 -c "
import struct, sys
d = open(sys.argv[1], 'rb').read(1536)
print(struct.unpack('<Q', d[1024 + 32:1024 + 40])[0] * 512)" "$W/gt.img")" cat /zwei.bin | cmp -s - "$W/f2" \
    && ok "16 GiB: Datei nach dem Wiederherstellen gleich" || bad "16 GiB: Datei verschieden"
# Null-Rahmen verfälscht (RLE-Byte 1 statt 0): erkannt, nicht als Nullen übernommen
cp "$W/g.img.zst" "$W/gx.img.zst"
python3 - "$W/gx.img.zst" <<'EOF'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
i = b.index(bytes.fromhex('5c2a4d18'))
b[i + 36 + 14 + 3] = 1   # Hinweis (36) + Rahmenkopf (14), erster Block: Kopf (3) + RLE-Byte
open(p, 'wb').write(b)
EOF
"$IB" restore "$W/gx.img.zst" "$W/gt.img" 2>&1 | grep -q "beschädigt" && ok "verfälschter Null-Rahmen erkannt" || bad "Null-Rahmen"
head -c $(( $(python3 -c "import sys; print(open(sys.argv[1], 'rb').read().index(bytes.fromhex('5c2a4d18')))" "$W/g.img.zst") + 60 )) \
    "$W/g.img.zst" >"$W/gx.img.zst"
"$IB" restore "$W/gx.img.zst" "$W/gt.img" 2>&1 | grep -q "unvollständig" && ok "mitten im Null-Rahmen abgeschnitten: unvollständig" \
    || bad "Null-Rahmen abgeschnitten"
if command -v zstd >/dev/null; then
    zstd -q -d -c "$W/g.img.zst" | cmp -s - "$W/g.img" && ok "16 GiB: zstd -d entpackt alle Rahmen zum Original" || bad "16 GiB: zstd -d"
fi
rm -f "$W/g.img" "$W/gt.img"

# fremdes Image, mit zstd selbst gepackt (ohne Kopf: alles wird geschrieben)
if command -v zstd >/dev/null; then
    # b.img.zst und s.img sind vom letzten Durchlauf (FAT32 ohne Tabelle, freie Bereiche dort Null)
    zstd -q -d -c "$W/b.img.zst" >"$W/plain.img" && cmp -s "$W/plain.img" "$W/s.img" \
        && ok "zstd -d entpackt die Sicherung zum Original-Image" || bad "zstd -d"
    zstd -q -3 -c "$W/plain.img" >"$W/fremd.img.zst"
    # ohne Karte wird alles geschrieben - die SHA-256 gilt dann dem ganzen Image
    "$IB" restore "$W/fremd.img.zst" "$W/t.img" | grep -q "^$((300 * 1048576)) $(sha256sum <"$W/plain.img" | cut -c1-64)" \
        && cmp -s "$W/t.img" "$W/s.img" && ok "fremdes zstd-Image: alles geschrieben" || bad "fremdes zstd-Image"
    "$IB" verify "$W/fremd.img.zst" | grep -q "^$((300 * 1048576)) 0 $(sha256sum <"$W/plain.img" | cut -c1-64) " \
        && ok "fremdes zstd-Image prüfen: entpackt, ohne eigene Prüfsumme" || bad "fremdes zstd-Image prüfen"
    cat "$W/fremd.img.zst" "$W/fremd.img.zst" >"$W/zwei.img.zst"   # mehrere Rahmen hintereinander (wie pzstd)
    "$IB" restore "$W/zwei.img.zst" "$W/t.img" 2>&1 | grep -q "mehreren zstd-Rahmen" \
        && ok "mehrere zstd-Rahmen: nicht als fertig gemeldet" || bad "mehrere zstd-Rahmen"
    zstd -q -c <"$W/plain.img" >"$W/pipe.img.zst"   # über Pipe: ohne Größenangabe
    "$IB" restore "$W/pipe.img.zst" "$W/t.img" 2>&1 | grep -q "Größe des Images ist unbekannt" \
        && ok "zstd ohne Größe abgelehnt" || bad "zstd ohne Größe"
else
    echo "  (zstd fehlt - Prüfung mit zstd -d übersprungen)"
fi

[ $FAIL = 0 ] && echo "Image-Sicherung: alle Prüfungen bestanden" || echo "Image-Sicherung: FEHLER"
exit $FAIL
