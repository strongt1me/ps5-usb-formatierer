# Baut usbfmt_win.exe (Windows-Helfer für den PC-Modus) aus demselben C-Code wie der PS5-Payload.
# Benötigt: python -m pip install ziglang
$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "..\usb_format\src"
$files = "win\usbfmt_win.c", "win\wincompat.c", "mkfs.c", "upcase.c", "devtest.c", "vollabel.c", "fsdetect.c", "fsread.c" |
    ForEach-Object { Join-Path $src $_ }
$out = Join-Path $PSScriptRoot "usbfmt_win.exe"
python -m ziglang cc -target x86_64-windows-gnu -O2 -municode -std=gnu11 -Wall -Wno-unused-function `
    -D_FILE_OFFSET_BITS=64 -include (Join-Path $src "win\wincompat.h") `
    @files -lbcrypt -o $out
if ($LASTEXITCODE -ne 0) { throw "Bau von usbfmt_win.exe fehlgeschlagen" }
Remove-Item (Join-Path $PSScriptRoot "usbfmt_win.pdb") -ErrorAction SilentlyContinue
Write-Host "Fertig: $out"
