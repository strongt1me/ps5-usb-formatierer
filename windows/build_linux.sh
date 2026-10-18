#!/usr/bin/env bash
# Baut beide Programme für Linux (x86_64) als einzelne ausführbare Dateien (PyInstaller) und packt sie als .tar.gz:
#   PS5-USB-Formatierer   PS5 im Netz, USB am PC für die PS5 und "Am PC Standard" (braucht usb_format.elf)
#   USB-Formatierer       nur "Am PC Standard": frei formatieren, ohne PS5-Funktionen
# Benötigt: python3 mit tkinter, pip install pyinstaller pillow; gcc (statische glibc empfohlen).
#   windows/build_linux.sh [pfad/zu/usb_format.elf]
set -euo pipefail
cd "$(dirname "$0")"
ELF=${1:-../usb_format/usb_format.elf}
[ -f "$ELF" ] || { echo "usb_format.elf fehlt ($ELF) - für die PS5-Fassung nötig"; exit 1; }
PY=${PYTHON:-python3}

# Übersetzungen vollständig?
"$PY" i18n_check.py
"$PY" ../usb_format/tools/i18n_gen.py

# Helfer (statisch) und seine Prüfsumme - das Programm startet nur diese Fassung mit Root-Rechten
bash ../usb_format/src/linux/build.sh "$PWD/usbfmt_linux"
HASH=$(sha256sum usbfmt_linux | cut -d' ' -f1)
printf '"""Erzeugt von build_linux.sh - SHA-256 des ausgelieferten usbfmt_linux"""\nHELPER_SHA256 = "%s"\n' "$HASH" >helper_hash.py
echo "Helfer: $HASH"

# Programmsymbol als PNG (Linux-Fenster nehmen kein .ico)
"$PY" - <<'EOF'
from PIL import Image
Image.open("../usb_format/app/art/icon.png").convert("RGBA").resize((128, 128), Image.LANCZOS).save("app.png")
EOF

# --noupx: UPX würde den Helfer verändern - seine Prüfsumme passte dann nicht mehr
COMMON=(--noconfirm --clean --noupx --onefile --add-binary "usbfmt_linux:." --add-data "app.png:." --add-data "i18n_app.json:." --add-data "../VERSION:." --add-data "avatar-22.png:." --add-data "avatar-44.png:."
        --add-data "../usb_format/web/help.html:." --add-data "../usb_format/third_party/zstd/LICENSE:lizenzen/zstd")
# unter dem Namen, den das Programm sucht (usb_format.elf) - auch wenn die übergebene Datei anders heißt
ELFDIR=$(mktemp -d)
cp "$ELF" "$ELFDIR/usb_format.elf"
"$PY" -m PyInstaller "${COMMON[@]}" --name "PS5-USB-Formatierer" --add-data "$ELFDIR/usb_format.elf:." usb_formatierer.py
rm -rf "$ELFDIR"
"$PY" -m PyInstaller "${COMMON[@]}" --name "USB-Formatierer" usb_standard.py

# je Programm ein Archiv (behält das Ausführrecht), dazu die Lizenzhinweise
mkdir -p dist/pack
for app in PS5-USB-Formatierer USB-Formatierer; do
    d="dist/pack/$app"
    rm -rf "$d"
    mkdir -p "$d"
    cp "dist/$app" "$d/"
    cp ../LICENSE "$d/LICENSE.txt"
    cat >"$d/LIESMICH.txt" <<EOF
$app für Linux (x86_64)

Starten: ./$app  (oder doppelt anklicken)
Zum Formatieren fragt das System nach dem Administrator-Passwort (pkexec, Paket polkit).
NTFS, FAT16 und ext2/3/4 schreiben die Werkzeuge des Systems: Pakete ntfs-3g, dosfstools und e2fsprogs.
Anleitung: Knopf "Hilfe" im Programm.
EOF
    tar -C dist/pack -czf "dist/$app-linux-x86_64.tar.gz" "$app"
done
ls -l dist/*.tar.gz
