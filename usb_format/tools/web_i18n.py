"""Übersetzungen der Weboberfläche (web/index.html) aus i18n/web.json einsetzen.
Findet alle tr("...")/N_("...") im Skript und die festen Texte der Seite (Textknoten, placeholder, aria-label, title;
ohne translate="no"), prüft Vollständigkeit und dass die {platzhalter} in jeder Sprache dieselben sind, und schreibt die
Tabelle zwischen /*I18N*/ und /*I18N-END*/ in die Seite.
    python tools/web_i18n.py            prüfen und Tabelle einsetzen (Exit 1 bei Fehlern)
    python tools/web_i18n.py --check    nur prüfen (auch ob die eingesetzte Tabelle aktuell ist)
    python tools/web_i18n.py --missing  fehlende Texte als JSON-Gerüst ausgeben"""
import html.parser
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "web", "index.html")
CATALOG = os.path.join(ROOT, "i18n", "web.json")
LANGS = ["en", "it", "es", "fr"]
CALL = re.compile(r'\b(?:tr|N_)\(\s*"((?:[^"\\\n]|\\.)*)"')
VARS = re.compile(r"\{(\w+)\}")
TABLE = re.compile(r"/\*I18N\*/.*?/\*I18N-END\*/", re.S)


def translatable(text):
    """Nur Texte mit Wörtern; reine Abkürzungen (MBR, GPT, OK) und Zahlen bleiben in allen Sprachen gleich"""
    return re.search(r"[A-Za-zÄÖÜäöüß]", text) and not re.fullmatch(r"[A-Z0-9 ./+%-]+", text)


class Static(html.parser.HTMLParser):
    """feste Texte außerhalb von script/style und translate="no" """
    VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack, self.found = [], {}

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        skip = tag in ("script", "style", "title", "head") or a.get("translate") == "no" or \
            bool(self.stack and self.stack[-1][1])
        for name in ("placeholder", "aria-label", "title"):
            if a.get(name) and not skip and translatable(a[name]):
                self.found.setdefault(a[name], self.getpos()[0])
        if tag not in self.VOID:
            self.stack.append((tag, skip))

    def handle_endtag(self, tag):
        while self.stack:
            if self.stack.pop()[0] == tag:
                break

    def handle_data(self, data):
        text = " ".join(data.split())
        if text and not (self.stack and self.stack[-1][1]) and translatable(text):
            self.found.setdefault(text, self.getpos()[0])


def js_unescape(s):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)


def page_texts(page):
    found = {}
    for m in CALL.finditer(page):
        found.setdefault(js_unescape(m.group(1)), "Skript, Zeile %d" % (page.count("\n", 0, m.start()) + 1))
    p = Static()
    p.feed(page)
    for t, line in p.found.items():
        found.setdefault(t, "Seite, Zeile %d" % line)
    found.setdefault("USB-Formatierer", "Titel")
    return found


def main():
    page = open(PAGE, encoding="utf-8").read()
    texts = page_texts(page)
    try:
        cat = json.load(open(CATALOG, encoding="utf-8"))
    except FileNotFoundError:
        cat = {}
    if "--missing" in sys.argv:
        json.dump({t: {lg: "" for lg in LANGS} for t in texts if t not in cat}, sys.stdout, ensure_ascii=False, indent=1)
        return 0
    bad = 0
    for t, where in texts.items():
        tr = cat.get(t)
        if not tr:
            print(f"FEHLT    {where}: {t[:80]!r}")
            bad += 1
            continue
        for lg in LANGS:
            if not tr.get(lg):
                print(f"FEHLT {lg} {where}: {t[:70]!r}")
                bad += 1
            elif sorted(VARS.findall(tr[lg])) != sorted(VARS.findall(t)):
                print(f"PLATZHALTER {lg} {where}: {t[:60]!r}")
                bad += 1
    for t in cat:
        if t not in texts:
            print(f"UNBENUTZT: {t[:80]!r}")
    if bad:
        print(f"{len(texts)} Texte, {bad} Probleme - Tabelle nicht eingesetzt")
        return 1
    table = {t: [cat[t][lg] for lg in LANGS] for t in sorted(texts)}
    code = "/*I18N*/" + json.dumps(table, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/") + \
        "/*I18N-END*/"
    if not TABLE.search(page):
        print("Marke /*I18N*/.../*I18N-END*/ fehlt in der Seite")
        return 1
    new = TABLE.sub(lambda m: code, page, count=1)
    if "--check" in sys.argv:
        if new != page:
            print("Tabelle in der Seite ist veraltet - python tools/web_i18n.py ausführen")
            return 1
        print(f"{len(texts)} Texte, Tabelle aktuell")
        return 0
    if new != page:
        open(PAGE, "w", encoding="utf-8", newline="\n").write(new)
    print(f"{len(texts)} Texte, Tabelle {'eingesetzt' if new != page else 'unverändert'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
