"""CHANGELOG.md (deutsch) und CHANGELOG.en.md im Format Keep a Changelog: lesen, prüfen und beim Release den Abschnitt
"Unveröffentlicht"/"Unreleased" zur neuen Version machen. Genutzt von release.py, als Prüfung direkt aufrufbar:

    python tools/changelog.py            prüft beide Dateien (auch in der CI), Exit 1 bei Fehlern
    python tools/changelog.py --tags     zusätzlich: jede Version mit Git-Tag hat einen Eintrag (braucht die Tags)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "strongt1me/ps5-usb-formatierer"
# Sprache: Datei, Name des Abschnitts für noch nicht veröffentlichte Änderungen, Rubriken in fester Reihenfolge
FILES = {
    "de": ("CHANGELOG.md", "Unveröffentlicht", ("Hinzugefügt", "Geändert", "Veraltet", "Entfernt", "Behoben", "Sicherheit")),
    "en": ("CHANGELOG.en.md", "Unreleased", ("Added", "Changed", "Deprecated", "Removed", "Fixed", "Security")),
}
VERSION_RE = r"\d+\.\d+\.\d+"


def path(lang):
    return os.path.join(ROOT, FILES[lang][0])


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def blocks(text):
    """Abschnitte der Datei: [(name, datum, rumpf)] in Dateireihenfolge - name ist die Version oder der
    Unveröffentlicht-Name, datum "" beim Unveröffentlicht-Abschnitt"""
    out = []
    parts = re.split(r"(?m)^## ", text)[1:]
    for part in parts:
        head, _, body = part.partition("\n")
        m = re.fullmatch(r"\[([^\]]+)\](?: – (\d{4}-\d{2}-\d{2}))?\s*", head)
        body = re.split(r"(?m)^\[[^\]]+\]: ", body)[0]   # Linkverweise am Dateiende gehören nicht dazu
        out.append((m.group(1) if m else head, (m.group(2) or "") if m else None, body))
    return out


def sections(body, kinds):
    """Rubriken eines Abschnitts: [(rubrik, punkte)] in fester Reihenfolge, leere weggelassen"""
    found, kind = {}, None
    for line in body.replace("\r\n", "\n").split("\n"):
        m = re.match(r"### +(.+?)\s*$", line)
        if m:
            kind = m.group(1)
            continue
        if kind:
            found.setdefault(kind, []).append(line)
    return [(k, "\n".join(found[k]).strip()) for k in kinds if "\n".join(found.get(k, [])).strip()]


def unreleased(lang, version=None):
    """Rubriken des Unveröffentlicht-Abschnitts - bzw. der Version, wenn sie schon geschnitten ist"""
    _file, label, kinds = FILES[lang]
    bl = {name: body for name, _date, body in blocks(read(path(lang)))}
    body = bl.get(version) if version in bl else bl.get(label, "")
    return sections(body, kinds)


def cut(lang, version, date):
    """Unveröffentlicht -> neue Version: Überschrift mit Datum, darüber ein neuer leerer Unveröffentlicht-Abschnitt,
    unten die Linkverweise angepasst. Neuer Text der Datei."""
    _file, label, kinds = FILES[lang]
    text = read(path(lang))
    if re.search(rf"(?m)^## \[{re.escape(version)}\]", text):
        return text   # schon geschnitten (z. B. zweiter --publish-Versuch)
    m = re.search(rf"(?ms)^## \[{re.escape(label)}\]\n(.*?)(?=^## |^\[[^\]]+\]: |\Z)", text)
    if not m:
        raise ValueError(f"{FILES[lang][0]}: Abschnitt ## [{label}] fehlt")
    body = "\n\n".join(f"### {k}\n\n{v}" for k, v in sections(m.group(1), kinds))   # Rubriken in fester Reihenfolge
    text = text[:m.start()] + f"## [{label}]\n\n## [{version}] – {date}\n\n{body}\n\n" + text[m.end():]
    text = re.sub(rf"(?m)^\[{re.escape(label)}\]: .*$",
                  f"[{label}]: https://github.com/{REPO}/compare/v{version}...HEAD\n"
                  f"[{version}]: https://github.com/{REPO}/releases/tag/v{version}", text, count=1)
    return text


def release_body(version, extra=""):
    """Text des GitHub-Releases: Rubriken aus beiden Changelogs (Englisch aufklappbar), freier Zusatztext, Aktualisieren
    und Dateien (aus Unveröffentlicht oder, schon geschnitten, aus dem Abschnitt der Version)"""
    def md(lang, level):
        return "\n\n".join(f"{level} {k}\n\n{v}" for k, v in unreleased(lang, version))
    parts = [md("de", "##")]
    if extra.strip():
        parts.append(extra.strip())
    parts.append(UPDATE_DE)
    parts.append("<details><summary>English</summary>\n\n" + md("en", "####") + "\n\n" + UPDATE_EN + "\n\n</details>")
    return "\n\n".join(parts) + "\n"


UPDATE_DE = """## Aktualisieren

Am einfachsten im PS5-USB-Formatierer auf „ELF senden“ klicken. Für die App-Kachel den Ordner `PPSA86920` aus dem ZIP nach `/data/homebrew/` kopieren und in der Weboberfläche „Kachel neu anmelden“ wählen.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows: an der PS5 (übers Netz) und am PC, mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Image-Sicherung |
| `…-x86_64.AppImage`, `…-aarch64.AppImage` | dieselben Programme für Linux (Intel/AMD bzw. ARM64), `.zsync` für „Jetzt aktualisieren“ |
| `…-linux-x86_64.tar.gz`, `…-linux-aarch64.tar.gz` | Linux als Archiv: entpacken und starten |
| `usb_format.elf` | Formatierer als Payload für den ELF-Loader (Port 9021) |
| `PPSA86920.zip` | App-Kachel |
| `SHA256SUMS` | Prüfsummen aller Dateien |
"""

UPDATE_EN = """#### Updating

The easiest way is "Send ELF" in the PS5 USB Formatter. For the app tile, copy the folder `PPSA86920` from the ZIP to `/data/homebrew/` and choose "Re-register tile" in the web interface. `.exe` for Windows, `.AppImage`/`.tar.gz` for Linux, `usb_format.elf` for the ELF loader (port 9021), `SHA256SUMS` with the checksums."""


def check(tags=False):
    """Fehlerliste für beide Dateien"""
    errs, versions = [], {}
    for lang, (name, label, kinds) in FILES.items():
        text = read(path(lang))
        bl = blocks(text)
        if not bl or bl[0][0] != label:
            errs.append(f"{name}: erster Abschnitt muss ## [{label}] sein")
        seen, prev = [], None
        for ver, date, body in bl:
            if ver == label:
                continue
            if not re.fullmatch(VERSION_RE, ver) or not date:
                errs.append(f"{name}: Überschrift ## [{ver}] – JJJJ-MM-TT erwartet")
                continue
            t = tuple(map(int, ver.split(".")))
            if prev and t >= prev:
                errs.append(f"{name}: {ver} steht nicht absteigend sortiert")
            prev = t
            heads = re.findall(r"(?m)^### +(.+?)\s*$", body)
            if not heads:
                errs.append(f"{name}: {ver} hat keine Rubrik")
            bad = [h for h in heads if h not in kinds]
            if bad:
                errs.append(f"{name}: {ver} hat unbekannte Rubrik(en) {bad} - erlaubt: {', '.join(kinds)}")
            elif heads != [k for k in kinds if k in heads]:
                errs.append(f"{name}: {ver}: Rubriken in der Reihenfolge {', '.join(kinds)}")
            if re.search(r"(?m)^#{1,2} |^#### ", body):
                errs.append(f"{name}: {ver}: nur ### als Überschrift im Abschnitt")
            for k, v in sections(body, kinds):
                if any(ln and not ln.startswith(("- ", "  ")) for ln in v.split("\n")):
                    errs.append(f"{name}: {ver} {k}: nur Aufzählungspunkte (- …)")
            seen.append(ver)
        refs = dict(re.findall(r"(?m)^\[([^\]]+)\]: (\S+)$", text))
        for ver in seen:
            if refs.get(ver) != f"https://github.com/{REPO}/releases/tag/v{ver}":
                errs.append(f"{name}: Linkverweis [{ver}] fehlt oder stimmt nicht")
        if seen and refs.get(label) != f"https://github.com/{REPO}/compare/v{seen[0]}...HEAD":
            errs.append(f"{name}: Linkverweis [{label}] muss auf compare/v{seen[0]}...HEAD zeigen")
        extra = [r for r in refs if r not in seen and r != label and re.fullmatch(VERSION_RE, r)]
        if extra:
            errs.append(f"{name}: Linkverweise ohne Abschnitt: {', '.join(extra)}")
        versions[lang] = seen
    if versions["de"] != versions["en"]:
        only = sorted(set(versions["de"]) ^ set(versions["en"]))
        errs.append("CHANGELOG.md und CHANGELOG.en.md nennen nicht dieselben Versionen" + (f": {', '.join(only)}" if only else
                                                                                          " (Reihenfolge)"))
    else:   # dieselben Rubriken je Version in beiden Sprachen
        de, en = FILES["de"][2], FILES["en"][2]
        dmap = {v: [de.index(k) for k, _ in sections(b, de)] for v, _d, b in blocks(read(path("de")))}
        emap = {v: [en.index(k) for k, _ in sections(b, en)] for v, _d, b in blocks(read(path("en")))}
        for v in versions["de"]:
            if dmap.get(v) != emap.get(v):
                errs.append(f"{v}: Rubriken in CHANGELOG.md und CHANGELOG.en.md verschieden")
        if [de.index(k) for k, _ in unreleased("de")] != [en.index(k) for k, _ in unreleased("en")]:
            errs.append("Unveröffentlicht/Unreleased: Rubriken in beiden Sprachen verschieden")
    current = read(os.path.join(ROOT, "VERSION")).strip()
    if versions["de"] and versions["de"][0] != current:
        errs.append(f"neueste Version im Changelog ist {versions['de'][0]}, VERSION sagt {current}")
    if tags:
        out = subprocess.run(["git", "tag", "-l", "v*"], cwd=ROOT, capture_output=True, text=True).stdout.split()
        missing = [t[1:] for t in out if re.fullmatch(VERSION_RE, t[1:]) and t[1:] not in versions["de"]]
        if missing:
            errs.append("Tags ohne Changelog-Eintrag: " + ", ".join(sorted(missing, key=lambda v: tuple(map(int, v.split("."))))))
    return errs


if __name__ == "__main__":
    errors = check("--tags" in sys.argv)
    for e in errors:
        print("FEHLER:", e)
    print("Changelog in Ordnung" if not errors else f"{len(errors)} Fehler")
    sys.exit(1 if errors else 0)
