"""Gezielt kaputte Abbilder für test/fsread_fuzz.sh (Fallen, die zufällige Änderungen kaum treffen).
    fsread_craft.py exfat-loop    <ein> <aus>   Ordner "schleife" zeigt auf den Wurzelordner zurück
    fsread_craft.py exfat-fatloop <ein> <aus>   data/gross.bin: 1 TB lang, FAT-Kette zeigt auf sich selbst
    fsread_craft.py ufs-loop      <ein> <aus>   Ordner "schleife" zeigt auf die Wurzel (Inode 2)"""
import struct
import sys

mode, src, dst = sys.argv[1:4]
img = bytearray(open(src, "rb").read())


def stream_of(name):
    """Offset des Stream-Extension-Eintrags (0xC0) vor dem ersten Namenseintrag (0xC1) mit diesem Namen"""
    pat = b"\xc1\x00" + name.encode("utf-16-le")
    i = img.find(pat)
    assert i > 32 and img[i - 32] == 0xC0, "Eintrag nicht gefunden: " + name
    return i - 32


if mode.startswith("exfat"):
    ss, spc = 1 << img[108], 1 << img[109]
    fat_off, root = struct.unpack_from("<I", img, 80)[0] * ss, struct.unpack_from("<I", img, 96)[0]
    s = stream_of("schleife" if mode == "exfat-loop" else "gross.bin")
    if mode == "exfat-loop":
        struct.pack_into("<I", img, s + 20, root)               # erster Cluster = Wurzelordner
        struct.pack_into("<Q", img, s + 24, ss * spc)           # Länge: ein Cluster
        img[s + 1] &= ~2 & 0xFF                                 # über die FAT (wie die Wurzel)
    else:
        clus = struct.unpack_from("<I", img, s + 20)[0]
        struct.pack_into("<Q", img, s + 8, 1 << 40)             # ValidDataLength
        struct.pack_into("<Q", img, s + 24, 1 << 40)            # DataLength: 1 TB
        img[s + 1] &= ~2 & 0xFF                                 # FAT-Kette benutzen ...
        struct.pack_into("<I", img, fat_off + clus * 4, clus)   # ... die auf sich selbst zeigt
elif mode == "ufs-loop":
    name = b"schleife"
    pat = bytes([4, len(name)]) + name   # d_type DT_DIR, d_namlen, Name
    i = img.find(pat)
    assert i >= 6, "Eintrag nicht gefunden"
    struct.pack_into("<I", img, i - 6, 2)   # d_ino = Wurzel
else:
    sys.exit("unbekannt: " + mode)
open(dst, "wb").write(img)
