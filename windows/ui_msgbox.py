"""Meldungen und Rückfragen im Theme der Programme (hell/dunkel) - Ersatz für tkinter.messagebox mit denselben
Aufrufen: showinfo, showwarning, showerror, askyesno, askokcancel (title, message, parent=..., icon=...).
Die Windows-Standarddialoge wären im dunklen Modus grell weiß und zeigen Knöpfe in der Systemsprache."""
import tkinter as tk
from tkinter import ttk

import ui_theme
from i18n import _

_KIND = {"info": ("accent", "i"), "warning": ("warn", "!"), "error": ("danger", "!"), "question": ("accent", "?")}


def _dialog(title, message, kind, buttons, parent=None, default=0, cancel=None):
    """buttons: [(text, wert, stil)]; liefert den Wert des gedrückten Knopfs (Esc/Schließen: cancel)"""
    root = parent.winfo_toplevel() if parent is not None else tk._default_root
    win = tk.Toplevel(root)
    win.withdraw()
    win.title(title)
    win.transient(root)
    win.resizable(False, False)
    win.configure(bg=ui_theme.colors()["card"])
    frm = ttk.Frame(win, padding=ui_theme.pad((22, 18, 22, 16)))
    frm.pack(fill="both", expand=True)
    color, mark = _KIND.get(kind, _KIND["info"])
    c = ui_theme.colors()
    n = ui_theme.px(30)
    badge = tk.Canvas(frm, width=n, height=n, highlightthickness=0, bg=c["card"])
    badge.create_oval(1, 1, n - 1, n - 1, fill=c[color + "_bg"] if color != "accent" else c["accent_bg"],
                      outline=c[color])
    badge.create_text(n / 2, n / 2, text=mark, fill=c[color], font="UfBig")
    badge.grid(row=0, column=0, sticky="n", padx=(0, ui_theme.px(14)))
    ttk.Label(frm, text=title, style="H2.TLabel").grid(row=0, column=1, sticky="w")
    ttk.Label(frm, text=message, wraplength=ui_theme.px(440), justify="left").grid(
        row=1, column=1, sticky="w", pady=(ui_theme.px(6), 0))
    result = {"v": cancel}

    def done(v):
        result["v"] = v
        win.destroy()

    btns = ttk.Frame(frm)
    btns.grid(row=2, column=0, columnspan=2, sticky="e", pady=(ui_theme.px(18), 0))
    widgets = []
    for text, value, style in buttons:
        b = ttk.Button(btns, text=text, style=style, command=lambda v=value: done(v))
        b.pack(side="left", padx=(ui_theme.px(8), 0))
        widgets.append(b)
    win.bind("<Escape>", lambda e: done(cancel))

    def enter(e):   # Enter löst den Knopf mit dem Fokus aus (mit Tab gewählt), sonst den vorgewählten
        w = e.widget if e.widget in widgets else ui_theme.focus_of(win)
        if w in widgets:
            w.invoke()
        else:
            done(buttons[default][1])
    win.bind("<Return>", enter)
    win.protocol("WM_DELETE_WINDOW", lambda: done(cancel))
    win.update_idletasks()
    if root is not None and root.winfo_viewable():
        x = root.winfo_rootx() + (root.winfo_width() - win.winfo_reqwidth()) // 2
        y = root.winfo_rooty() + (root.winfo_height() - win.winfo_reqheight()) // 3
        win.geometry(f"+{max(0, x)}+{max(0, y)}")
    win.deiconify()
    ui_theme.dark_titlebar(win)
    widgets[default].focus_set()
    old = ui_theme.grab_of(win)   # z. B. ein modaler Dialog, der diese Meldung zeigt - danach wieder modal machen
    try:
        win.wait_visibility()   # Linux: grab_set scheitert sonst, solange die Maustaste noch gedrückt ist
        win.grab_set()
    except tk.TclError:
        pass
    win.wait_window()
    if old is not None:
        try:
            if old.winfo_exists():
                old.grab_set()
                old.focus_set()
        except tk.TclError:
            pass
    return result["v"]


def showinfo(title, message, parent=None, **kw):
    return _dialog(title, message, "info", [(_("OK"), "ok", "Accent.TButton")], parent, cancel="ok")


def showwarning(title, message, parent=None, **kw):
    return _dialog(title, message, "warning", [(_("OK"), "ok", "Accent.TButton")], parent, cancel="ok")


def showerror(title, message, parent=None, **kw):
    return _dialog(title, message, "error", [(_("OK"), "ok", "Accent.TButton")], parent, cancel="ok")


def _second(icon, default):
    """vorgewählt "Nein"/"Abbrechen": ausdrücklich (default="no"/"cancel" wie bei tkinter.messagebox) oder bei
    Warnungen - Enter allein soll nichts Unumkehrbares bestätigen"""
    return 1 if default in ("no", "cancel") or (default is None and icon == "warning") else 0


def askyesno(title, message, parent=None, icon="question", default=None, **kw):
    return _dialog(title, message, icon, [(_("Ja"), True, "Accent.TButton"), (_("Nein"), False, "TButton")],
                   parent, default=_second(icon, default), cancel=False)


def askokcancel(title, message, parent=None, icon="question", default=None, **kw):
    return _dialog(title, message, icon, [(_("OK"), True, "Accent.TButton"), (_("Abbrechen"), False, "TButton")],
                   parent, default=_second(icon, default), cancel=False)
