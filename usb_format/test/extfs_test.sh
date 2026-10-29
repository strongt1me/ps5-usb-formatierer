#!/usr/bin/env bash
# NTFS/ext4 über die API der Linux-Testversion: mkntfs/mke2fs laufen, Ergebnis wird mit ntfsinfo/ntfsls/e2fsck geprüft.
# Werkzeuge: e2fsprogs (System), ntfs-3g unter ~/fstools/root (apt-get download ntfs-3g libntfs-3g89t64 + dpkg -x).
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$T/sbin:$PATH"
export LD_LIBRARY_PATH="$T/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18093 || exit 1
truncate -s 2G "$W/a.img"
truncate -s 600M "$W/b.img"
USBFMT_IMAGES="$W/a.img:$W/b.img" USBFMT_DATA="$W/data" "$W/srv" >"$W/log" 2>&1 &
PID=$!
sleep 0.5
U=http://127.0.0.1:18093
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; fails=$((fails+1)); }
chk() { if eval "$2" >"$W/out" 2>&1; then ok "$1"; else bad "$1"; sed 's/^/        /' "$W/out" | head -15; fi; }

format() {  # format <dev> <fs> <label> <cluster> <size>
    curl -s -o "$W/resp" -w '%{http_code}' -H 'Content-Type: application/json' \
        -d "{\"device\":\"$1\",\"fs\":\"$2\",\"label\":\"$3\",\"cluster\":$4,\"size\":$5}" $U/api/format
}
wait_done() {
    for _ in $(seq 1 120); do
        curl -s $U/api/status >"$W/status"
        grep -q '"busy":false' "$W/status" && return 0
        sleep 0.5
    done
    return 1
}
part() { dd if="$1" of="$W/part.img" bs=1M skip=1 status=none; }

echo "== NTFS (2 GB, Standard-Cluster, Bezeichnung mit Leerzeichen) =="
code=$(format da0 ntfs "Mein NTFS Laufwerk" 0 $(stat -c %s "$W/a.img"))
[ "$code" = 202 ] && ok "Auftrag angenommen" || { bad "Auftrag: $code $(cat "$W/resp")"; }
wait_done; echo "        $(sed 's/.*"message":"\([^"]*\)".*/\1/' "$W/status")"
chk "Formatierung erfolgreich" "grep -q '\"ok\":true' '$W/status'"
chk "MBR-Typ 07 ab Sektor 2048" "sfdisk -d '$W/a.img' | grep -Eq 'start= *2048, size= *[0-9]+, type=7\$'"
part "$W/a.img"
chk "ntfsinfo liest das Volume" "ntfsinfo -m '$W/part.img' | grep -q 'Volume Name'"
chk "Bezeichnung 'Mein NTFS Laufwerk'" "ntfslabel '$W/part.img' | grep -qx 'Mein NTFS Laufwerk'"
chk "Clustergröße 4096 (Standard)" "ntfsinfo -m '$W/part.img' | grep -Eq 'Cluster Size: *4096'"
chk "Wurzelverzeichnis lesbar (ntfsls)" "ntfsls '$W/part.img'"
chk "Hidden Sectors im Bootsektor = 2048" "test \$(od -An -tu4 -j28 -N4 '$W/part.img') -eq 2048"

echo "== NTFS mit 64 KB Clustern =="
code=$(format da0 ntfs "" 65536 $(stat -c %s "$W/a.img")); wait_done
chk "Formatierung erfolgreich" "grep -q '\"ok\":true' '$W/status'"
part "$W/a.img"
chk "Clustergröße 65536" "ntfsinfo -m '$W/part.img' | grep -Eq 'Cluster Size: *65536'"

echo "== ext4 (600 MB, Standard) =="
code=$(format da1 ext4 "LINUXDATEN" 0 $(stat -c %s "$W/b.img"))
[ "$code" = 202 ] && ok "Auftrag angenommen" || bad "Auftrag: $code $(cat "$W/resp")"
wait_done; echo "        $(sed 's/.*"message":"\([^"]*\)".*/\1/' "$W/status")"
chk "Formatierung erfolgreich" "grep -q '\"ok\":true' '$W/status'"
chk "MBR-Typ 83" "sfdisk -d '$W/b.img' | grep -Eq 'start= *2048, size= *[0-9]+, type=83\$'"
part "$W/b.img"
chk "e2fsck ohne Befund" "e2fsck -fn '$W/part.img'"
chk "Bezeichnung LINUXDATEN" "e2label '$W/part.img' | grep -qx LINUXDATEN"
chk "Journal vorhanden (ext4)" "dumpe2fs -h '$W/part.img' 2>/dev/null | grep -q has_journal"
chk "extents aktiv (ext4)" "dumpe2fs -h '$W/part.img' 2>/dev/null | grep -q extent"

echo "== ext4 mit 1 KB Blöcken =="
code=$(format da1 ext4 "" 1024 $(stat -c %s "$W/b.img")); wait_done
chk "Formatierung erfolgreich" "grep -q '\"ok\":true' '$W/status'"
part "$W/b.img"
chk "Blockgröße 1024" "dumpe2fs -h '$W/part.img' 2>/dev/null | grep -Eq 'Block size: *1024'"
chk "e2fsck ohne Befund" "e2fsck -fn '$W/part.img'"

echo "== Ungültige Angaben =="
chk "ext4 mit 8 KB Blöcken abgelehnt (400)" "test \$(format da1 ext4 '' 8192 $(stat -c %s "$W/b.img")) = 400"
chk "NTFS-Bezeichnung > 32 Zeichen abgelehnt" "test \$(format da0 ntfs 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456' 0 $(stat -c %s "$W/a.img")) = 400"
chk "ext4-Bezeichnung > 16 Zeichen abgelehnt" "test \$(format da1 ext4 'ABCDEFGHIJKLMNOPQ' 0 $(stat -c %s "$W/b.img")) = 400"
chk "unbekanntes Dateisystem abgelehnt" "test \$(format da1 btrfs '' 0 $(stat -c %s "$W/b.img")) = 400"

echo
[ $fails -eq 0 ] && echo "Alle NTFS/ext4-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
