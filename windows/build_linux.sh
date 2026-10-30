#!/usr/bin/env bash
# Baut beide Programme für Linux (Architektur dieses Rechners: x86_64 oder aarch64) als einzelne ausführbare Dateien
# (PyInstaller) und packt sie als .tar.gz und - wenn APPIMAGETOOL gesetzt ist - als AppImage:
#   PS5-USB-Formatierer   PS5 im Netz, "Am PC – für PS5" und "Am PC – frei" (braucht usb_format.elf)
#   USB-Formatierer       nur "Am PC – frei": frei formatieren, ohne PS5-Funktionen
# Benötigt: python3 mit tkinter, python3 -m pip install -r requirements-build.txt; gcc (statische glibc empfohlen).
#   [APPIMAGETOOL=pfad/appimagetool] [APPIMAGE_RUNTIME=pfad/runtime] windows/build_linux.sh [pfad/zu/usb_format.elf]
# Die Update-Information der AppImages zeigt auf GITHUB_REPOSITORY (in der CI gesetzt, sonst das Projekt selbst).
set -euo pipefail
cd "$(dirname "$0")"
ELF=${1:-../usb_format/usb_format.elf}
[ -f "$ELF" ] || { echo "usb_format.elf fehlt ($ELF) - für die PS5-Fassung nötig"; exit 1; }
PY=${PYTHON:-python3}
ARCH=$(uname -m)
case "$ARCH" in amd64) ARCH=x86_64 ;; arm64) ARCH=aarch64 ;; esac
REPO=${GITHUB_REPOSITORY:-strongt1me/ps5-usb-formatierer}
REPO_OWNER=${REPO%%/*}
REPO_NAME=${REPO#*/}

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
        --add-data "../usb_format/web/help.html:." --add-data "../usb_format/third_party/zstd/LICENSE:lizenzen/zstd"
        --add-data "../LICENSE:lizenzen")
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
$app für Linux ($ARCH)

Starten: ./$app  (oder doppelt anklicken)
Zum Formatieren fragt das System nach dem Administrator-Passwort (pkexec, Paket polkit).
NTFS, FAT16 und ext2/3/4 schreiben die Werkzeuge des Systems: Pakete ntfs-3g, dosfstools und e2fsprogs.
Anleitung: Knopf "Hilfe" im Programm.
EOF
    tar -C dist/pack -czf "dist/$app-linux-$ARCH.tar.gz" "$app"
done
ls -l dist/*.tar.gz

# AppImage: eine Datei, ausführbar machen und starten. Drin steckt dasselbe Einzeldatei-Programm wie im Archiv - mit
# Absicht: es entpackt sich (samt Helfer) nach /tmp. Läge der Helfer direkt im AppImage, also in dessen FUSE-Einhängung,
# könnte root (pkexec) ihn nicht lesen - FUSE lässt ohne allow_other nur den Benutzer selbst hinein.
if [ -z "${APPIMAGETOOL:-}" ]; then
    echo "AppImage übersprungen (APPIMAGETOOL nicht gesetzt)"
    exit 0
fi
"$PY" - <<'EOF'
from PIL import Image
Image.open("../usb_format/app/art/icon.png").convert("RGBA").resize((256, 256), Image.LANCZOS).save("dist/pack/icon256.png")
EOF
for app in PS5-USB-Formatierer USB-Formatierer; do
    a="dist/pack/$app.AppDir"
    rm -rf "$a"
    mkdir -p "$a/usr/bin"
    cp "dist/$app" "$a/usr/bin/$app"
    cp dist/pack/icon256.png "$a/$app.png"
    ln -s "$app.png" "$a/.DirIcon"
    cat >"$a/AppRun" <<EOF
#!/bin/sh
exec "\$APPDIR/usr/bin/$app" "\$@"
EOF
    chmod +x "$a/AppRun"
    cat >"$a/$app.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$app
Comment=USB-Laufwerke formatieren, prüfen und sichern
Exec=$app
Icon=$app
Terminal=false
Categories=Utility;System;
EOF
    # Update-Information für AppImageUpdate: holt die .zsync-Datei vom neuesten Release; appimagetool legt sie neben
    # das AppImage (zsyncmake steckt in appimagetool)
    ARCH=$ARCH APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" --no-appstream \
        -u "gh-releases-zsync|${REPO_OWNER}|${REPO_NAME}|latest|$app-$ARCH.AppImage.zsync" \
        ${APPIMAGE_RUNTIME:+--runtime-file "$APPIMAGE_RUNTIME"} "$a" "dist/$app-$ARCH.AppImage"
    [ -s "dist/$app-$ARCH.AppImage.zsync" ] || { echo "dist/$app-$ARCH.AppImage.zsync fehlt"; exit 1; }
done
ls -l dist/*.AppImage dist/*.zsync
