"""Übersetzungen der Windows-Programme prüfen: findet alle Texte _("...") im Code (und die Tabellen in fsinfo,
partition_editor, usb_formatierer) und vergleicht mit i18n_app.json.
    python i18n_check.py            Bericht; Exit 1 bei fehlenden/falschen Übersetzungen
    python i18n_check.py --missing  fehlende Texte als JSON-Gerüst ausgeben (zum Übersetzen)"""
import ast
import json
import os
import string
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCES = ["usb_formatierer.py", "usb_standard.py", "standard_panel.py", "partition_editor.py", "pc_common.py",
           "pc_linux.py", "fsinfo.py", "ui_msgbox.py", "ui_widgets.py", "ui_theme.py"]
LANGS = ["en", "it", "es", "fr"]


def code_texts():
    found = {}
    for name in SOURCES:
        tree = ast.parse(open(os.path.join(HERE, name), encoding="utf-8").read())
        for node in ast.walk(tree):
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "_" and node.args
                    and isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str)):
                found.setdefault(node.args[0].value, f"{name}:{node.lineno}")
    sys.path.insert(0, HERE)
    import fsinfo
    import partition_editor
    import standard_panel
    import usb_formatierer
    for text in [h for _f, _m, h in fsinfo.FILESYSTEMS.values()] + [t for _k, t in fsinfo.SCHEMES] + \
            [t for _k, t in fsinfo.PTYPES] + list(partition_editor.PTYPE_SHORT.values()) + \
            [t for _k, t in usb_formatierer.MODES] + [t for _k, t in standard_panel.PURPOSES] +             [g for g, _i in usb_formatierer.NAV] + [t for _g, items in usb_formatierer.NAV for _k, t in items] + [fsinfo.NO_FS]:
        found.setdefault(text, "Tabelle")
    return found


def fields(text):
    return sorted(f for _lit, f, _spec, _conv in string.Formatter().parse(text) if f)


def main():
    texts = code_texts()
    path = os.path.join(HERE, "i18n_app.json")
    try:
        cat = json.load(open(path, encoding="utf-8"))
    except FileNotFoundError:
        cat = {}
    if "--missing" in sys.argv:
        missing = {t: {lg: "" for lg in LANGS} for t in texts if t not in cat}
        json.dump(missing, sys.stdout, ensure_ascii=False, indent=1)
        return 0
    bad = 0
    for t, where in sorted(texts.items(), key=lambda x: x[1]):
        tr = cat.get(t)
        if not tr:
            print(f"FEHLT    {where}: {t[:70]}")
            bad += 1
            continue
        for lg in LANGS:
            if not tr.get(lg):
                print(f"FEHLT {lg} {where}: {t[:70]}")
                bad += 1
            elif fields(tr[lg]) != fields(t):
                print(f"PLATZHALTER {lg} {where}: {t[:50]} -> {tr[lg][:50]}")
                bad += 1
    for t in cat:
        if t not in texts:
            print(f"UNBENUTZT: {t[:70]}")
    print(f"{len(texts)} Texte, {bad} Probleme")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
