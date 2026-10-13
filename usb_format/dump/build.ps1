# Baut den Dump-Ordner dump/out/PPSA86920 (für /data/homebrew/ auf der PS5).
# Voraussetzung: app/eboot.bin ist gebaut (WSL: make all app) und das .NET-10-SDK ist installiert.
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & dotnet run -c Release
    if ($LASTEXITCODE -ne 0) { throw 'Dump-Ordner konnte nicht gebaut werden.' }
} finally {
    Pop-Location
}
