"""Layout der Windows-App in allen fünf Sprachen, in allen Betriebsarten und Reitern, bei der Standardgröße und der kleinsten
Fenstergröße: kein Bedienelement ragt über den Fensterrand, und Knöpfe/Beschriftungen sind nicht abgeschnitten (angeforderte Breite
passt in die tatsächliche). Je Sprache läuft ein eigener Prozess (mehrere Tk-Fenster nacheinander gehen im selben Prozess nicht:
die benannten Schriften sind dann weg). Ohne Anzeige wird übersprungen.
Aufruf: python tests/gui_layout_test.py [sprache]"""
import sys as _sys
for _s in (_sys.stdout, _sys.stderr):   # CI-Rechner geben oft in cp1252 aus: Umlaute und Ersatzzeichen dürfen nie zum Absturz führen
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
import os
import subprocess
import sys
import tempfile
import time
import tkinter as tk
from tkinter import ttk

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANGS = ("de", "en", "it", "es", "fr")

if len(sys.argv) == 1:   # Hauptlauf: je Sprache ein Prozess
    rc = 0
    for code in LANGS:
        r = subprocess.run([sys.executable, "-u", os.path.abspath(__file__), code], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        sys.stdout.write(r.stdout)
        if r.returncode == 3:
            print("ÜBERSPRUNGEN (keine Anzeige)")
            sys.exit(0)
        if r.returncode:
            rc = 1
            sys.stderr.write(r.stderr[-1500:])
    print("Alle Layout-Tests bestanden." if not rc else "Layout-Tests fehlgeschlagen")
    sys.exit(rc)

CODE = sys.argv[1]
sys.path.insert(0, HERE)
W = tempfile.mkdtemp(prefix="gui-layout-")
os.environ["XDG_CONFIG_HOME"] = os.environ["APPDATA"] = W
os.environ["USBFMT_NO_UPDATE_CHECK"] = "1"
os.environ["USBFMT_NO_DISCOVERY"] = "1"   # nie die echte PS5 im Netz suchen

try:
    probe = tk.Tk()
    probe.destroy()
except tk.TclError as e:
    print("keine Anzeige: %s" % e)
    sys.exit(3)

import i18n  # noqa: E402
import usb_formatierer as uf  # noqa: E402

problems = []
scanned = [0]
CHECKED = (ttk.Button, ttk.Label, ttk.Checkbutton, ttk.Radiobutton, tk.Button, tk.Label)


def widgets(w):
    for c in w.winfo_children():
        yield c
        yield from widgets(c)


def pump(app, sec=0.4):
    end = time.time() + sec
    while time.time() < end:
        app.update()
        time.sleep(0.02)


def close_app(app):
    """Fenster schließen: offene Taktgeber vorher abmelden, sonst meldet Tcl beim Zerstören Fehler"""
    try:
        for _id in app.tk.call("after", "info"):
            try:
                app.after_cancel(_id)
            except Exception:
                pass
        app.destroy()
    except tk.TclError:
        pass


def scan(app, where):
    top_right = app.winfo_rootx() + app.winfo_width()
    top_bottom = app.winfo_rooty() + app.winfo_height()
    for w in widgets(app):
        try:
            if not w.winfo_ismapped() or not w.winfo_viewable():
                continue
            x2, y2 = w.winfo_rootx() + w.winfo_width(), w.winfo_rooty() + w.winfo_height()
            scanned[0] += 1
            text = str(w.cget("text")) if "text" in w.keys() else ""
            name = (text or w.winfo_class()).replace("\n", " ")[:40]
            if isinstance(w, CHECKED) and text and w.winfo_width() > 1 and w.winfo_width() < w.winfo_reqwidth() - 3:
                problems.append("%s: abgeschnitten (%d < %d px): %s" % (where, w.winfo_width(), w.winfo_reqwidth(), name))
            elif w.winfo_width() > 4 and x2 > top_right + 2:
                problems.append("%s: ragt über den rechten Rand: %s" % (where, name))
            elif w.winfo_height() > 4 and y2 > top_bottom + 2 and not isinstance(w, tk.Toplevel):
                problems.append("%s: ragt über den unteren Rand: %s" % (where, name))
        except tk.TclError:
            continue


uf.save_settings({"lang": CODE})   # die App liest ihre Sprache beim Start aus den Einstellungen (set_lang allein würde überschrieben)
app = uf.App()
assert i18n.lang() == CODE, "Sprache nicht übernommen: %s" % i18n.lang()
try:
    app.update()
    if app.winfo_screenwidth() < app.winfo_reqwidth() + 40 or app.winfo_screenheight() < app.winfo_reqheight() + 60:
        # Bildschirm kleiner als das Fenster (z. B. CI-Rechner mit 1024 x 768): das Fenster wird dann am Bildschirm abgeschnitten, nicht vom Layout
        print("ÜBERSPRUNGEN (%s): Bildschirm %dx%d ist kleiner als das Fenster %dx%d" % (CODE, app.winfo_screenwidth(), app.winfo_screenheight(),
                                                                                   app.winfo_reqwidth(), app.winfo_reqheight()))
        sys.exit(0)
    if CODE == LANGS[0]:   # Gegenprobe: ein zu breiter und ein zu schmaler Knopf müssen auffallen
        pump(app)
        wide = ttk.Button(app, text="x" * 400)
        wide.place(x=0, y=0)
        narrow = ttk.Button(app, text="ein ziemlich langer Text", width=3)
        narrow.place(x=0, y=40, width=20)
        pump(app)
        scan(app, "Gegenprobe")
        found = [p for p in problems if p.startswith("Gegenprobe")]
        problems.clear()
        wide.destroy()
        narrow.destroy()
        if len(found) < 2:
            print("FEHLER  Gegenprobe: der Test erkennt zu breite/abgeschnittene Knöpfe nicht (%r)" % found)
            sys.exit(1)
        print("OK      Gegenprobe: zu breite und abgeschnittene Knöpfe werden erkannt")
        scanned[0] = 0
    for size in ("default", "min"):
        if size == "min":
            mw, mh = app.minsize()
            app.geometry("%dx%d" % (max(mw, 1), max(mh, 1)))
        pump(app)
        for mode in ("ps5", "pc", "std"):
            app._nav(mode)
            pump(app, 0.3)
            if mode == "ps5":
                for tab in ("games", "pkg", "saves", "images", "format"):
                    app._set_tab(tab, False)
                    pump(app, 0.3)
                    scan(app, "%s/%s/%s" % (CODE, size, tab))
            else:
                scan(app, "%s/%s/%s" % (CODE, size, mode))
finally:
    close_app(app)

problems = sorted(set(problems))
if problems:
    for p in problems[:40]:
        print("FEHLER  " + p)
    print("%d Layout-Probleme (%s)" % (len(problems), CODE))
    sys.exit(1)
print("OK      Layout (%s): %d Bedienelemente in 3 Betriebsarten, allen Reitern, Standard- und Mindestgröße: nichts abgeschnitten" % (CODE, scanned[0]))
