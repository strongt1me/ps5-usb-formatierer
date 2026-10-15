#!/usr/bin/env bash
# Neue Funktionen über die API: Belegung, Spielebibliothek mit Duplikaten, Ordner-Vorlage, Echtheits-/Tempoprüfung
# (echt und gefälscht, danach nur den echten Bereich formatieren), vollständig formatieren, GPT, Lesetempo,
# exFAT prüfen/reparieren, Bezeichnung ändern, Abbrechen und Fehlerfälle.
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18095 || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/fsread_test" test/fsread_test.c src/fsread.c || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c || exit 1

U=http://127.0.0.1:18095
J='Content-Type: application/json'
MIB=1048576
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
post() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$1" "$U/api/$2"; }
wait_job() { for _ in $(seq 1 600); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
size_of() { stat -c %s "$1"; }
start() {   # start <images> <eingebunden> [weitere Umgebung]
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    env USBFMT_IMAGES="$1" USBFMT_MOUNTED="$2" USBFMT_MOUNTROOT="$W/mnt" USBFMT_INTERNAL="$W/data/homebrew" \
        USBFMT_DATA="$W/cfg" "${@:3}" "$W/srv" >>"$W/log" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && return; sleep 0.1; done
}

# ---------------------------------------------------------------- Bibliothek und Belegung
mkdir -p "$W/mnt/usb0/homebrew/Astro/sce_sys" "$W/mnt/usb0/etaHEN/games" "$W/data/homebrew/Astro2/sce_sys"
printf '{"titleId":"PPSA01325","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Astro Bot"}}}' \
    | tee "$W/mnt/usb0/homebrew/Astro/sce_sys/param.json" >"$W/data/homebrew/Astro2/sce_sys/param.json"
printf '{"titleId":"PPSA03117","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Crazy Chicken"}}}' >"$W/cc.json"
"$W/mkfs_test" part "$W/mnt/usb0/homebrew/Crazy.exfat" exfat $((64 * MIB)) 512 "" 4096 --file=sce_sys/param.json="$W/cc.json" >/dev/null
head -c 5000 /dev/zero >"$W/mnt/usb0/spiel.pkg"
truncate -s 1G "$W/a.img"
"$W/mkfs_test" disk "$W/a.img" exfat $((1024 * MIB)) 512 SPIELE 0 --dir=homebrew >/dev/null
truncate -s 1G "$W/b.img"
start "$W/a.img:$W/b.img" "1,0"

curl -s "$U/api/devices" >"$W/dev"
[ "$(q 'd["devices"][0]["label"]' <"$W/dev")" = SPIELE ] && [ "$(q 'd["devices"][0]["scheme"]' <"$W/dev")" = MBR ] \
    && ok "Gerät: Bezeichnung SPIELE, Partitionstabelle MBR" || bad "Bezeichnung/Partitionstabelle" "$(cat "$W/dev")"
[ "$(q 'd["devices"][0]["mounts"][0]["total"] > 0 and d["devices"][0]["mounts"][0]["free"] > 0' <"$W/dev")" = True ] \
    && ok "Belegung der Einbindung (total/free)" || bad "Belegung" "$(cat "$W/dev")"

for _ in $(seq 1 50); do curl -s "$U/api/library" >"$W/lib"; grep -q '"scanning":false' "$W/lib" && grep -q PPSA03117 "$W/lib" && break; sleep 0.1; done
[ "$(q 'len([t for t in d["titles"] if t["device"] == "da0"])' <"$W/lib")" = 2 ] && ok "Bibliothek: 2 Spiele auf da0" \
    || bad "Bibliothek da0" "$(cat "$W/lib")"
[ "$(q '[r["pkgs"] for r in d["roots"] if r["device"] == "da0"][0]' <"$W/lib")" = 1 ] && ok "Bibliothek: 1 PKG-Datei" || bad "PKG"
[ "$(q 'd["duplicates"][0]["id"] + "/" + str(len(d["duplicates"][0]["paths"]))' <"$W/lib")" = "PPSA01325/2" ] \
    && ok "Duplikat USB/intern erkannt: PPSA01325 zweimal" || bad "Duplikat" "$(cat "$W/lib")"
grep -q '"folders":"etaHEN/homebrew"\|"folders":"homebrew/etaHEN"' "$W/lib" && ok "Ordner der Wurzel" || bad "Ordner der Wurzel" "$(cat "$W/lib")"

# ---------------------------------------------------------------- Formatieren mit Vorlage und Prüfung (echt)
code=$(post "{\"device\":\"da1\",\"fs\":\"exfat\",\"label\":\"NEU\",\"size\":$(size_of "$W/b.img"),\"verify\":true,\"folders\":true}" format)
[ "$code" = 202 ] && wait_job || bad "Format mit Prüfung angenommen" "$code $(cat "$W/body")"
[ "$(q 'd["ok"] and d["kind"] == "format" and d["result"]["test"] == "quick" and d["result"]["read"] > 0 and d["result"]["folders"]' <"$W/st")" = True ] \
    && ok "Format mit Prüfung und Vorlage: $(q 'd["message"]' <"$W/st" | cut -c1-150)" || bad "Format mit Prüfung" "$(cat "$W/st")"
for d in homebrew etaHEN/games pkg; do
    "$W/fsread_test" "$W/b.img" $MIB isdir "$d" && ok "  Ordner $d angelegt" || bad "  Ordner $d"
done
dd if="$W/b.img" of="$W/p.img" bs=1M skip=1 status=none && fsck.exfat -n "$W/p.img" >/dev/null 2>&1 && ok "  fsck.exfat sauber" || bad "  fsck.exfat"

# ---------------------------------------------------------------- Lesetempo, Umbenennen, Prüfen/Reparieren
code=$(post "{\"device\":\"da0\",\"size\":$(size_of "$W/a.img")}" speed)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"] and d["result"]["read"] > 0' <"$W/st")" = True ] \
    && ok "Lesetempo (eingebunden, nur lesen): $(q 'd["message"]' <"$W/st")" || bad "Lesetempo" "$(cat "$W/st")"
code=$(post "{\"device\":\"da0\",\"size\":$(size_of "$W/a.img"),\"label\":\"UMBENANNT\"}" label)
[ "$code" = 409 ] && ok "Umbenennen eingebunden ohne replug: 409" || bad "Umbenennen ohne replug" "$code"
code=$(post "{\"device\":\"da0\",\"size\":$(size_of "$W/a.img"),\"label\":\"UMBENANNT\",\"replug\":true}" label)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] \
    && [ "$(curl -s "$U/api/devices" | q 'd["devices"][0]["label"]')" = UMBENANNT ] \
    && ok "Umbenennen: $(q 'd["message"]' <"$W/st")" || bad "Umbenennen" "$(cat "$W/st")"
code=$(post "{\"device\":\"da0\",\"size\":$(size_of "$W/a.img"),\"replug\":true}" check)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["result"]["code"]' <"$W/st")" = 0 ] \
    && ok "Prüfen: $(q 'd["message"]' <"$W/st")" || bad "Prüfen sauber" "$(cat "$W/st")"
python3 - "$W/a.img" <<'EOF'
import sys
f = open(sys.argv[1], 'r+b'); d = f.read(64 << 20)
s = d.find('homebrew'.encode('utf-16-le')) - 2 - 64   # Eintragssatz des Ordners: Prüfsumme verfälschen
assert d[s] == 0x85
f.seek(s + 2); f.write(bytes([d[s + 2] ^ 0xFF]))
EOF
code=$(post "{\"device\":\"da0\",\"size\":$(size_of "$W/a.img"),\"replug\":true}" check)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["result"]["code"]' <"$W/st")" != 0 ] \
    && ok "Prüfen findet Fehler: $(q 'd["message"]' <"$W/st")" || bad "Prüfen findet Fehler" "$(cat "$W/st")"
code=$(post "{\"device\":\"da0\",\"size\":$(size_of "$W/a.img"),\"replug\":true,\"repair\":true}" check)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["result"]["code"]' <"$W/st")" = 1 ] \
    && ok "Reparieren: $(q 'd["message"]' <"$W/st")" || bad "Reparieren" "$(cat "$W/st")"
code=$(post "{\"device\":\"da0\",\"size\":$(size_of "$W/a.img"),\"replug\":true}" check)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["result"]["code"]' <"$W/st")" = 0 ] && ok "danach sauber" || bad "danach sauber" "$(cat "$W/st")"

# ---------------------------------------------------------------- GPT, vollständig, Fehlerfälle
code=$(post "{\"device\":\"da1\",\"fs\":\"fat32\",\"label\":\"G\",\"size\":$(size_of "$W/b.img"),\"scheme\":\"gpt\",\"folders\":true}" format)
[ "$code" = 202 ] && wait_job && [ "$(curl -s "$U/api/devices" | q 'd["devices"][1]["scheme"]')" = GPT ] \
    && ok "FAT32 mit GPT: $(q 'd["message"]' <"$W/st" | cut -c1-140)" || bad "GPT" "$(cat "$W/st")"
sgdisk -v "$W/b.img" 2>&1 | grep -q 'No problems found' && ok "  sgdisk -v ohne Befund" || bad "  sgdisk -v"
code=$(post "{\"device\":\"da1\",\"size\":$(size_of "$W/b.img")}" check)
[ "$code" = 400 ] && ok "Prüfen bei FAT32 abgelehnt (400)" || bad "Prüfen FAT32" "$code"
code=$(post "{\"device\":\"da1\",\"fs\":\"exfat\",\"size\":$(size_of "$W/b.img"),\"scheme\":\"apm\"}" format)
[ "$code" = 400 ] && ok "unbekanntes Partitionsschema abgelehnt" || bad "scheme apm" "$code"
code=$(post "{\"device\":\"da1\",\"fs\":\"exfat\",\"size\":$(size_of "$W/b.img"),\"limit\":1000}" format)
[ "$code" = 400 ] && ok "zu kleines limit abgelehnt" || bad "limit 1000" "$code"

truncate -s 256M "$W/c.img"
start "$W/c.img" "0"
code=$(post "{\"device\":\"da0\",\"fs\":\"exfat\",\"size\":$(size_of "$W/c.img"),\"full\":true,\"folders\":true}" format)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"] and d["result"]["test"] == "full"' <"$W/st")" = True ] \
    && ok "vollständig formatiert: $(q 'd["message"]' <"$W/st" | cut -c1-160)" || bad "vollständig" "$(cat "$W/st")"

# ---------------------------------------------------------------- Abbrechen während des vollständigen Beschreibens
truncate -s 3G "$W/d.img"
start "$W/d.img" "0" USBFMT_SLOW=1
code=$(post "{\"device\":\"da0\",\"fs\":\"exfat\",\"size\":$(size_of "$W/d.img"),\"full\":true}" format)
sleep 1.5
post '{}' cancel >/dev/null
wait_job && [ "$(q 'd["ok"] and d["result"]["test"] == "cancelled"' <"$W/st")" = True ] && grep -q abgebrochen "$W/st" \
    && ok "abgebrochen, trotzdem formatiert: $(q 'd["message"]' <"$W/st" | cut -c1-120)" || bad "Abbrechen" "$(cat "$W/st")"

# ---------------------------------------------------------------- Gefälschter Stick: erkennen, dann nur den echten Bereich
truncate -s 256M "$W/fake.img"
start "$W/fake.img" "0" USBFMT_FAKE_REAL=$((256 * MIB))
curl -s "$U/api/devices" >/dev/null
# der Host meldet die Dateigröße (256 MB) - für den Test eine 2-GB-Datei vorgaukeln: echtes Image vergrößern,
# Zugriffe hinter 256 MB laufen über (USBFMT_FAKE_REAL)
truncate -s 2G "$W/fake.img"
code=$(post "{\"device\":\"da0\",\"fs\":\"exfat\",\"size\":$((2048 * MIB)),\"verify\":true}" format)
[ "$code" = 202 ] && wait_job && [ "$(q 'not d["ok"] and d["result"]["fake"] and d["result"]["real"] == 268435456' <"$W/st")" = True ] \
    && ok "Fälschung erkannt: $(q 'd["message"]' <"$W/st" | cut -c1-150)" || bad "Fälschung" "$(cat "$W/st")"
code=$(post "{\"device\":\"da0\",\"fs\":\"exfat\",\"size\":$((2048 * MIB)),\"limit\":268435456}" format)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"] and d["result"]["limit"] == 268435456' <"$W/st")" = True ] \
    && ok "nur den echten Bereich formatiert: $(q 'd["message"]' <"$W/st" | cut -c1-150)" || bad "limit-Format" "$(cat "$W/st")"
sfdisk -d "$W/fake.img" 2>/dev/null | grep -Eq 'start= *2048, size= *522240' && ok "  Partition endet bei 256 MB" || bad "  Partition limit"
# gemerkt: /api/devices kennzeichnet den Stick (auch nach Neustart des Servers), Partition im echten Bereich
start "$W/fake.img" "0" USBFMT_FAKE_REAL=$((256 * MIB))
[ "$(curl -s "$U/api/devices" | q 'str(d["devices"][0]["fake_real"]) + "/" + str(d["devices"][0]["part_end"] <= d["devices"][0]["fake_real"])')" = "268435456/True" ] \
    && ok "Fälschung gemerkt (nach Neustart): fake_real 256 MB, Partition im echten Bereich" || bad "Fälschung merken" "$(curl -s "$U/api/devices")"
# ganzer Stick erneut geprüft und echt: Kennzeichnung verschwindet
start "$W/fake.img" "0"
code=$(post "{\"device\":\"da0\",\"fs\":\"exfat\",\"size\":$((2048 * MIB)),\"verify\":true}" format)
[ "$code" = 202 ] && wait_job && [ "$(curl -s "$U/api/devices" | q 'd["devices"][0]["fake_real"]')" = 0 ] \
    && ok "als echt geprüft: Kennzeichnung entfernt" || bad "Kennzeichnung entfernen" "$(cat "$W/st")"

# ---------------------------------------------------------------- alte Sticks: Partition ab Sektor 63 (nicht auf 4 KiB)
truncate -s 64M "$W/s63.img"
python3 - "$W/s63.img" <<'EOF'
import struct, sys
mbr = bytearray(512)
mbr[446:462] = bytes([0, 0, 0, 0, 0x0C, 0, 0, 0]) + struct.pack("<II", 63, 131072 - 63)
mbr[510:512] = b"\x55\xaa"
with open(sys.argv[1], "r+b") as f:
    f.write(mbr)
EOF
start "$W/s63.img" "0"
[ "$(curl -s "$U/api/devices" | q 'd["devices"][0]["format"] + "/" + d["devices"][0]["scheme"]')" = "leer/MBR" ] \
    && ok "Partition ab Sektor 63, leer: erkannt (ohne über den Puffer zu lesen)" || bad "Sektor 63" "$(curl -s "$U/api/devices")"

echo
[ $fails -eq 0 ] && echo "Alle Funktions-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
