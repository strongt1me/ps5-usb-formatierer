"""Gebautes Programm (PyInstaller, eine Datei) prüfen: steckt genau diese usb_format.elf darin und die erwartete Version?
Aufruf: python tests/elf_in_program.py <programm> <usb_format.elf> <version>   (Exit 1 bei Abweichung)"""
import hashlib
import sys

from PyInstaller.archive.readers import CArchiveReader

prog, elf, version = sys.argv[1], sys.argv[2], sys.argv[3]
arc = CArchiveReader(prog)
fails = 0


def check(name, ok, extra=""):
    global fails
    fails += not ok
    print(("OK      " if ok else "FEHLER  ") + name + ("" if ok else f"  [{extra}]"))


inside = hashlib.sha256(arc.extract("usb_format.elf")).hexdigest()
want = hashlib.sha256(open(elf, "rb").read()).hexdigest()
check("eingebaute usb_format.elf = Release-ELF", inside == want, (inside[:16], want[:16]))
got = arc.extract("VERSION").decode("ascii", "replace").strip()
check(f"Version {version}", got == version, got)
check("zstd-Lizenz enthalten", any("lizenzen/zstd" in n.replace("\\", "/") for n in arc.toc))
sys.exit(1 if fails else 0)
