"""Hilfswerkzeug für die Übersetzung: setzt deutsche Formattexte in snprintf(...)-Aufrufen einer C-Datei in T(...).
    python tools/i18n_wrap.py <datei.c> [--dry]
snprintf(buf, n, "Text %s", x)  ->  snprintf(buf, n, T("Text %s"), x)
snprintf(buf, n, "Text")        ->  snprintf(buf, n, "%s", T("Text"))
JSON-, Protokoll- und reine Platzhalter-Formate bleiben unverändert; schon übersetzte (T(...)) auch."""
import re
import sys

GERMAN = re.compile(r"[äöüÄÖÜß]|\b(der|die|das|den|dem|nicht|und|mit|ist|sind|wird|werden|bitte|Bitte|kein|keine|"
                    r"Kein|Keine|für|bei|von|nur|noch|auf|aus|zu|Datenträger|Laufwerk|Fehler|Partition|Ordner|"
                    r"Bezeichnung|Ungültig\w*|ungültig\w*|Spiel\w*|Stick|Abbild|Zugriff|erfolgreich|fehlgeschlagen|"
                    r"Abgebrochen|Prüfung|gefunden|Formatieren|Umbenennen|Tempo|Lesetempo|Schreibtempo|Echtheit|"
                    r"Speicher|Partitionen|Cluster|echt|lesen|schreiben|Jetzt|Windows)\b")
STR = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def split_args(s, start):
    """Argumente ab der öffnenden Klammer bei start: Liste von (anfang, ende) und Index der schließenden Klammer"""
    depth, i, args, a0 = 0, start, [], start + 1
    while i < len(s):
        c = s[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while s[i] != q:
                i += 2 if s[i] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append((a0, i))
                return args, i
        elif c == "," and depth == 1:
            args.append((a0, i))
            a0 = i + 1
        i += 1
    raise ValueError("Klammer nicht geschlossen")


def wrap(src):
    out, pos, changed = [], 0, 0
    for m in re.finditer(r"\bsnprintf\(", src):
        if m.start() < pos:
            continue
        open_ = m.end() - 1
        args, close = split_args(src, open_)
        if len(args) < 3:
            continue
        a, b = args[2]
        fmt = src[a:b].strip()
        lits = STR.findall(fmt)
        if not lits or STR.sub("", fmt).strip():   # kein reines Literal (schon T(...), Variable, Bedingung)
            continue
        text = "".join(l[1:-1] for l in lits)
        if text.startswith("{") or text.startswith("R\\t") or text.startswith("P\\t") or not GERMAN.search(text):
            continue
        lead = src[a:b][:len(src[a:b]) - len(src[a:b].lstrip())]
        body = src[a:b].strip()
        if len(args) == 3:
            new = f'{lead}"%s", T({body})'
        else:
            new = f"{lead}T({body})"
        out.append(src[pos:a])
        out.append(new)
        pos = b
        changed += 1
    out.append(src[pos:])
    return "".join(out), changed


def main():
    path = sys.argv[1]
    src = open(path, encoding="utf-8").read()
    new, n = wrap(src)
    if "--dry" not in sys.argv and n:
        open(path, "w", encoding="utf-8", newline="\n").write(new)
    print(f"{path}: {n} snprintf-Formate übersetzbar gemacht")


if __name__ == "__main__":
    main()
