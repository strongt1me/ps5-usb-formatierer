// Baut die App "USB-Formatierer" (PPSA86920, Medien) als Dump-Ordner für den Homebrew-Ordner der PS5
// (z. B. /data/homebrew/PPSA86920 – ShadowMountPlus meldet ihn an; es nimmt nur PPSA-/CUSA-Kennungen):
//   PPSA86920/eboot.bin           Starter als nativer Titel, in WSL umgewandelt und signiert (app/build_title.sh)
//   PPSA86920/sce_sys/param.json  Titel, Kategorie Medien, Version (aus der Datei VERSION)
//   PPSA86920/sce_sys/icon0.png   Kachelsymbol, dazu pic0/pic1 (Kachel-Hintergrund)
//   PPSA86920/sce_module/libc.prx vom App-Lader verlangt (app/sce_module/NOTICE.txt)

using System.Text.RegularExpressions;
using LibProsperoPkg.Content;

string repo = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", ".."));
string app = Path.Combine(repo, "app");
string target = Path.Combine(repo, "dump", "out", "PPSA86920");

string raw = Path.Combine(app, "eboot.bin");
if (!File.Exists(raw))
{
    Console.Error.WriteLine($"app/eboot.bin fehlt – zuerst in WSL 'make all app' bauen ({raw}).");
    return 1;
}

if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
Directory.CreateDirectory(Path.Combine(target, "sce_sys"));

// Bereits von ps5-native-tool signiert (Kennung 4F 15 3D 1D). Ein erneutes MakeFself würde die Signatur ersetzen:
// Dessen Ergebnis (statische ELF, leere Prozess-Parameter) startete auf FW 12 nicht (CE-107750-0).
byte[] self = File.ReadAllBytes(raw);
if (self.Length < 4 || self[0] != 0x4F || self[1] != 0x15 || self[2] != 0x3D || self[3] != 0x1D)
{
    Console.Error.WriteLine("app/eboot.bin ist nicht signiert – in WSL mit 'make app' (app/build_title.sh) bauen.");
    return 1;
}
File.Copy(raw, Path.Combine(target, "eboot.bin"));
Console.WriteLine($"eboot.bin übernommen ({self.Length} Bytes, nativer Titel).");

foreach (var file in Directory.GetFiles(Path.Combine(app, "sce_sys")))
    File.Copy(file, Path.Combine(target, "sce_sys", Path.GetFileName(file)));
// Version der App (Datei VERSION im Projekt, wie Formatierer und PC-Programme) als contentVersion, z. B. 2.5.3 ->
// 02.005.003 - die PS5 zeigt sie unter "Informationen" der Kachel
string version = File.ReadAllText(Path.Combine(repo, "..", "VERSION")).Trim();
var v = Regex.Match(version, @"^(\d+)\.(\d+)\.(\d+)$");
if (!v.Success)
{
    Console.Error.WriteLine($"VERSION ungültig: {version}");
    return 1;
}
string contentVersion = $"{int.Parse(v.Groups[1].Value):00}.{int.Parse(v.Groups[2].Value):000}.{int.Parse(v.Groups[3].Value):000}";
string paramJson = Path.Combine(target, "sce_sys", "param.json");
string param = File.ReadAllText(paramJson);
string patched = Regex.Replace(param, "\"contentVersion\": \"[0-9.]*\"", $"\"contentVersion\": \"{contentVersion}\"");
if (patched == param && !param.Contains($"\"contentVersion\": \"{contentVersion}\""))
{
    Console.Error.WriteLine("param.json: contentVersion nicht gefunden.");
    return 1;
}
File.WriteAllText(paramJson, patched);
Console.WriteLine($"Version {version} (contentVersion {contentVersion})");
Console.WriteLine("sce_sys übernommen: " + string.Join(", ", Directory.GetFiles(Path.Combine(target, "sce_sys")).Select(Path.GetFileName)));

// Der App-Lader verlangt sce_module/libc.prx in jedem Titel (sonst PRX_SCE_MODULE_LOAD_ERROR), siehe app/sce_module/NOTICE.txt
string libc = Path.Combine(app, "sce_module", "libc.prx");
if (!File.Exists(libc))
{
    Console.Error.WriteLine("app/sce_module/libc.prx fehlt.");
    return 1;
}
Directory.CreateDirectory(Path.Combine(target, "sce_module"));
File.Copy(libc, Path.Combine(target, "sce_module", "libc.prx"));
Console.WriteLine("sce_module übernommen: libc.prx");

// Nur ein Hinweis: Die Prüfung von LibProsperoPkg hielt die alte, nicht startfähige eboot.bin für startbar und
// meldet die libc.prx (signiert, unverschlüsselt; startet so in anderen Titeln auf FW 12) als verschlüsselt.
var ready = ProsperoLaunchReadiness.InspectAppRoot(target);
Console.WriteLine($"Hinweis LibProsperoPkg-Prüfung: {(ready.IsLaunchReady ? "startbar" : "Einwände")}");
foreach (var issue in ready.Issues) Console.WriteLine($"  {issue}");
Console.WriteLine($"Dump-Ordner: {target}");
return 0;
