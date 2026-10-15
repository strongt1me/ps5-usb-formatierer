@echo off
rem Baut beide Windows-Programme (benoetigt: pip install pyinstaller pillow ziglang):
rem   PS5-USB-Formatierer.exe  PS5 im Netz, USB am PC fuer die PS5 und "Am PC Standard"
rem   USB-Formatierer.exe      nur "Am PC Standard": frei formatieren, ohne PS5-Funktionen
rem usb_format.elf muss vorher gebaut sein und wird in die PS5-Fassung eingebettet.
setlocal
cd /d "%~dp0"
if not exist "..\usb_format\usb_format.elf" (
    echo usb_format.elf fehlt - zuerst den PS5-Payload bauen.
    exit /b 1
)
if not exist app.ico python make_icon.py || exit /b 1
rem Windows-Helfer fuer die Arbeit am PC (benoetigt: pip install ziglang)
powershell -NoProfile -ExecutionPolicy Bypass -File build_helper.ps1 || exit /b 1
python -m PyInstaller --noconfirm --clean --windowed --onefile ^
    --name "PS5-USB-Formatierer" --icon app.ico ^
    --add-data "app.ico;." --add-data "..\usb_format\usb_format.elf;." --add-binary "usbfmt_win.exe;." ^
    usb_formatierer.py || exit /b 1
python -m PyInstaller --noconfirm --clean --windowed --onefile ^
    --name "USB-Formatierer" --icon app.ico ^
    --add-data "app.ico;." --add-binary "usbfmt_win.exe;." ^
    usb_standard.py || exit /b 1
echo.
echo Fertig: "%~dp0dist\PS5-USB-Formatierer.exe"
echo         "%~dp0dist\USB-Formatierer.exe"
