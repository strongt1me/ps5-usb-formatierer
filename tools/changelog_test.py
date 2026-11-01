"""Test für tools/changelog.py an Kopien beider Changelogs: Prüfung erkennt Formfehler, Schneiden beim Release
(Unveröffentlicht -> Version, Linkverweise), Release-Text, zweiter Versuch ändert nichts"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import changelog as C  # noqa: E402

REAL = C.ROOT
fails = 0


def check(name, ok, extra=""):
    global fails
    print(("OK      " if ok else "FEHLER  ") + name + ("" if ok else f"  [{extra}]"))
    fails += not ok


def setup(version=None):
    d = tempfile.mkdtemp(prefix="changelog-")
    for lang in C.FILES:
        shutil.copy(os.path.join(REAL, C.FILES[lang][0]), d)
    shutil.copy(os.path.join(REAL, "VERSION"), d)
    if version:
        with open(os.path.join(d, "VERSION"), "w") as f:
            f.write(version + "\n")
    C.ROOT = d
    return d


def edit(lang, old, new):
    p = C.path(lang)
    t = C.read(p)
    assert old in t, old
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(t.replace(old, new, 1))


def write(lang, text):
    with open(C.path(lang), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


try:
    setup()
    check("echte Dateien fehlerfrei", C.check() == [], C.check())

    cur = C.read(os.path.join(REAL, "VERSION")).strip()
    for name, lang, old, new, want in [
        ("unbekannte Rubrik", "de", "### Entfernt\n", "### Weg\n", "unbekannte Rubrik"),
        ("falsche Reihenfolge der Rubriken", "de", "### Geändert\n\n- Weboberfläche: „Formatierer stoppen“",
         "### Sicherheit\n\n- x\n\n### Geändert\n\n- Weboberfläche: „Formatierer stoppen“", "Reihenfolge"),
        ("Fließtext statt Punkte", "en", "### Removed\n\n- Web", "### Removed\n\nText\n- Web", "Aufzählungspunkte"),
        ("fehlender Linkverweis", "en", f"[{cur}]: https", f"[x{cur}]: https", f"Linkverweis [{cur}]"),
        ("Rubriken der Sprachen verschieden", "en", "### Removed\n\n- Web", "### Fixed\n\n- Web", "verschieden"),
        ("Datum fehlt", "de", f"## [{cur}] – ", f"## [{cur}]  ", "Überschrift"),
    ]:
        setup()
        edit(lang, old, new)
        errs = C.check()
        check("erkannt: " + name, any(want in e for e in errs), errs)

    setup()
    t = C.read(C.path("en"))
    write("en", t.replace(f"## [{cur}] – ", "## [0.0.1] – ", 1))
    check("erkannt: Versionen der Sprachen verschieden", any("dieselben Versionen" in e or "absteigend" in e
                                                            for e in C.check()), C.check())

    # Release schneiden
    new = "%d.%d.%d" % (*map(int, cur.split(".")[:2]), int(cur.split(".")[2]) + 1)
    setup(new)
    edit("de", "## [Unveröffentlicht]\n", "## [Unveröffentlicht]\n\n### Behoben\n\n- Testpunkt B\n\n### Hinzugefügt\n\n- Testpunkt A\n")
    edit("en", "## [Unreleased]\n", "## [Unreleased]\n\n### Fixed\n\n- Test item B\n\n### Added\n\n- Test item A\n")
    old_de = C.unreleased("de")
    body = C.release_body(new, "Zusatztext")
    check("Release-Text: Rubriken in fester Reihenfolge, Zusatz, Aktualisieren, Englisch",
          body.index("## Hinzugefügt") < body.index("## Behoben") < body.index("Zusatztext") < body.index("## Aktualisieren")
          < body.index("<summary>English</summary>") < body.index("#### Added") < body.index("#### Fixed"), body)
    for lang in C.FILES:
        write(lang, C.cut(lang, new, "2099-01-02"))
    errs = C.check()
    check("nach dem Schneiden fehlerfrei", errs == [], errs)
    de = C.read(C.path("de"))
    check("neue Version unter leerem Unveröffentlicht", de.index("## [Unveröffentlicht]\n\n## [" + new + "] – 2099-01-02")
          >= 0 and C.unreleased("de") == [], de[:600])
    check("Inhalt wanderte mit", C.unreleased("de", new) == old_de, C.unreleased("de", new))
    check("Linkverweise angepasst", f"[Unveröffentlicht]: https://github.com/{C.REPO}/compare/v{new}...HEAD\n"
          f"[{new}]: https://github.com/{C.REPO}/releases/tag/v{new}\n[{cur}]:" in de)
    check("Release-Text beim zweiten Versuch aus der Version", C.release_body(new, "Zusatztext") == body)
    for lang in C.FILES:
        before = C.read(C.path(lang))
        check(f"zweites Schneiden ändert nichts ({lang})", C.cut(lang, new, "2099-01-03") == before)
finally:
    C.ROOT = REAL

print(f"\n{'alles bestanden' if not fails else str(fails) + ' fehlgeschlagen'}")
sys.exit(1 if fails else 0)
