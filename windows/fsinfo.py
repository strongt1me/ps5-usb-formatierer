"""Wissen über Dateisysteme, Zuordnungseinheiten und Partitionsschemata - ohne Oberfläche.
Gilt für den Windows-Helfer (usbfmt_win) und später ebenso für Linux und macOS."""
import re
import sys

from i18n import _, number, unit

KB, MB, GB, TB = 1024, 1024 ** 2, 1024 ** 3, 1024 ** 4
LABEL_CHARS = re.compile(r"^[A-Za-z0-9 _-]*$")
FAT16_MAX_CLUSTERS = 65524
MAX_PARTS = 16          # wie MAX_PARTS im Helfer; MBR: 4

# Anzeigename -> (Name für den Helfer, längste Bezeichnung, Hinweis)
FILESYSTEMS = {
    "exFAT": ("exfat", 11, "Für USB-Sticks und Speicherkarten: Windows, macOS, Linux, Kameras und Konsolen – "
                           "Dateien beliebig groß."),
    "FAT32": ("fat32", 11, "Läuft fast überall (Autoradio, Fernseher, ältere Geräte) – Dateien aber höchstens 4 GB."),
    "NTFS": ("ntfs", 32, "Für Windows: Dateien beliebig groß, mit Journal und Zugriffsrechten. macOS kann NTFS nur "
                         "lesen, viele Fernseher und Autoradios gar nicht."),
    "FAT (FAT16)": ("fat", 11, "Für sehr alte Geräte: nur für Datenträger bis 4 GB, Dateien höchstens 2 GB."),
    "ext4 (Linux)": ("ext4", 16, "Das Linux-Dateisystem (auch Raspberry Pi, Steam Deck, NAS) – mit Journal, Dateien "
                                 "beliebig groß. Windows und macOS lesen es ohne Zusatzprogramm nicht."),
    "ext3 (Linux)": ("ext3", 16, "Älteres Linux-Dateisystem mit Journal – für ältere Linux-Geräte."),
    "ext2 (Linux)": ("ext2", 16, "Linux ohne Journal – für sehr alte Geräte oder kleine Speicher."),
    "HFS+ (Mac OS Extended)": ("hfsplus", 32, "Für Macs (Mac OS Extended, ohne Journal – macOS kann es im "
                                              "Festplattendienstprogramm zuschalten). Windows liest es nicht, Linux "
                                              "schon."),
}
NO_FS = "Ohne Dateisystem"   # nur Expertenmodus: Partition bleibt leer
FS_EXPERT = list(FILESYSTEMS) + [NO_FS]

SCHEMES = [("auto", "Automatisch – MBR, GPT erst wenn MBR nicht reicht (über 2 TB)"),
           ("mbr", "MBR – auch für ältere Geräte und Autoradios"),
           ("gpt", "GPT – für aktuelle PCs, auch für sehr große Datenträger")]

# Partitionstyp im Expertenmodus: Name für den Helfer -> Anzeige
PTYPES = [("auto", "Automatisch (passend zum Dateisystem)"), ("windows", "Windows-Daten (Basic Data / 0x07)"),
          ("linux", "Linux-Dateisystem (0x83)"), ("swap", "Linux-Auslagerung / Swap (0x82)"),
          ("efi", "EFI-Systempartition (0xEF)"), ("apple", "Apple HFS+ (0xAF)")]


def fs_hint(name):
    return _("Bleibt leer (z. B. für ein späteres Betriebssystem). Windows zeigt sie nicht an.") if name == NO_FS \
        else _(FILESYSTEMS[name][2])


def fs_display(name):
    """Anzeigename (nur "Ohne Dateisystem" wird übersetzt, die übrigen sind Eigennamen)"""
    return _(name) if name == NO_FS else name


def helper_fs(name):
    return "none" if name == NO_FS else FILESYSTEMS[name][0]


def label_max(name):
    return 0 if name == NO_FS else FILESYSTEMS[name][1]


def unit_text(v):
    return f"{v // MB} {unit('MB')}" if v >= MB else f"{v // KB} {unit('KB')}" if v >= KB else f"{v} {unit('Bytes')}"


def size_text(b):
    units = ["Bytes", "KB", "MB", "GB", "TB"]
    v, i = float(b), 0
    while v >= 1024 and i < len(units) - 1:
        v /= 1024
        i += 1
    text = f"{v:.1f}".rstrip("0").rstrip(".") if i else str(int(v))
    return number(text) + " " + unit(units[i])


def mbr_limit(ss):
    """MBR zählt höchstens 2^32 Sektoren: 2 TB bei 512-Byte-, 16 TB bei 4K-Sektoren"""
    return 0xFFFFFFFF * max(512, ss or 512)


def uses_gpt(size, scheme, ss):
    """Wie der Helfer: automatisch erst GPT, wenn MBR den Datenträger nicht mehr fassen kann"""
    return scheme == "gpt" or (scheme == "auto" and size - MB > mbr_limit(ss))


def usable_bytes(size, scheme, ss=512):
    """Platz für Partitionen: ab 1 MiB; GPT hinten mit Sicherungskopie, MBR höchstens bis zur MBR-Grenze"""
    if uses_gpt(size, scheme, ss):   # wie layout_disk: hinten Sicherungskopie (Kopf + 16 KB Einträge)
        ss = max(512, ss or 512)
        return max(0, size - MB - (1 + 16384 // ss) * ss)
    return max(0, min(size, mbr_limit(ss)) - MB)


def partition_bytes(size, scheme, ss=512):
    """Ungefähre Größe der einen Partition im einfachen Modus"""
    return usable_bytes(size, scheme, ss)


def cluster_choices(fs, part, ss):
    """Gültige Zuordnungseinheiten (Bytes) und die Standardgröße für eine Partition dieser Größe"""
    ss = max(512, ss or 512)
    pow2 = [512 << i for i in range(17)]   # 512 Bytes bis 32 MB
    if fs == "exfat":
        std = 4 * KB if part <= 256 * MB else 32 * KB if part <= 32 * GB else 128 * KB
        # mkfs_exfat verlangt mindestens 8 MB
        vals = [v for v in pow2 if ss <= v <= 32 * MB and 16 <= part // v <= 0xFFFFFFF5] if part >= 8 * MB else []
    elif fs == "fat32":   # FAT32 braucht 65.525 bis 268.435.444 Cluster (mit etwas Abstand)
        std = 4 * KB if part <= 8 * GB else 8 * KB if part <= 16 * GB else 16 * KB if part <= 32 * GB else 32 * KB
        # und höchstens 2^32 Sektoren (32-Bit-Feld im Bootsektor) - sonst lehnt der Helfer ab
        vals = [v for v in pow2 if ss <= v <= 64 * KB and 66000 <= part // v <= 268000000]             if part // ss <= 0xFFFFFFFF else []
    elif fs == "ntfs":
        std = next((c for lim, c in ((16 * TB, 4 * KB), (32 * TB, 8 * KB), (64 * TB, 16 * KB), (128 * TB, 32 * KB))
                    if part <= lim), 64 * KB)
        vals = [v for v in pow2 if ss <= v <= 2 * MB and part // v <= 0xFFFFFFFE]
    elif fs == "fat":   # FAT16: 4.085 bis 65.524 Cluster; unter 16 MB wird es (wie bei Windows) FAT12 mit höchstens 4.084
        std = next((c for lim, c in ((32 * MB, 512), (64 * MB, KB), (128 * MB, 2 * KB), (256 * MB, 4 * KB),
                                     (512 * MB, 8 * KB), (GB, 16 * KB), (2 * GB, 32 * KB)) if part <= lim), 64 * KB)
        lo, hi = (4085, FAT16_MAX_CLUSTERS) if part >= 16 * MB else (1, 4084)
        vals = [v for v in pow2 if ss <= v <= 64 * KB and lo <= part // v <= hi]
    elif fs in ("ext2", "ext3", "ext4"):   # wie mke2fs: kleine Datenträger 1 KB, sonst 4 KB
        std = KB if part < 512 * MB else 4 * KB
        vals = [v for v in (KB, 2 * KB, 4 * KB) if v >= ss and part // v >= 64 and part // v <= 0xFFFFFFFF]
    elif fs == "hfsplus":
        std = 4 * KB
        vals = [v for v in pow2 if ss <= v <= 64 * KB and 64 <= part // v <= 0xFFFFFFFF] if part >= 8 * MB else []
    else:   # ohne Dateisystem
        return [0], 0
    if sys.platform == "win32" and fs in ("ntfs", "fat") and part < 8 * MB:
        vals = []   # Windows: der Helfer legt dafür erst ein vorläufiges exFAT an (mindestens 8 MB)
    if vals and std not in vals:   # Standard passt nicht (sehr kleiner/großer Datenträger): nächstgültige Größe
        std = min(vals, key=lambda v: abs(v.bit_length() - std.bit_length()))
    return vals, std


def fs_fits(name, part, ss):
    return name == NO_FS or bool(cluster_choices(FILESYSTEMS[name][0], part, ss)[0])


def available_filesystems(dev, scheme, expert=False):
    names = FS_EXPERT if expert else list(FILESYSTEMS)
    if not dev:
        return names
    if dev["size"] < 16 * MB:   # der Helfer formatiert erst ab 16 MB
        return []
    part = partition_bytes(dev["size"], scheme, dev.get("ss", 512))
    return [n for n in names if fs_fits(n, part, dev.get("ss", 512))]


def plan(parts, size, scheme, ss=512):
    """Lage der Partitionen wie im Helfer (jede an einer MiB-Grenze, Größen auf ganze MiB abgerundet).
    parts: Liste von dict mit "bytes" (0 = Rest). Liefert (größen, frei, fehler) - größen in Bytes je Partition."""
    usable_end = MB + usable_bytes(size, scheme, ss)
    at, sizes = MB, []
    if size < 16 * MB:   # wie der Helfer
        return [], 0, _("Der Datenträger ist zu klein (mindestens 16 MB).")
    if not parts:
        return [], usable_end - MB, _("Mindestens eine Partition anlegen.")
    if not uses_gpt(size, scheme, ss) and len(parts) > 4:
        return [], 0, _("MBR fasst höchstens 4 Partitionen – bitte GPT wählen.")
    if len(parts) > MAX_PARTS:
        return [], 0, _("Höchstens {n} Partitionen.", n=MAX_PARTS)
    for i, p in enumerate(parts):
        at = (at + MB - 1) // MB * MB
        if p["bytes"]:
            n = p["bytes"] // MB * MB
            if n < MB:
                return sizes, 0, _("Partition {n} ist kleiner als 1 MB.", n=i + 1)
        else:
            if i != len(parts) - 1:
                return sizes, 0, _("Nur die letzte Partition darf den Rest bekommen.")
            n = max(0, usable_end - at)
        if at + n > usable_end or n < MB:
            missing = at + n - usable_end
            return sizes, 0, (_("Partition {n} passt nicht mehr – es fehlen {size}.", n=i + 1,
                                size=size_text(max(missing, MB)))
                              if missing > 0 else _("Für Partition {n} ist kein Platz mehr.", n=i + 1))
        sizes.append(n)
        at += n
    return sizes, max(0, usable_end - at), None


def part_arg(p, cluster):
    """part=<fs>,<bytes|rest>,<cluster>,<typ>,<aktiv>,<bezeichnung> für den Helfer"""
    return (f"part={helper_fs(p['fs'])},{p['bytes'] or 'rest'},{cluster},{p.get('ptype', 'auto')},"
            f"{1 if p.get('active') else 0},{p.get('label', '')}")
