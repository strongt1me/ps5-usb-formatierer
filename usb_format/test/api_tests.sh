#!/usr/bin/env bash
# Prueft die laufende Testinstanz (test/run_server.sh): Images per fsck und die Schutzregeln der API.
set -u
T=${FSTOOLS:-$HOME/fstools/root}
export PATH="$T/usr/sbin:$T/usr/bin:$PATH"
B=${BUILD:-$HOME/usbfmt_test}
U=http://127.0.0.1:8090
fails=0

expect() {  # expect <name> <erwarteter HTTP-Code> curl-Argumente...
    local name=$1 want=$2; shift 2
    local got
    got=$(curl -s -o /tmp/api_body -w '%{http_code}' "$@")
    if [ "$got" = "$want" ]; then echo "OK      $name ($got): $(head -c 120 /tmp/api_body)"
    else echo "FEHLER  $name: erwartet $want, bekommen $got: $(cat /tmp/api_body)"; fails=$((fails + 1)); fi
}

echo "== Images der UI-Durchlaeufe pruefen =="
for img in stick0 stick1; do
    type=$(sfdisk -d "$B/$img.img" 2>/dev/null | grep -o 'type=[0-9a-f]*' | head -1)
    echo "        $img: MBR $type"
    # Partition per Loop-Offset geht ohne root nicht -> mit dd herauskopieren (sparse)
    dd if="$B/$img.img" of=/tmp/$img.part bs=1M skip=1 conv=sparse status=none
    if [ "$type" = "type=7" ]; then
        if fsck.exfat -n /tmp/$img.part >/tmp/fsck.log 2>&1; then echo "OK      $img fsck.exfat, Label: $(exfatlabel /tmp/$img.part 2>/dev/null | tail -1)"
        else echo "FEHLER  $img fsck.exfat"; cat /tmp/fsck.log; fails=$((fails + 1)); fi
    else
        if fsck.fat -n /tmp/$img.part >/tmp/fsck.log 2>&1; then echo "OK      $img fsck.fat, Label: $(fatlabel /tmp/$img.part 2>/dev/null)"
        else echo "FEHLER  $img fsck.fat"; cat /tmp/fsck.log; fails=$((fails + 1)); fi
    fi
    rm -f /tmp/$img.part
done

echo "== API-Schutz =="
size0=$(stat -c %s "$B/stick0.img")
J='Content-Type: application/json'
expect "Geraeteliste"                 200 $U/api/devices
expect "falsche Groesse (anderer Stick)" 409 -H "$J" -d "{\"device\":\"da0\",\"fs\":\"exfat\",\"label\":\"\",\"size\":123}" $U/api/format
expect "unbekanntes Geraet"           404 -H "$J" -d "{\"device\":\"da9\",\"fs\":\"exfat\",\"size\":$size0}" $U/api/format
expect "falsches Dateisystem"         400 -H "$J" -d "{\"device\":\"da0\",\"fs\":\"btrfs\",\"size\":$size0}" $U/api/format
expect "ungueltiges Label"            400 -H "$J" -d "{\"device\":\"da0\",\"fs\":\"exfat\",\"label\":\"a/b\",\"size\":$size0}" $U/api/format
expect "kaputtes JSON"                400 -H "$J" -d "{\"device\":" $U/api/format
expect "ohne JSON-Content-Type (CSRF per Formular)" 415 -H 'Content-Type: text/plain' -d "{\"device\":\"da0\",\"fs\":\"exfat\",\"size\":$size0}" $U/api/format
expect "fremde Origin (CSRF)"         403 -H "$J" -H 'Origin: http://boese.example' -d "{\"device\":\"da0\",\"fs\":\"exfat\",\"size\":$size0}" $U/api/format
expect "DNS-Rebinding-Host"           403 -H 'Host: boese.example:8090' $U/api/devices
expect "GET auf format"               405 $U/api/format
expect "unbekannter Pfad"             404 $U/geheim
expect "zu grosser Body"              400 -H "$J" -d "$(head -c 6000 /dev/zero | tr '\0' a)" $U/api/format
expect "gleiche Origin erlaubt (Status)" 200 -H 'Origin: http://127.0.0.1:8090' $U/api/status

echo
if [ $fails -eq 0 ]; then echo "Alle API-Tests bestanden."; else echo "$fails Test(s) fehlgeschlagen."; fi
exit $((fails > 0))
