"""Oberfläche bleibt bedienbar, wenn die Auswahlliste eines Combobox aufgeklappt ist (Tk meldet dafür beim Abfragen von
Fokusfang und Fokus einen KeyError "popdown"): Taktgeber des Programms, Tastenkürzel, Meldungsfenster und der
Fehlerdialog dürfen dann nicht scheitern. Ohne Anzeige wird übersprungen.
Aufruf: python tests/gui_robust_test.py"""
import os
import sys
import tempfile
import tkinter as tk
from tkinter import ttk

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
W = tempfile.mkdtemp(prefix="gui-robust-")
os.environ["XDG_CONFIG_HOME"] = os.environ["APPDATA"] = W
os.environ["USBFMT_NO_UPDATE_CHECK"] = "1"

fails = 0


def ok(text):
    print("OK      " + text)


def bad(text, why=""):
    global fails
    fails += 1
    print("FEHLER  " + text + (" - " + str(why) if why else ""))


try:
    root = tk.Tk()
except tk.TclError as e:
    print("ÜBERSPRUNGEN (keine Anzeige): %s" % e)
    sys.exit(0)
root.withdraw()

import ui_msgbox  # noqa: E402
import ui_theme  # noqa: E402

# 1. Helfer direkt: Dialog mit Fangs und aufgeklappter Liste
dlg = tk.Toplevel(root)
box = ttk.Combobox(dlg, values=["a", "b"], state="readonly")
box.pack()
dlg.update()
dlg.grab_set()
root.update()
root.tk.call("ttk::combobox::Post", str(box))
root.update()
try:
    root.grab_current()
    print("HINWEIS  Tk wirft hier keinen KeyError (andere Tk-Fassung) - der Test prüft nur noch die Helfer")
except KeyError:
    pass
try:
    g = ui_theme.grab_of(root, True)
    f = ui_theme.focus_of(root)
    ok("grab_of/focus_of mit aufgeklappter Liste: kein Fehler (grab=%r, fokus=%r)" % (bool(g), f))
    if not g:
        bad("grab_of meldet bei aufgeklappter Liste keinen offenen Dialog")
except Exception as e:
    bad("Helfer werfen bei aufgeklappter Liste", repr(e))

# 2. Meldungsfenster bei aufgeklappter Liste (Enter über focus_of)
state = {"done": False}


def press_enter():
    state["tries"] = state.get("tries", 0) + 1
    tops = [w for w in root.winfo_children() + dlg.winfo_children() if isinstance(w, tk.Toplevel) and w is not dlg]
    if state["tries"] > 40:   # nie hängen bleiben: Meldungsfenster nicht gefunden
        for w in tops:
            w.destroy()
        return
    if tops:
        def buttons(w):
            for c in w.winfo_children():
                if isinstance(c, ttk.Button):
                    yield c
                yield from buttons(c)
        btn = next(buttons(tops[-1]), None)   # OK-Knopf direkt auslösen (ein Tastendruck ohne Fokus kommt nicht an)
        if btn is not None:
            btn.invoke()
            state["done"] = True
        else:
            tops[-1].destroy()
    else:
        root.after(100, press_enter)


root.after(300, press_enter)
try:
    ui_msgbox.showerror("Test", "Meldung bei aufgeklappter Liste", parent=dlg)
    ok("Meldungsfenster bei aufgeklappter Liste: öffnet und schließt")
except Exception as e:
    bad("Meldungsfenster scheitert bei aufgeklappter Liste", repr(e))
if not state["done"]:
    bad("Meldungsfenster wurde nicht bedient")
dlg.destroy()

# 3. Fehlerdialog darf selbst nie werfen
gone = tk.Toplevel(root)
gone.destroy()   # Fenster schon weg: der Dialog kann nicht erscheinen, report_error muss das abfangen
try:
    ui_theme.report_error(gone, "Test", "Fehler ohne Fenster")
    ok("report_error ohne gültiges Fenster: kein zweiter Fehler")
except Exception as e:
    bad("report_error wirft selbst", repr(e))

# 3b. Fehlerbericht: Fehler kommt in fehler.log, der Dialog bietet "Bericht kopieren", der Bericht enthält Version und Fehler
import pc_common  # noqa: E402

probe = tk.Toplevel(root)
box2 = ttk.Combobox(probe, values=["a"], state="readonly")
box2.pack()
probe.update()


def press_copy():
    tops = [w for w in root.winfo_children() + probe.winfo_children() if isinstance(w, tk.Toplevel)]
    btns = []

    def walk(w):
        for c in w.winfo_children():
            if isinstance(c, ttk.Button):
                btns.append(c)
            walk(c)
    for t in tops:
        walk(t)
    copy = [b for b in btns if str(b.cget("text")) == uf_text("Bericht kopieren")]
    ok_btn = [b for b in btns if str(b.cget("text")) == uf_text("OK")]
    state["copy_seen"] = state.get("copy_seen", 0) + 1
    if state["copy_seen"] > 60:
        for t in tops:
            t.destroy()
        return
    if copy and "copied" not in state:
        state["copied"] = True
        root.after(150, press_copy)   # danach die Bestätigung schließen (modal: Aufruf blockiert bis dahin)
        copy[0].invoke()
    elif "copied" in state and ok_btn:
        ok_btn[0].invoke()
    else:
        root.after(100, press_copy)


from i18n import _ as uf_text  # noqa: E402

root.after(300, press_copy)
try:
    try:
        raise ValueError("Testfehler fuer den Bericht")
    except ValueError:
        info = sys.exc_info()
    ui_theme.report_error(probe, "Test", "Unerwarteter Fehler: Test", info, "GuiRobustTest", "Betriebsart: test")
    log = open(pc_common.error_log_file("GuiRobustTest"), encoding="utf-8").read()
    clip = root.clipboard_get()
    check_ok = "Testfehler fuer den Bericht" in log and "Testfehler fuer den Bericht" in clip and "GuiRobustTest" in clip and "Betriebsart: test" in clip
    ok("Fehlerbericht: fehler.log und Zwischenablage enthalten Version, Angaben und Fehler") if check_ok else bad("Fehlerbericht", clip[:200])
except Exception as e:
    bad("Fehlerbericht wirft", repr(e))
probe.destroy()

# 4. Programm: Taktgeber und Tastenkürzel bei aufgeklappter Liste
import usb_formatierer as uf  # noqa: E402
import i18n  # noqa: E402

if os.environ.get("GUI_TEST_LANG"):   # z. B. en: wie auf dem CI-Rechner prüfen
    i18n.set_lang(os.environ["GUI_TEST_LANG"])

root.destroy()
app = uf.App()
app.update()
top = tk.Toplevel(app)
box = ttk.Combobox(top, values=["a", "b"], state="readonly")
box.pack()
top.update()
top.grab_set()
app.tk.call("ttk::combobox::Post", str(box))
app.update()
try:
    app._follow_system()
    app.after_cancel(app._sys_id)
    ok("Designprüfung (Taktgeber) bei aufgeklappter Liste: kein Fehler")
except Exception as e:
    bad("Taktgeber scheitert", repr(e))
try:
    r = app._key_ok()
    ok("Tastenkürzel-Prüfung bei aufgeklappter Liste: kein Fehler (%s)" % ("gesperrt" if not r else "frei"))
except Exception as e:
    bad("Tastenkürzel-Prüfung scheitert", repr(e))
top.destroy()
app.update()

# 5. Kopierdialog (Knöpfe über den übersetzten Text suchen: auf dem CI-Rechner läuft die Oberfläche englisch): Vorabprüfung meldet "passt nicht" - Hinweiszeile erscheint, "Kopieren" zeigt die Meldung und bleibt offen
dev = {"name": "da0", "model": "Quelle", "size": 1 << 30, "mounts": [{"free": 1 << 20}]}
dst = {"name": "da1", "model": "Ziel", "size": 1 << 30, "mounts": [{"free": 1 << 20}]}
seen = {}


def walk(w):
    for c in w.winfo_children():
        yield c
        yield from walk(c)


def drive(step=0):
    tops = [w for w in app.winfo_children() if isinstance(w, tk.Toplevel)]
    if step > 60:   # nie hängen bleiben
        for w in tops:
            w.destroy()
        return
    if not tops:
        app.after(100, drive, step + 1)
        return
    win = tops[0]
    kids = list(walk(win))
    texts = {str(c.cget("text")): c for c in kids if isinstance(c, (ttk.Label, ttk.Button))}
    inner = [w for w in kids if isinstance(w, tk.Toplevel)]
    if inner:   # Meldungsfenster der Vorabprüfung: bestätigen
        btn = next((c for c in walk(inner[-1]) if isinstance(c, ttk.Button)), None)
        seen["msg"] = True
        app.after(300, drive, step + 1)   # vor dem Klick planen: ein modaler Dialog blockiert den Aufruf bis zum Schließen
        (btn.invoke() if btn else inner[-1].destroy())
        return
    if "note" not in seen:
        if any("Zu wenig Platz" in t for t in texts):
            seen["note"] = True
        else:
            app.after(100, drive, step + 1)
            return
    if "msg" not in seen:
        app.after(500, drive, step + 1)   # "Kopieren" zeigt eine modale Meldung: der nächste Schritt muss schon geplant sein
        texts[uf._("Kopieren")].invoke()
        return
    seen["still_open"] = win.winfo_exists()
    texts[uf._("Abbrechen")].invoke()


app.after(300, drive)
app.after(40000, lambda: [w.destroy() for w in app.winfo_children() if isinstance(w, tk.Toplevel)])   # Wächter
try:
    r = uf.ask_move(app, dev, [dst], ["Spiel"], check=lambda t, ex: ("Zu wenig Platz auf dem Ziel", ""))
    if seen.get("note") and seen.get("msg") and seen.get("still_open") and r is None:
        ok("Kopierdialog: Hinweiszeile, Meldung beim Kopieren, Dialog bleibt offen, Abbrechen liefert nichts")
    else:
        bad("Kopierdialog", "%r %r" % (seen, r))
except Exception as e:
    bad("Kopierdialog wirft", repr(e))
app.destroy()

print("Alle Oberflächen-Robustheitstests bestanden." if not fails else "%d Fehler" % fails)
sys.exit(1 if fails else 0)
