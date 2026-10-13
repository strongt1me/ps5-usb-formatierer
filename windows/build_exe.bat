@echo off
rem Baut PS5-USB-Formatierer.exe (benoetigt: pip install pyinstaller pillow ziglang)
rem usb_format.elf muss vorher gebaut sein und wird in die EXE eingebettet.
setlocal
cd /d "%~dp0"
if not exist "..\usb_format\usb_format.elf" (
    echo usb_format.elf fehlt - zuerst den PS5-Payload bauen.
    exit /b 1
)
if not exist app.ico python make_icon.py || exit /b 1
rem Windows-Helfer fuer den PC-Modus (benoetigt: pip install ziglang)
powershell -NoProfile -ExecutionPolicy Bypass -File build_helper.ps1 || exit /b 1
python -m PyInstaller --noconfirm --clean --windowed --onefile ^
    --name "PS5-USB-Formatierer" --icon app.ico ^
    --add-data "app.ico;." --add-data "..\usb_format\usb_format.elf;." --add-binary "usbfmt_win.exe;." ^
    usb_formatierer.py || exit /b 1
echo.
echo Fertig: "%~dp0dist\PS5-USB-Formatierer.exe"
