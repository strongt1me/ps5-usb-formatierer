#!/usr/bin/env bash
# Image-Sicherung über die API: Orte und Liste, Sichern nach intern und auf ein anderes USB-Laufwerk, Download mit
# Einmal-Zeichen, Wiederherstellen aus Datei und per Hochladen, Abbrechen und die Schutzregeln (Ziel = Quelle, Pfade
# außerhalb, zu kleines Ziel, gefälschter Stick, fremder Ursprung, falscher Inhalt), Sicherungen prüfen und löschen.
set -u
cd "$(dirname "$0")/.."
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
W=$(mktemp -d)
PID=
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
bash test/host_build.sh "$W/srv" -DSERVER_PORT=18096 || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/fsread_test" test/fsread_test.c src/fsread.c || exit 1
gcc -Wall -Wextra -Werror -O2 -o "$W/mkfs_test" test/mkfs_test.c src/mkfs.c src/hfsplus.c src/upcase.c src/i18n.c src/i18n_table.c || exit 1

U=http://127.0.0.1:18096
J='Content-Type: application/json'
MIB=1048576
fails=0
ok()  { echo "OK      $1"; }
bad() { echo "FEHLER  $1"; [ -n "${2:-}" ] && echo "        $2"; fails=$((fails + 1)); }
q()   { python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"; }
post() { curl -s -o "$W/body" -w '%{http_code}' -H "$J" -d "$1" "$U/api/$2"; }
wait_job() { for _ in $(seq 1 600); do curl -s "$U/api/status" >"$W/st"; grep -q '"busy":false' "$W/st" && return 0; sleep 0.2; done; return 1; }
start() {   # start <images> <eingebunden> [weitere Umgebung]
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    env USBFMT_IMAGES="$1" USBFMT_MOUNTED="$2" USBFMT_MOUNTROOT="$W/mnt" USBFMT_DATA="$W/data/usb_format" \
        "${@:3}" "$W/srv" >>"$W/log" 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do curl -s "$U/api/info" >/dev/null 2>&1 && return; sleep 0.1; done
}
same_files() {   # same_files <abbild> -> die beiden Dateien auf dem exFAT darin gleich den Vorlagen?
    "$W/fsread_test" "$1" $((MIB)) cat /a/b/eins.bin | cmp -s - "$W/f1" && "$W/fsread_test" "$1" $((MIB)) cat /zwei.bin | cmp -s - "$W/f2"
}

head -c $((3 * MIB + 777)) /dev/urandom >"$W/f1"   # fsread_test cat liefert höchstens 4 MiB
head -c $((300 * 1024)) /dev/urandom >"$W/f2"
mkdir -p "$W/mnt/usb0" "$W/mnt/usb1" "$W/data/usb_format"
truncate -s 1G "$W/a.img"
"$W/mkfs_test" disk "$W/a.img" exfat $((1024 * MIB)) 512 QUELLE 0 --mbr --dir=/a/b "--file=/a/b/eins.bin=$W/f1" \
    "--file=/zwei.bin=$W/f2" >/dev/null || { echo "mkfs fehlgeschlagen"; exit 1; }
head -c $((1024 * MIB)) /dev/urandom >"$W/b.img"   # Ziel voller Zufall: nur die belegten Blöcke werden geschrieben
truncate -s 512M "$W/c.img"
SA=$(stat -c %s "$W/a.img"); SB=$(stat -c %s "$W/b.img"); SC=$(stat -c %s "$W/c.img")
start "$W/a.img:$W/b.img:$W/c.img" "1,1,0" USBFMT_MODELS="SanDisk Ultra|Kingston DT|Klein"

# ---------------------------------------------------------------- Orte
curl -s "$U/api/images" >"$W/img"
[ "$(q '[l["dir"] for l in d["locations"]]' <"$W/img")" = "['$W/data/usbfmt-backup', '$W/mnt/usb0/usbfmt-backup', '$W/mnt/usb1/usbfmt-backup']" ] \
    && ok "Orte: intern und beide eingebundenen Laufwerke" || bad "Orte" "$(cat "$W/img")"
[ "$(q 'd["locations"][1]["device"] + " " + str(d["locations"][0]["free"] > 0)' <"$W/img")" = "da0 True" ] \
    && ok "Orte: Gerät und freier Platz" || bad "Orte: Gerät/frei" "$(cat "$W/img")"

# ---------------------------------------------------------------- Sichern nach intern
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"$W/data/usbfmt-backup\"}" backup)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && ok "Sichern nach intern: $(q 'd["message"]' <"$W/st")" \
    || bad "Sichern nach intern ($code)" "$(cat "$W/body" "$W/st")"
IMG=$(q 'd["result"]["file"]' <"$W/st")
[ -s "$IMG" ] && [ -z "$(ls "$W/data/usbfmt-backup"/*.part 2>/dev/null)" ] && ok "Datei $(basename "$IMG"), keine .part übrig" || bad "Datei fehlt"
case "$(basename "$IMG")" in SanDisk_Ultra_QUELLE_20*.img.zst) ok "Name aus Modell, Bezeichnung und Zeit";; *) bad "Name: $IMG";; esac
(cd "$(dirname "$IMG")" && sha256sum -c --quiet "$(basename "$IMG").sha256") && ok ".sha256 stimmt" || bad ".sha256"
[ "$(stat -c %s "$IMG")" -lt $((7 * MIB)) ] && ok "klein: $(($(stat -c %s "$IMG") / 1024)) KB für 1 GiB mit 3,3 MB Daten" || bad "zu groß"
[ "$(q 'd["result"]["sha256"]' <"$W/st")" = "$(sha256sum <"$IMG" | cut -c1-64)" ] && ok "Ergebnis nennt die SHA-256 der Datei" || bad "SHA im Ergebnis"
if command -v zstd >/dev/null; then
    zstd -q -d -c "$IMG" | cmp -s - "$W/a.img" && ok "zstd -d ergibt das Abbild" || bad "zstd -d"
fi

# zweimal in derselben Minute: Nummer anhängen statt überschreiben
post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"$W/data/usbfmt-backup\"}" backup >/dev/null; wait_job
[ "$(ls "$W/data/usbfmt-backup"/*.img.zst | wc -l)" = 2 ] && ok "zweite Sicherung: eigener Name ($(q 'd["result"]["name"]' <"$W/st"))" || bad "zweite Sicherung"

# auf ein anderes USB-Laufwerk
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"$W/mnt/usb1/usbfmt-backup\"}" backup)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && [ -n "$(ls "$W/mnt/usb1/usbfmt-backup"/*.img.zst)" ] \
    && ok "Sichern auf anderes USB-Laufwerk" || bad "Sichern auf USB ($code)" "$(cat "$W/body" "$W/st")"

# Schutzregeln
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"$W/mnt/usb0/usbfmt-backup\"}" backup)
[ "$code" = 400 ] && grep -q "nicht auf dem Datenträger liegen" "$W/body" && ok "Ziel = Quelle abgelehnt" || bad "Ziel = Quelle ($code)" "$(cat "$W/body")"
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"/etc\"}" backup)
[ "$code" = 400 ] && ok "fremder Ordner abgelehnt" || bad "fremder Ordner ($code)" "$(cat "$W/body")"
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"dest\":\"$W/data/usbfmt-backup\"}" backup)
[ "$code" = 409 ] && ok "eingebunden ohne replug abgelehnt" || bad "ohne replug ($code)"
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"anderer\",\"replug\":true,\"dest\":\"$W/data/usbfmt-backup\"}" backup)
[ "$code" = 409 ] && ok "anderer Stick (Seriennummer) abgelehnt" || bad "Seriennummer ($code)"

# Liste
curl -s "$U/api/images" >"$W/img"
[ "$(q 'len(d["images"])' <"$W/img")" = 3 ] && [ "$(q 'd["images"][0]["model"] + "|" + d["images"][0]["label"] + "|" + str(d["images"][0]["size"]) + "|" + str(d["images"][0]["own"]) + "|" + str(d["images"][0]["sha256"])' <"$W/img")" = "SanDisk Ultra|QUELLE|$SA|True|True" ] \
    && ok "Liste: 3 Sicherungen mit Angaben" || bad "Liste" "$(cat "$W/img")"

# ---------------------------------------------------------------- Download
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"download\"}" backup)
TOK=$(q 'd["token"]' <"$W/body")
[ "$code" = 200 ] && [ ${#TOK} = 32 ] && ok "Download: Einmal-Zeichen" || bad "Download-Zeichen ($code)" "$(cat "$W/body")"
hdr=$(curl -s -D - -o "$W/dl.img.zst" "$U/api/backup/download?token=$TOK")
echo "$hdr" | grep -qi 'content-disposition: attachment; filename="SanDisk_Ultra_QUELLE_' && echo "$hdr" | grep -qi "transfer-encoding: chunked" \
    && ok "Download: Dateiname und Chunked-Übertragung" || bad "Download-Kopf" "$hdr"
wait_job
[ "$(q 'd["ok"]' <"$W/st")" = True ] && [ "$(q 'd["result"]["sha256"]' <"$W/st")" = "$(sha256sum <"$W/dl.img.zst" | cut -c1-64)" ] \
    && ok "Download vollständig (SHA-256 wie gemeldet)" || bad "Download" "$(cat "$W/st")"
code=$(curl -s -o "$W/body" -w '%{http_code}' "$U/api/backup/download?token=$TOK")
[ "$code" = 404 ] && ok "Zeichen nur einmal gültig" || bad "Zeichen zweimal ($code)"
code=$(curl -s -o "$W/body" -w '%{http_code}' "$U/api/backup/download?token=0123456789abcdef0123456789abcdef")
[ "$code" = 404 ] && ok "falsches Zeichen abgelehnt" || bad "falsches Zeichen ($code)"

# ---------------------------------------------------------------- Wiederherstellen aus Datei
code=$(post "{\"device\":\"da1\",\"size\":$SB,\"ident\":\"b.img\",\"replug\":true,\"path\":\"$IMG\"}" restore)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && ok "Wiederherstellen: $(q 'd["message"]' <"$W/st")" \
    || bad "Wiederherstellen ($code)" "$(cat "$W/body" "$W/st")"
same_files "$W/b.img" && ok "wiederhergestellt: Dateien gleich" || bad "Dateien verschieden"
if command -v fsck.exfat >/dev/null; then
    dd if="$W/b.img" of="$W/p.img" bs=1M skip=1 status=none
    fsck.exfat -n "$W/p.img" >/dev/null 2>&1 && ok "wiederhergestellt: fsck.exfat sauber" || bad "fsck.exfat"
    rm -f "$W/p.img"
fi
code=$(post "{\"device\":\"da2\",\"size\":$SC,\"ident\":\"c.img\",\"path\":\"$IMG\"}" restore)
[ "$code" = 400 ] && grep -q "größer als der Datenträger" "$W/body" && ok "zu kleines Ziel abgelehnt" || bad "zu klein ($code)" "$(cat "$W/body")"
cp "$IMG" "$W/mnt/usb1/usbfmt-backup/auf_b.img.zst"
code=$(post "{\"device\":\"da1\",\"size\":$SB,\"ident\":\"b.img\",\"replug\":true,\"path\":\"$W/mnt/usb1/usbfmt-backup/auf_b.img.zst\"}" restore)
[ "$code" = 400 ] && grep -q "liegt auf dem Datenträger" "$W/body" && ok "Image auf dem Ziel selbst abgelehnt" || bad "Image auf Ziel ($code)" "$(cat "$W/body")"
cp "$IMG" "$W/fremd.img.zst"
for p in "$W/fremd.img.zst" "$W/data/usbfmt-backup/../../fremd.img.zst" "/etc/passwd" "$W/a.img"; do
    code=$(post "{\"device\":\"da2\",\"size\":$SC,\"ident\":\"c.img\",\"path\":\"$p\"}" restore)
    [ "$code" = 400 ] && ok "Pfad außerhalb abgelehnt: ${p#$W/}" || bad "Pfad außerhalb ($code): $p"
done
ln -s "$W/fremd.img.zst" "$W/data/usbfmt-backup/link.img.zst"
code=$(post "{\"device\":\"da2\",\"size\":$SC,\"ident\":\"c.img\",\"path\":\"$W/data/usbfmt-backup/link.img.zst\"}" restore)
[ "$code" = 400 ] && ok "Verknüpfung nach außen abgelehnt" || bad "Verknüpfung ($code)"
head -c 50000 /dev/urandom >"$W/data/usbfmt-backup/kaputt.img.zst"
code=$(post "{\"device\":\"da2\",\"size\":$SC,\"ident\":\"c.img\",\"path\":\"$W/data/usbfmt-backup/kaputt.img.zst\"}" restore)
[ "$code" = 400 ] && grep -q "Keine gültige Image-Datei" "$W/body" && ok "keine Image-Datei abgelehnt" || bad "kaputt ($code)" "$(cat "$W/body")"
[ "$(head -c $((4 * MIB)) "$W/c.img" | tr -d '\000' | wc -c)" = 0 ] && ok "abgelehnt: Ziel unberührt" || bad "Ziel beschrieben"
# gefälschter Stick: echter Teil zu klein für das Image
printf 'b.img|%s\t%s\n' "$SB" $((300 * MIB)) >"$W/data/usb_format/fakes.txt"
code=$(post "{\"device\":\"da1\",\"size\":$SB,\"ident\":\"b.img\",\"replug\":true,\"path\":\"$IMG\"}" restore)
[ "$code" = 400 ] && grep -q "echten Speicher" "$W/body" && ok "gefälschter Stick: Image passt nicht in den echten Teil" || bad "Fälschung ($code)" "$(cat "$W/body")"
rm -f "$W/data/usb_format/fakes.txt"

# ---------------------------------------------------------------- Prüfen und Löschen
vpost() { code=$(post "{\"path\":\"$1\"}" "images/$2"); }
vpost "$IMG" verify
[ "$code" = 202 ] && wait_job && [ "$(q 'd["kind"] + str(d["ok"]) + str(d["result"]["checked"]) + d["result"]["side"]' <"$W/st")" = verifyTrueTrueok ] \
    && [ "$(q 'd["result"]["sha256"]' <"$W/st")" = "$(sha256sum <"$IMG" | cut -c1-64)" ] \
    && ok "Prüfen: $(q 'd["message"]' <"$W/st")" || bad "Prüfen ($code)" "$(cat "$W/body" "$W/st")"
D="$W/data/usbfmt-backup"
cp "$IMG" "$D/defekt.img.zst"
FB=$(stat -c %s "$IMG")
printf '\xff\x00\xff\x00' | dd of="$D/defekt.img.zst" bs=1 seek=$((FB / 2)) conv=notrunc status=none
vpost "$D/defekt.img.zst" verify
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q "fehlerhaft" "$W/st" \
    && ok "Prüfen: beschädigte Sicherung erkannt" || bad "Prüfen beschädigt ($code)" "$(cat "$W/st")"
cp "$IMG" "$D/anders.img.zst"
echo "0000000000000000000000000000000000000000000000000000000000000000  anders.img.zst" >"$D/anders.img.zst.sha256"
vpost "$D/anders.img.zst" verify
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q ".sha256-Datei" "$W/st" \
    && ok "Prüfen: passt nicht zur .sha256 erkannt" || bad "Prüfen .sha256 ($code)" "$(cat "$W/st")"
for p in "/etc/passwd" "$W/fremd.img.zst" "$D/../../fremd.img.zst" "$D/link.img.zst"; do
    vpost "$p" verify
    [ "$code" = 400 ] && ok "Prüfen: Pfad außerhalb abgelehnt: ${p#$W/}" || bad "Prüfen außerhalb ($code): $p"
    vpost "$p" delete
    [ "$code" = 400 ] && ok "Löschen: Pfad außerhalb abgelehnt: ${p#$W/}" || bad "Löschen außerhalb ($code): $p"
done
[ -f /etc/passwd ] && [ -f "$W/fremd.img.zst" ] && [ -L "$D/link.img.zst" ] && ok "abgelehnt: nichts gelöscht" || bad "doch gelöscht"
vpost "$D/anders.img.zst" delete
[ "$code" = 200 ] && [ ! -e "$D/anders.img.zst" ] && [ ! -e "$D/anders.img.zst.sha256" ] \
    && ok "Löschen: Sicherung samt .sha256 gelöscht" || bad "Löschen ($code)" "$(cat "$W/body"; ls "$D")"
vpost "$D/defekt.img.zst" delete
[ "$code" = 200 ] && [ ! -e "$D/defekt.img.zst" ] && ok "Löschen: Sicherung ohne .sha256 gelöscht" || bad "Löschen ohne .sha256 ($code)"
vpost "$D/defekt.img.zst" delete
[ "$code" = 400 ] && ok "Löschen: schon gelöscht - abgelehnt" || bad "doppelt löschen ($code)"
[ -s "$IMG" ] && [ -s "$IMG.sha256" ] && ok "andere Sicherungen unberührt" || bad "andere Sicherung weg"

# ---------------------------------------------------------------- Hochladen
head -c $((1024 * MIB)) /dev/urandom >"$W/b.img"
UP="$U/api/restore/upload?device=da1&size=$SB&ident=b.img&replug=1"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "Content-Type: application/octet-stream" --data-binary "@$W/dl.img.zst" "$UP")
wait_job
[ "$code" = 200 ] && [ "$(q 'd["kind"] + str(d["ok"])' <"$W/st")" = restoreTrue ] && ok "Hochladen und wiederherstellen" || bad "Hochladen ($code)" "$(cat "$W/body" "$W/st")"
same_files "$W/b.img" && ok "hochgeladen: Dateien gleich" || bad "hochgeladen: Dateien verschieden"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "Content-Type: application/octet-stream" --data-binary "@$W/data/usbfmt-backup/kaputt.img.zst" "$UP")
[ "$code" = 400 ] && ok "Hochladen: keine Image-Datei abgelehnt" || bad "Hochladen kaputt ($code)" "$(cat "$W/body")"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "Content-Type: application/json" --data-binary "@$W/dl.img.zst" "$UP")
[ "$code" = 415 ] && ok "Hochladen: falscher Content-Type abgelehnt" || bad "Content-Type ($code)"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "Origin: http://boese.example" -H "Content-Type: application/octet-stream" --data-binary "@$W/dl.img.zst" "$UP")
[ "$code" = 403 ] && ok "Hochladen: fremder Ursprung abgelehnt" || bad "Ursprung ($code)"
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "Content-Type: application/octet-stream" --data-binary "@$W/dl.img.zst" \
    "$U/api/restore/upload?device=da2&size=$SC&ident=c.img")
[ "$code" = 400 ] && ok "Hochladen: zu kleines Ziel abgelehnt" || bad "Hochladen zu klein ($code)" "$(cat "$W/body")"

# ---------------------------------------------------------------- Abbrechen
start "$W/a.img:$W/b.img:$W/c.img" "1,1,0" USBFMT_SLOW=1
rm -f "$W/data/usbfmt-backup"/*.img.zst*
post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"$W/data/usbfmt-backup\"}" backup >/dev/null
for _ in $(seq 1 50); do curl -s "$U/api/status" | grep -q '"cancellable":true' && break; sleep 0.1; done
head -c 1000 /dev/urandom >"$W/data/usbfmt-backup/kaputt_rest.img.zst"
code=$(post "{\"path\":\"$W/data/usbfmt-backup/kaputt_rest.img.zst\"}" images/delete)
[ "$code" = 409 ] && [ -e "$W/data/usbfmt-backup/kaputt_rest.img.zst" ] && ok "Löschen während eines Auftrags abgelehnt" \
    || bad "Löschen während Auftrag ($code)" "$(cat "$W/body")"
curl -s -H "$J" -d '{}' "$U/api/cancel" >/dev/null
wait_job
[ "$(q 'str(d["ok"]) + str(d["cancelled"])' <"$W/st")" = FalseTrue ] && ok "Sichern abgebrochen" || bad "Abbrechen" "$(cat "$W/st")"
[ -z "$(ls "$W/data/usbfmt-backup" 2>/dev/null | grep -v kaputt | grep -v link)" ] && ok "abgebrochen: keine Dateien übrig" \
    || bad "Reste: $(ls "$W/data/usbfmt-backup")"

# ---------------------------------------------------------------- Sperre bei laufendem Spiel
# ShadowMountPlus spiegelt eingebundene Laufwerke in die Sandbox laufender Titel: dann nie abmelden
mkdir -p "$W/sandbox/PPSA01325_000"
start "$W/a.img:$W/b.img:$W/c.img" "1,1,0" USBFMT_SANDBOX="$W/sandbox"
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"$W/data/usbfmt-backup\"}" backup)
[ "$code" = 409 ] && grep -q "PPSA01325" "$W/body" && ok "Spiel läuft: Sichern eines eingebundenen Laufwerks abgelehnt" \
    || bad "Sperre Sichern ($code)" "$(cat "$W/body")"
for req in "label:{\"label\":\"X\"}" "check:{\"repair\":false}" "format:{\"fs\":\"exfat\",\"label\":\"X\"}"; do
    api=${req%%:*} extra=${req#*:}
    code=$(post "$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); d.update(device='da0', size=$SA, ident='a.img', replug=True); print(json.dumps(d))" "$extra")" "$api")
    [ "$code" = 409 ] && grep -q "Spiel" "$W/body" && ok "Spiel läuft: $api abgelehnt" || bad "Sperre $api ($code)" "$(cat "$W/body")"
done
code=$(curl -s -o "$W/body" -w '%{http_code}' -H "Content-Type: application/octet-stream" --data-binary "@$W/dl.img.zst" \
    "$U/api/restore/upload?device=da1&size=$SB&ident=b.img&replug=1")
[ "$code" = 409 ] && ok "Spiel läuft: Hochladen auf eingebundenes Laufwerk abgelehnt" || bad "Sperre Hochladen ($code)"
code=$(post "{\"device\":\"da2\",\"size\":$SC,\"ident\":\"c.img\",\"dest\":\"$W/data/usbfmt-backup\"}" backup)
[ "$code" = 202 ] && wait_job && [ "$(q 'd["ok"]' <"$W/st")" = True ] && ok "nicht eingebundenes Laufwerk: trotz Spiel erlaubt" \
    || bad "nicht eingebunden ($code)" "$(cat "$W/body" "$W/st")"
rm -rf "$W/sandbox/PPSA01325_000"
mkdir -p "$W/sandbox/PPSA86920_000"   # die Kachel des Formatierers selbst zählt nicht
code=$(post "{\"device\":\"da0\",\"size\":$SA,\"ident\":\"a.img\",\"replug\":true,\"dest\":\"download\"}" backup)
[ "$code" = 200 ] && ok "eigene Kachel läuft: kein Hindernis" || bad "eigene Kachel ($code)" "$(cat "$W/body")"
TOK=$(q 'd["token"]' <"$W/body")
mkdir -p "$W/sandbox/CUSA00419_000"   # Spiel startet zwischen Anfrage und Abmelden
code=$(curl -s -o "$W/body" -w '%{http_code}' "$U/api/backup/download?token=$TOK")
wait_job
[ "$code" = 409 ] && [ "$(q 'd["ok"]' <"$W/st")" = False ] && grep -q "CUSA00419" "$W/st" \
    && ok "Spiel nach der Anfrage gestartet: vor dem Abmelden erkannt, nichts abgemeldet" || bad "Prüfung vor dem Abmelden ($code)" "$(cat "$W/st")"
grep -q "Stick jetzt abziehen" "$W/st" && bad "trotzdem abgemeldet" || true

echo
[ $fails -eq 0 ] && echo "Alle Image-Sicherungs-Tests bestanden." || echo "$fails Test(s) fehlgeschlagen."
exit $((fails > 0))
