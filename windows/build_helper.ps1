# Baut usbfmt_win.exe (Windows-Helfer für die Arbeit am PC) aus demselben C-Code wie der PS5-Payload, dazu
# mke2fs.exe (ext2/3/4, siehe mke2fs\build_mke2fs.ps1), deren Prüfsumme der Helfer vor jedem Start kontrolliert.
# Benötigt: python -m pip install ziglang
#   build_helper.ps1         ausgelieferte Fassung + helper_hash.py (Prüfsumme, die das Programm vor dem Start prüft)
#   build_helper.ps1 -Test   zusätzlich usbfmt_win_test.exe mit --image (Abbild-Datei statt Datenträger, nur für Tests)
param([switch]$Test)
$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "..\usb_format\src"
$files = "win\usbfmt_win.c", "win\wincompat.c", "mkfs.c", "hfsplus.c", "upcase.c", "devtest.c", "vollabel.c",
         "fsdetect.c", "fsread.c", "extfs.c" | ForEach-Object { Join-Path $src $_ }

# mke2fs.exe (einmalig gebaut, danach wiederverwendet) und ihre Prüfsumme für den Helfer
$mke2fs = Join-Path $PSScriptRoot "mke2fs.exe"
if (-not (Test-Path $mke2fs)) {
    & (Join-Path $PSScriptRoot "mke2fs\build_mke2fs.ps1")
    if (-not (Test-Path $mke2fs)) { throw "mke2fs.exe fehlt" }
}
$mhash = (Get-FileHash -Algorithm SHA256 $mke2fs).Hash.ToLower()
Set-Content -Encoding ascii (Join-Path $src "win\mke2fs_hash.h") @"
/* Erzeugt von windows/build_helper.ps1 - SHA-256 der mitgelieferten mke2fs.exe */
#define MKE2FS_SHA256 "$mhash"
"@

function Build($out, [string[]]$extra) {
    python -m ziglang cc -target x86_64-windows-gnu -O2 -municode -std=gnu11 -Wall -Wno-unused-function `
        -D_FILE_OFFSET_BITS=64 -include (Join-Path $src "win\wincompat.h") @extra `
        @files -o $out
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
