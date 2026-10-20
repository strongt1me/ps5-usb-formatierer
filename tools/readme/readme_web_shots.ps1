# README-Bilder der Weboberfläche und der Anleitung je Sprache (Edge ohne Fenster, über readme_proxy.py auf 8095)
param([string[]]$Langs = @("de", "en", "it", "es", "fr"), [string]$Out = (Join-Path ([IO.Path]::GetTempPath()) "usbfmt_readme_raw"),
      [switch]$BackupOnly)
$s = $Out   # Rohbilder und Edge-Profile
$raw = $Out
New-Item -ItemType Directory -Force $raw | Out-Null
$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$ua = 'Mozilla/5.0 (PlayStation; PlayStation 5/12.00) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15'
function Set-Server($body) {
    Invoke-RestMethod -Method Post -ContentType 'application/json' -Body ($body | ConvertTo-Json) `
        http://127.0.0.1:8090/api/settings | Out-Null
}
function Shot($size, $file, $url, $extra, $profile) {
    $prof = Join-Path $s "edge_readme_$profile"
    Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue   # frischer Zustand (localStorage)
    $a = "--headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `"--user-data-dir=$prof`" " +
         "--window-size=$size --virtual-time-budget=10000 $extra `"--screenshot=$raw\$file`" `"$url`""
    Start-Process -FilePath $edge -ArgumentList $a -Wait
}
$P = "http://127.0.0.1:8095"
foreach ($l in $Langs) {
    Set-Server @{ lang = $l; theme = "dark"; scale_pc = 100; scale_tv = 130 }
    Shot "1280,1250" "backup_$l.png" "$P/?pick=2&img=1" "" "pc"
    if ($BackupOnly) { "$l fertig"; continue }
    Shot "1280,1260" "web_$l.png" "$P/?pick=0" "" "pc"
    Shot "1280,1000" "dialog_$l.png" "$P/?pick=0&start=1" "" "pc"
    Shot "1540,900" "ps5_$l.png" "$P/?pick=0" "`"--user-agent=$ua`"" "tv"
    Shot "1100,1050" "help_$l.png" "$P/help?lang=$l" "" "help"
    Set-Server @{ theme = "light" }
    Shot "1280,1000" "light_$l.png" "$P/?pick=1" "" "pc"
    "$l fertig"
}
Set-Server @{ lang = ""; theme = "auto"; scale_pc = 100; scale_tv = 160 }
