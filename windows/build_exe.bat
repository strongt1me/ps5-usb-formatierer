@echo off
rem Baut beide Windows-Programme (benoetigt: pip install pyinstaller pillow ziglang):
rem   PS5-USB-Formatierer.exe  PS5 im Netz, USB am PC fuer die PS5 und "Am PC Standard"
rem   USB-Formatierer.exe      nur "Am PC Standard": frei formatieren, ohne PS5-Funktionen
rem usb_format.elf muss vorher gebaut sein und wird in die PS5-Fassung eingebettet.
rem In beiden: Helfer usbfmt_win.exe, mke2fs.exe (ext2/3/4, GPL) samt Lizenzhinweis, Uebersetzungen und die
rem Anleitung (usb_format\web\help.html, dieselbe wie in der Weboberflaeche).
setlocal
cd /d "%~dp0"
if not exist "..\usb_format\usb_format.elf" (
    echo usb_format.elf fehlt - zuerst den PS5-Payload bauen.
    exit /b 1
)
if not exist app.ico python make_icon.py || exit /b 1
rem Uebersetzungen vollstaendig?
python i18n_check.py || exit /b 1
rem Windows-Helfer fuer die Arbeit am PC (benoetigt: pip install ziglang); baut bei Bedarf auch mke2fs.exe
powershell -NoProfile -ExecutionPolicy Bypass -File build_helper.ps1 || exit /b 1
set COMMON=--add-data "app.ico;." --add-binary "usbfmt_win.exe;." --add-binary "mke2fs.exe;." ^
    --add-data "mke2fs\NOTICE.txt;lizenzen\mke2fs" --add-data "..\usb_format\third_party\zstd\LICENSE;lizenzen\zstd" ^
    --add-data "i18n_app.json;." --add-data "..\VERSION;." ^
    --add-data "..\usb_format\web\help.html;." --add-data "avatar-22.png;." --add-data "avatar-44.png;."
rem --noupx: UPX wuerde Helfer und mke2fs.exe veraendern - ihre Pruefsummen passten dann nicht mehr
python -m PyInstaller --noconfirm --clean --noupx --windowed --onefile ^
    --name "PS5-USB-Formatierer" --icon app.ico %COMMON% ^
    --add-data "..\usb_format\usb_format.elf;." ^
    usb_formatierer.py || exit /b 1
python -m PyInstaller --noconfirm --clean --noupx --windowed --onefile ^
    --name "USB-Formatierer" --icon app.ico %COMMON% ^
    usb_standard.py || exit /b 1
echo.
echo Fertig: "%~dp0dist\PS5-USB-Formatierer.exe"
echo         "%~dp0dist\USB-Formatierer.exe"
