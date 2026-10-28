"""Warteschlange der Windows-App gegen einen laufenden Testserver (Host-Fassung): zwei Kopieraufträge einstellen, nacheinander ablaufen
lassen, am Ziel liegen beide Spiele. Der Testserver braucht auf da0 die Ordner-Spiele "Beta" und "Alpha" (je mit sce_sys/param.json) und
da1 als leeres Ziel (test/queue_server.sh in usb_format startet ihn). Ohne Argument oder ohne Anzeige wird übersprungen.
Aufruf: python tests/gui_queue_test.py 127.0.0.1:18129"""
import os
import sys
import tempfile
import time
import tkinter as tk

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
if len(sys.argv) < 2:
    print("ÜBERSPRUNGEN (kein Testserver angegeben)")
    sys.exit(0)
W = tempfile.mkdtemp(prefix="gui-queue-")
os.environ["XDG_CONFIG_HOME"] = os.environ["APPDATA"] = W
os.environ["USBFMT_NO_UPDATE_CHECK"] = os.environ["USBFMT_NO_DISCOVERY"] = "1"
try:
    tk.Tk().destroy()
except tk.TclError as e:
    print("ÜBERSPRUNGEN (keine Anzeige): %s" % e)
    sys.exit(0)

import usb_formatierer as uf  # noqa: E402

host, _sep, port = sys.argv[1].partition(":")
uf.API_PORT = int(port or uf.API_PORT)
fails = 0


def check(cond, text, why=""):
    global fails
    print(("OK      " if cond else "FEHLER  ") + text + ((" - " + str(why)) if (why and not cond) else ""))
    fails += 0 if cond else 1


shown = []
for n in ("showinfo", "showwarning", "showerror"):
    setattr(uf.messagebox, n, lambda title, msg="", _n=n, **kw: shown.append((_n, title, msg)))
uf.messagebox.askokcancel = lambda *a, **k: True

app = uf.App()


def pump(sec):
    end = time.time() + sec
    while time.time() < end:
        app.update()
        time.sleep(0.05)


try:
    pump(3)
    app.host_var.set(host)
    api = app._use_host(host)
    app.q.put(("connected", api, api.devices(timeout=10)))
    pump(12)
    i = next(i for i, d in enumerate(app.devices) if d["name"] == "da0")
    app.dev_combo.current(i)
    app._update()
    for _ in range(80):   # Spiele-Liste der PS5 abwarten
        pump(0.5)
        if app.games.get("da0"):
            break
    app._show_games()
    target = next(d for d in app.devices if d["name"] == "da1")
    uf.ask_move = lambda *a, **k: (target, True, False, True)   # "In die Warteschlange" gewählt
    for nm in ("Alpha", "Beta"):
        app.games_checked.clear()
        app.games_checked.update(g["key"] for g in app.games["da0"] if g["name"] == nm)
        app._move_games(copy_only=True)
        pump(0.5)
    check(len(app.copy_queue) == 2, "zwei Kopieraufträge eingestellt", app.copy_queue)
    check(app.queue_row.winfo_ismapped(), "Warteschlangen-Zeile sichtbar")
    app._queue_start()
    for _ in range(240):
        pump(0.5)
        if not app.queue_running and not app.copy_queue and not app.busy:
            break
    check(not app.queue_running and not app.copy_queue, "Warteschlange abgearbeitet")
    lib = api.library()
    for _ in range(40):   # Suche nach dem Kopieren abwarten
        if lib and not lib.get("scanning"):
            break
        time.sleep(0.5)
        lib = api.library()
    on_da1 = sorted(t["name"] for t in lib["titles"] if t["device"] == "da1")
    check(on_da1 == ["Alpha", "Beta"], "beide Spiele liegen am Ziel (da1)", on_da1)
    check(any("abgearbeitet" in t or "finished" in t.lower() for _n, t, _m in shown), "ein Bericht am Ende", shown[-2:])
    check(not app.queue_row.winfo_ismapped(), "Warteschlangen-Zeile danach ausgeblendet")
finally:
    try:
        for _id in app.tk.call("after", "info"):
            try:
                app.after_cancel(_id)
            except Exception:
                pass
        app.destroy()
    except tk.TclError:
        pass
print("Alle Warteschlangen-Tests bestanden." if not fails else "%d Fehler" % fails)
sys.exit(1 if fails else 0)
