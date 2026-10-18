# Windows-Helfer an Abbild-Dateien (usbfmt_win_test.exe --image, nie ein echter Datenträger): exFAT, FAT32, ext4 (über
# mke2fs.exe), Image-Sicherung hin und zurück. Für die CI und zum Nachprüfen am PC.
#   pwsh -File windows\tests\win_helper_test.ps1      (vorher build_helper.ps1 -Test)
$ErrorActionPreference = "Stop"
$here = Split-Path $PSScriptRoot
$exe = Join-Path $here "usbfmt_win_test.exe"
if (-not (Test-Path $exe)) { throw "usbfmt_win_test.exe fehlt - zuerst build_helper.ps1 -Test" }
$w = Join-Path ([IO.Path]::GetTempPath()) ("usbfmt-wintest-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $w | Out-Null
$size = 512MB
$script:fails = 0

function Check($name, $ok, $extra = "") {
    if ($ok) { Write-Host "OK      $name" } else { Write-Host "FEHLER  $name  [$extra]"; $script:fails++ }
}
function NewImage($path) {
    $f = [IO.File]::Create($path)
    $f.SetLength($size)
    $f.Close()
}
# Helfer starten; Rückgabe: letzte Zeile "R<TAB>1|0<TAB>Meldung<TAB>JSON"
function Helper($image, [string[]]$cmd) {
    $out = & $exe --lang de --image $image @cmd 2>&1
    return [string]($out | Select-Object -Last 1)
}

try {
    foreach ($fs in "exfat", "fat32", "ext4") {
        $img = Join-Path $w "$fs.img"
        NewImage $img
        $r = Helper $img @("format", "9", "$size", $fs, "TEST")
        Check "$fs formatiert" ($r -like "R`t1`t*") $r
    }

    # Image-Sicherung: exFAT mit Daten sichern, in ein leeres Abbild zurückschreiben - danach Byte für Byte gleich
    $src = Join-Path $w "exfat.img"
    $zst = Join-Path $w "sicherung.img.zst"
    $r = Helper $src @("backup", "9", "$size", $zst)
    Check "Sichern" ($r -like "R`t1`t*" -and (Test-Path $zst)) $r
    $dst = Join-Path $w "ziel.img"
    NewImage $dst
    $r = Helper $dst @("restore", "9", "$size", $zst)
    Check "Wiederherstellen und geprüft" ($r -like "R`t1`t*") $r
    $a = (Get-FileHash $src -Algorithm SHA256).Hash
    $b = (Get-FileHash $dst -Algorithm SHA256).Hash
    Check "Abbild nach dem Wiederherstellen gleich" ($a -eq $b) "$a / $b"

    # falsche Größe: abgelehnt, nichts geschrieben
    $r = Helper $dst @("restore", "9", "$($size - 1MB)", $zst)
    Check "falsche Größe abgelehnt" ($r -like "R`t0`t*") $r
} finally {
    Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
}
if ($script:fails) { Write-Host "$($script:fails) fehlgeschlagen"; exit 1 }
Write-Host "Windows-Helfer: alle Prüfungen bestanden"
