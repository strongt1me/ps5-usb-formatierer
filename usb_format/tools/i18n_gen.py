"""Übersetzungstabelle der C-Meldungen erzeugen (src/i18n_table.c) aus i18n/core.json.
Findet alle T("...") in src/ (auch "a" "b" aneinandergehängt), prüft Vollständigkeit und dass die %-Platzhalter in
jeder Sprache genau gleich sind (sonst stürzte printf ab).
    python tools/i18n_gen.py            prüfen und Tabelle schreiben (Exit 1 bei Fehlern)
    python tools/i18n_gen.py --missing  fehlende Texte als JSON-Gerüst ausgeben"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
CATALOG = os.path.join(ROOT, "i18n", "core.json")
OUT = os.path.join(SRC, "i18n_table.c")
LANGS = ["en", "it", "es", "fr"]
STR = r'"(?:[^"\\\n]|\\.)*"'
LITS = r'((?:' + STR + r'\s*)+)'
# Stellen, deren Text zur Laufzeit übersetzt wird: T("..."), N_("...") und Funktionen, die ihren Text selbst
# durch T() schicken (mkfs: fail/report/dev_zero, Helfer: on_progress/finish/win_error/ADD, devtest: progress,
# Server: sb_tr/respond_error)
CALLS = [
    re.compile(r'\b(?:T|N_|fail|mk_fail|mk_summary|progress|on_progress|ADD)\(\s*' + LITS + r'\s*[,)]'),
    re.compile(r'\b(?:report|mk_report)\(\s*[\w>.-]+\s*,\s*' + LITS + r'\s*,'),
    re.compile(r'\bsb_tr\(\s*&?[\w>.-]+\s*,\s*' + LITS + r'\s*[,)]'),
    re.compile(r'\brespond_error\(\s*\w+\s*,\s*\d+\s*,\s*' + LITS + r'\s*\)'),
    re.compile(r'\bfinish\(\s*[01]\s*,\s*' + LITS + r'\s*,'),
    re.compile(r'\bwin_error\(\s*[^,]+,\s*[^,]+,\s*' + LITS + r'\s*\)'),
    re.compile(r'\b(?:dev_zero|mk_zero)\((?:[^;"()]|\([^()]*\))*,\s*' + LITS + r'\s*\)'),
]
SPEC = re.compile(r"%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diouxXeEfgGcsp])")


def c_unescape(lit):
    out, i = [], 0
    body = lit[1:-1]
    simple = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'", "r": "\r", "0": "\0"}
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body):
            out.append(simple.get(body[i + 1], body[i + 1]))
            i += 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def c_escape(s):
    b = []
    for ch in s:
        if ch == "\\":
            b.append("\\\\")
        elif ch == '"':
            b.append('\\"')
        elif ch == "\n":
            b.append("\\n")
        elif ch == "\t":
            b.append("\\t")
        else:
            b.append(ch)
    return '"' + "".join(b) + '"'


def source_texts():
    found = {}
    files = glob.glob(os.path.join(SRC, "*.c")) + glob.glob(os.path.join(SRC, "win", "*.c"))
    for path in files:
        if path.endswith("i18n_table.c"):
            continue
        text = open(path, encoding="utf-8").read()
        text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)   # Kommentare
        for pat in CALLS:
            for m in pat.finditer(text):
                key = "".join(c_unescape(lit) for lit in re.findall(STR, m.group(1)))
                if not re.search(r"[A-Za-zÄÖÜäöüß„“]", SPEC.sub("", key)):   # deutsche Anführungszeichen: übersetzen
                    continue   # nur Satzzeichen/Platzhalter: in allen Sprachen gleich
                line = text.count("\n", 0, m.start()) + 1
                found.setdefault(key, f"{os.path.relpath(path, SRC)}:{line}")
    return found


def main():
    texts = source_texts()
    try:
        cat = json.load(open(CATALOG, encoding="utf-8"))
    except FileNotFoundError:
        cat = {}
    if "--missing" in sys.argv:
        json.dump({t: {lg: "" for lg in LANGS} for t in texts if t not in cat}, sys.stdout, ensure_ascii=False,
                  indent=1)
        return 0
    bad = 0
    for t, where in sorted(texts.items(), key=lambda x: x[1]):
        tr = cat.get(t)
        if not tr:
            print(f"FEHLT    {where}: {t[:70]!r}")
            bad += 1
            continue
        for lg in LANGS:
            if not tr.get(lg):
                print(f"FEHLT {lg} {where}: {t[:70]!r}")
                bad += 1
            elif SPEC.findall(tr[lg]) != SPEC.findall(t):
                print(f"PLATZHALTER {lg} {where}: {SPEC.findall(t)} != {SPEC.findall(tr[lg])}: {t[:50]!r}")
                bad += 1
    unused = [t for t in cat if t not in texts]
    for t in unused:
        print(f"UNBENUTZT: {t[:70]!r}")
    if bad:
        print(f"{len(texts)} Texte, {bad} Probleme - Tabelle nicht geschrieben")
        return 1
    keys = sorted((t for t in texts), key=lambda s: s.encode("utf-8"))
    lines = ["/* Erzeugt von tools/i18n_gen.py aus i18n/core.json - nicht von Hand ändern */",
             '#include "i18n.h"', "", "const i18n_entry_t i18n_table[] = {"]
    for t in keys:
        lines.append("    {{" + ", ".join(c_escape(s) for s in [t] + [cat[t][lg] for lg in LANGS]) + "}},")
    lines += ["};", f"const unsigned i18n_count = {len(keys)};", ""]
    new = "\n".join(lines)
    old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
    if new != old:
        open(OUT, "w", encoding="utf-8", newline="\n").write(new)
    print(f"{len(texts)} Texte, Tabelle {'geschrieben' if new != old else 'unverändert'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
