# Baut usbfmt_win.exe (Windows-Helfer für den PC-Modus) aus demselben C-Code wie der PS5-Payload.
# Benötigt: python -m pip install ziglang
#   build_helper.ps1         ausgelieferte Fassung + helper_hash.py (Prüfsumme, die das Programm vor dem Start prüft)
#   build_helper.ps1 -Test   zusätzlich usbfmt_win_test.exe mit --image (Abbild-Datei statt Datenträger, nur für Tests)
param([switch]$Test)
$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "..\usb_format\src"
$files = "win\usbfmt_win.c", "win\wincompat.c", "mkfs.c", "upcase.c", "devtest.c", "vollabel.c", "fsdetect.c", "fsread.c" |
    ForEach-Object { Join-Path $src $_ }

function Build($out, [string[]]$extra) {
    python -m ziglang cc -target x86_64-windows-gnu -O2 -municode -std=gnu11 -Wall -Wno-unused-function `
        -D_FILE_OFFSET_BITS=64 -include (Join-Path $src "win\wincompat.h") @extra `
        @files -lbcrypt -o $out
    if ($LASTEXITCODE -ne 0) { throw "Bau von $out fehlgeschlagen" }
    Remove-Item ([IO.Path]::ChangeExtension($out, ".pdb")) -ErrorAction SilentlyContinue
}

$out = Join-Path $PSScriptRoot "usbfmt_win.exe"
Build $out @()
# Das Programm startet den Helfer mit Administratorrechten aus dem Temp-Ordner (PyInstaller) - dort könnte ein
# Schadprogramm ihn austauschen. Deshalb prüft es vor dem Start diese Prüfsumme.
$hash = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLower()
Set-Content -Encoding utf8 (Join-Path $PSScriptRoot "helper_hash.py") @"
"""Erzeugt von build_helper.ps1 - SHA-256 der ausgelieferten usbfmt_win.exe"""
HELPER_SHA256 = "$hash"
"@
Write-Host "Fertig: $out ($hash)"
if ($Test) {
    $t = Join-Path $PSScriptRoot "usbfmt_win_test.exe"
    Build $t @("-DUSBFMT_TEST")
    Write-Host "Fertig: $t"
}
