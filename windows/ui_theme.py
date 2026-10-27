"""Modernes Aussehen der PC-Programme - ein eigenes ttk-Theme, hell und dunkel, in den Farben der PS5-Weboberfläche.

Runde Ecken, Schalter, Kästchen und Karten sind Bilder, die hier zur Laufzeit erzeugt werden (PNG mit Alphakanal,
passend zur Bildschirmskalierung) - wie beim Windows-11-Theme "Sun Valley", nur ohne zusätzliches Paket. Alle
Bedienelemente bleiben gewöhnliche ttk-Widgets: Programmlogik und Tests merken vom Aussehen nichts.

    apply_theme(root, "auto" | "light" | "dark")   Theme setzen (auch zum Umschalten; danach Oberfläche neu aufbauen)
    colors()                                         Farben des aktiven Themes (für Canvas & Co.)

Stile: Card.TFrame (Karte), Page.TFrame (Hintergrund), Accent.TButton, Danger.TButton, Ghost.TButton,
Switch.TCheckbutton, Title/Section/Muted/Hint/Warn/Danger/Ok.TLabel, Pill.TLabel und Pill<Art>.TLabel
(Accent, Ok, Warn, Danger), Drive.TFrame/DriveSel.TFrame (Laufwerkskarten), Usage.Horizontal.TProgressbar."""
import base64
import math
import struct
import sys
import tkinter as tk
import tkinter.font as tkfont
import zlib
from tkinter import ttk

IS_WIN = sys.platform == "win32"

# Farben wie usb_format/web/index.html (--bg, --card, --text, --muted, --border, --accent, ...), für den PC etwas
# ruhiger abgestuft
PALETTES = {
    "light": {
        "page": "#d9e0ea", "card": "#ffffff", "card2": "#f3f5f9", "text": "#1f1e1c", "muted": "#45474d",
        "hint": "#62666f", "border": "#c3cbd8", "border2": "#9aa5b6", "accent": "#185fa5", "accent_hover": "#1d6dbd",
        "accent_press": "#144f8a", "on_accent": "#ffffff", "accent_bg": "#dbe7f4", "danger": "#a32d2d",
        "danger_hover": "#b83636", "danger_press": "#8a2525", "on_danger": "#ffffff", "danger_bg": "#fcebeb",
        "ok": "#3b6d11", "ok_bg": "#e6f0d8", "warn": "#854f0b", "warn_bg": "#faeeda", "field": "#ffffff",
        "hover": "#eaeef4", "press": "#dde3ec", "bar": "#e3e8ef", "disabled": "#9ca3af", "select": "#dbe7f4",
    },
    "dark": {
        "page": "#05080f", "card": "#1c2230", "card2": "#232b3b", "text": "#ecebe6", "muted": "#a6acb8",
        "hint": "#8d94a3", "border": "#2e3a50", "border2": "#4a5a76", "accent": "#85b7eb", "accent_hover": "#9cc6f0",
        "accent_press": "#6fa5de", "on_accent": "#0d1726", "accent_bg": "#0c2c4c", "danger": "#f09595",
        "danger_hover": "#f4aaaa", "danger_press": "#e57f7f", "on_danger": "#2a0d0d", "danger_bg": "#4a1818",
        "ok": "#97c459", "ok_bg": "#20330c", "warn": "#fac775", "warn_bg": "#412402", "field": "#141a26",
        "hover": "#2a3346", "press": "#323d54", "bar": "#2c3546", "disabled": "#5d6676", "select": "#0c2c4c",
    },
}

_state = {"mode": "light", "scale": 1.0, "images": {}, "fonts_done": False}


def colors():
    return PALETTES[_state["mode"]]


def mode():
    return _state["mode"]


# ------------------------------------------------------------------ Bilder

def _rgb(h):
    return int(h[1:3], 16), int(h[3:5], 16), int(h[5:7], 16)


def _png(w, h, pix):
    """RGBA-Pixel (Liste von Tupeln, zeilenweise) als PNG"""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pix[y * w + x])

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def _sd_rrect(px, py, x0, y0, x1, y1, r):
    """vorzeichenbehafteter Abstand zum Rand eines abgerundeten Rechtecks (innen negativ)"""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    qx, qy = abs(px - cx) - hx, abs(py - cy) - hy
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def _sd_seg(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy or 1)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def _cov(d):
    return max(0.0, min(1.0, 0.5 - d))


def _image(root, key, w, h, layers):
    """Bild aus Ebenen [(farbe, abdeckung(px, py) -> 0..1), ...] übereinander; bleibt im Speicher"""
    cache = _state["images"]
    if key in cache:
        return cache[key]
    pix = []
    for y in range(h):
        for x in range(w):
            px, py = x + 0.5, y + 0.5
            r = g = b = a = 0.0
            for col, f in layers:
                c = f(px, py)
                if c <= 0:
                    continue
                cr, cg, cb = _rgb(col)
                # "über"-Mischung mit vormultipliziertem Alpha
                r, g, b, a = cr * c + r * (1 - c), cg * c + g * (1 - c), cb * c + b * (1 - c), c + a * (1 - c)
            if a > 0:
                pix.append((round(r / a) if a < 1 else round(r), round(g / a) if a < 1 else round(g),
                            round(b / a) if a < 1 else round(b), round(a * 255)))
            else:
                pix.append((0, 0, 0, 0))
    img = tk.PhotoImage(master=root, data=base64.b64encode(_png(w, h, pix)))
    cache[key] = img
    return img


def _rrect_img(root, key, w, h, r, fill, stroke=None, sw=1.0, inset=0.0):
    """abgerundetes Rechteck mit Rand (Rand innen) - für 9-Teile-Bilder (border=r+...)"""
    layers = []
    if stroke:
        layers.append((stroke, lambda px, py: _cov(_sd_rrect(px, py, inset, inset, w - inset, h - inset, r))))
        if fill:
            layers.append((fill, lambda px, py: _cov(_sd_rrect(px, py, inset + sw, inset + sw, w - inset - sw,
                                                               h - inset - sw, max(0.5, r - sw)))))
    elif fill:
        layers.append((fill, lambda px, py: _cov(_sd_rrect(px, py, inset, inset, w - inset, h - inset, r))))
    return _image(root, key, w, h, layers)


# ------------------------------------------------------------------ Schriften

def _fonts(root):
    s = _state["scale"]
    fams = set(tkfont.families(root))
    if IS_WIN:
        family = "Segoe UI" if "Segoe UI" in fams else tkfont.nametofont("TkDefaultFont").actual("family")
    else:
        family = next((f for f in ("Inter", "Noto Sans", "Cantarell", "Ubuntu", "DejaVu Sans") if f in fams),
                      tkfont.nametofont("TkDefaultFont").actual("family"))
    base = 10
    for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont", "TkCaptionFont", "TkTooltipFont"):
        try:
            tkfont.nametofont(name).configure(family=family, size=base)
        except tk.TclError:
            pass
    specs = {"UfTitle": (15, "bold"), "UfH2": (12, "bold"), "UfBold": (base, "bold"), "UfSmall": (9, "normal"),
             "UfSection": (9, "bold"), "UfBig": (11, "bold")}
    keep = _state.setdefault("fonts", {})   # Verweise halten: sonst löscht Python die benannte Schrift wieder
    for name, (size, weight) in specs.items():
        if name in keep:
            keep[name].configure(family=family, size=size, weight=weight)
        else:
            keep[name] = tkfont.Font(root, name=name, family=family, size=size, weight=weight, exists=False)
    _state["fonts_done"] = True
    return s


# ------------------------------------------------------------------ Hell/Dunkel des Systems

def system_dark():
    """Nutzt das System gerade ein dunkles Design? (Windows: Apps-Modus; Linux: GNOME/GTK-Einstellung)"""
    if IS_WIN:
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as k:
                return winreg.QueryValueEx(k, "AppsUseLightTheme")[0] == 0
        except OSError:
            return False
    import subprocess
    for cmd in (["gsettings", "get", "org.gnome.desktop.interface", "color-scheme"],
                ["gsettings", "get", "org.gnome.desktop.interface", "gtk-theme"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=2).stdout.lower()
        except (OSError, subprocess.SubprocessError):
            continue
        if "dark" in out:
            return True
    return False


def grab_of(win, default=None):
    """Widget mit dem Tk-Fokusfang (grab) oder default. Ist die Auswahlliste eines Combobox aufgeklappt, hält deren
    interner Dropdown-Toplevel den Fang: Tk kennt ihn nicht als Widget und grab_current() wirft einen KeyError - dann
    ist ein Dialog offen (default True für Abfragen, None zum Zurückgeben)"""
    try:
        return win.grab_current()
    except (KeyError, tk.TclError):
        return default


def focus_of(win):
    """Widget mit dem Tastaturfokus oder None. Steht der Fokus in der aufgeklappten Liste eines Combobox, wirft
    focus_get() aus demselben Grund wie grab_current() einen KeyError ("popdown")"""
    try:
        return win.focus_get()
    except (KeyError, tk.TclError):
        return None


def report_error(win, title, text, exc_info=None, app_name=None, extra=""):
    """Fehlermeldung aus einem abgefangenen Programmfehler: darf selbst nie scheitern (sonst bleibt nur der Tk-Traceback
    und die Ursache geht unter). Mit app_name: der Fehler kommt in fehler.log, und der Dialog bietet "Bericht kopieren"
    (Version, System, letzte Fehler) zum Weitergeben an. Erst als Dialog, sonst auf die Fehlerausgabe."""
    import traceback
    if exc_info:
        traceback.print_exception(*exc_info)   # Ursache bleibt in der Fehlerausgabe, auch wenn der Dialog scheitert
    try:
        import pc_common
        import ui_msgbox
        from i18n import _
        if not app_name:
            ui_msgbox.showerror(title, text, parent=win)
            return
        pc_common.log_error(app_name, exc_info)
        msg = text + "\n\n" + _("Der Fehler steht in: {path}", path=pc_common.error_log_file(app_name))
        buttons = [(_("Bericht kopieren"), "copy", "TButton"), (_("OK"), "ok", "Accent.TButton")]
        if ui_msgbox._dialog(title, msg, "error", buttons, win, default=1, cancel="ok") == "copy":
            copy_report(win, app_name, extra)
    except Exception:   # z. B. Fenster schon zerstört oder Dialog nicht möglich
        traceback.print_exc()
        print(title + ": " + text, file=sys.stderr)


def copy_report(win, app_name, extra=""):
    """Bericht in die Zwischenablage (zum Einfügen in eine Nachricht) und kurz bestätigen"""
    import pc_common
    import ui_msgbox
    from i18n import _
    report = pc_common.error_report(app_name, extra)
    win.clipboard_clear()
    win.clipboard_append(report)
    win.update()
    ui_msgbox.showinfo(_("Bericht kopiert"), _("Der Bericht liegt in der Zwischenablage – bitte zusammen mit der Fehlermeldung weitergeben."),
                       parent=win)


def dark_titlebar(win):
    """Windows 10/11: Titelleiste passend zum Theme (dunkel bzw. hell)"""
    if not IS_WIN:
        return
    try:
        import ctypes
        win.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(win.winfo_id())
        val = ctypes.c_int(1 if _state["mode"] == "dark" else 0)
        for attr in (20, 19):   # DWMWA_USE_IMMERSIVE_DARK_MODE (ab 20H1), davor 19
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(val), ctypes.sizeof(val)) == 0:
                break
    except (OSError, AttributeError, tk.TclError):
        pass


# ------------------------------------------------------------------ Theme

def apply_theme(root, choice="auto"):
    """Theme setzen: choice "light", "dark" oder "auto" (wie das System). Liefert den tatsächlichen Modus."""
    m = choice if choice in ("light", "dark") else ("dark" if system_dark() else "light")
    if _state.get("root") is not root:   # neues Hauptfenster (neuer Tcl-Interpreter): Bilder und Schriften neu
        _state.update(root=root, images={}, fonts={})
    _state["mode"] = m
    _state["scale"] = s = max(1.0, root.winfo_fpixels("1i") / 96)
    _fonts(root)
    c = PALETTES[m]
    style = ttk.Style(root)
    name = f"usbfmt-{m}"
    if name not in style.theme_names():
        style.theme_create(name, parent="clam")
        style.theme_use(name)
        _build(root, style, c, s, m)
    else:
        style.theme_use(name)
    root.configure(bg=c["page"])
    for pat, val in (("*TCombobox*Listbox.background", c["card"]), ("*TCombobox*Listbox.foreground", c["text"]),
                     ("*TCombobox*Listbox.selectBackground", c["accent_bg"]),
                     ("*TCombobox*Listbox.selectForeground", c["text"]),
                     ("*TCombobox*Listbox.font", "TkDefaultFont"), ("*Toplevel.background", c["card"]),
                     ("*Menu.background", c["card"]), ("*Menu.foreground", c["text"])):
        root.option_add(pat, val)
    dark_titlebar(root)
    return m


def _build(root, style, c, s, m):
    px = lambda v: max(1, round(v * s))  # noqa: E731
    k = f"{m}-{s:.2f}-"   # Bildschlüssel je Modus und Skalierung

    def img(key, *a, **kw):
        return _rrect_img(root, k + key, *a, **kw)

    rc, rb = px(12), px(8)   # Ecken: Karten, Bedienelemente
    # Mitte des 9-Teile-Bildes: ttk kachelt sie über die ganze Fläche, bei 4 px Kantenlänge also tausendfach je Karte
    # (erstes Zeichnen des Fensters und jeder Neuaufbau dauerten ~8 s); mit 64/32 px sind es ~0,3 s. Die Mindestgröße
    # des Elements (width/height) bleibt die alte, sonst würden Knöpfe und Karten größer.
    card_sz, ctl_sz = 2 * rc + 64, 2 * rb + 32
    card_min, ctl_min = 2 * rc + 4, 2 * rb + 4

    # Grundwerte
    style.configure(".", background=c["card"], foreground=c["text"], font="TkDefaultFont",
                    bordercolor=c["border"], lightcolor=c["card"], darkcolor=c["card"], troughcolor=c["bar"],
                    selectbackground=c["accent_bg"], selectforeground=c["text"], fieldbackground=c["field"],
                    insertcolor=c["text"], focuscolor=c["accent"], arrowcolor=c["muted"])
    style.map(".", foreground=[("disabled", c["disabled"])])

    # Flächen
    style.configure("TFrame", background=c["card"])
    style.configure("Page.TFrame", background=c["page"])
    style.element_create("Uf.Card.bg", "image", img("card", card_sz, card_sz, rc, c["card"], c["border"], 1),
                         border=rc, padding=0, sticky="nsew", width=card_min, height=card_min)
    style.layout("Card.TFrame", [("Uf.Card.bg", {"sticky": "nsew", "children": [
        ("Frame.padding", {"sticky": "nsew"})]})])
    style.configure("Card.TFrame", padding=(px(16), px(14)))
    for key, fill, stroke, sw in (("Drive", c["card"], c["border"], 1), ("DriveHover", c["card"], c["border2"], 1),
                                  ("DriveSel", c["card"], c["accent"], px(2))):
        style.element_create(f"Uf.{key}.bg", "image", img(key, card_sz, card_sz, rb + px(2), fill, stroke, sw),
                             border=rb + px(3), padding=0, sticky="nsew", width=card_min, height=card_min)
        style.layout(f"{key}.TFrame", [(f"Uf.{key}.bg", {"sticky": "nsew", "children": [
            ("Frame.padding", {"sticky": "nsew"})]})])
        style.configure(f"{key}.TFrame", padding=(px(12), px(9)))

    # Texte
    style.configure("TLabel", background=c["card"], foreground=c["text"])
    style.configure("Page.TLabel", background=c["page"], foreground=c["muted"])
    style.configure("Title.TLabel", font="UfTitle")
    style.configure("H2.TLabel", font="UfH2")
    style.configure("Bold.TLabel", font="UfBold")
    style.configure("Section.TLabel", font="UfSection", foreground=c["muted"])
    style.configure("Muted.TLabel", foreground=c["muted"])
    style.configure("Hint.TLabel", foreground=c["hint"], font="UfSmall")
    style.configure("Warn.TLabel", foreground=c["warn"], font="UfSmall")
    style.configure("Danger.TLabel", foreground=c["danger"])
    style.configure("Ok.TLabel", foreground=c["ok"])
    style.configure("Link.TLabel", foreground=c["accent"])
    style.configure("PageLink.TLabel", background=c["page"], foreground=c["accent"])
    style.configure("TSeparator", background=c["border"])

    # Marken (Pillen)
    pill_h = px(20)
    for key, fill, fg, stroke in (("", c["card2"], c["muted"], c["border"]), ("Accent", c["accent_bg"], c["accent"], None),
                                  ("Ok", c["ok_bg"], c["ok"], None), ("Warn", c["warn_bg"], c["warn"], None),
                                  ("Danger", c["danger_bg"], c["danger"], None)):
        el = f"Uf.Pill{key}.bg"
        style.element_create(el, "image", img("pill" + key, pill_h + 2, pill_h, pill_h // 2, fill, stroke, 1),
                             border=(pill_h // 2, pill_h // 2), padding=0, sticky="nsew")
        style.layout(f"Pill{key}.TLabel", [(el, {"sticky": "nsew", "children": [
            ("Label.padding", {"sticky": "nsew", "children": [("Label.label", {"sticky": "nsew"})]})]})])
        style.configure(f"Pill{key}.TLabel", foreground=fg, font="UfSmall", padding=(px(9), px(1)))

    # Knöpfe
    def button(stylename, el, normal, hover, press, dis, fg, fg_dis, stroke=None, dis_stroke=None, focus=None):
        imgs = {n: img(f"{el}-{n}", ctl_sz, ctl_sz, rb, f, st, 1) for n, f, st in (
            ("n", normal, stroke), ("h", hover, stroke), ("p", press, stroke), ("d", dis, dis_stroke),
            ("f", normal, focus or stroke))}
        style.element_create(el, "image", imgs["n"], ("disabled", imgs["d"]), ("pressed", imgs["p"]),
                             ("active", imgs["h"]), ("focus", imgs["f"]), border=rb + 1, padding=0, sticky="nsew",
                             width=ctl_min, height=ctl_min)
        style.layout(stylename, [(el, {"sticky": "nsew", "children": [("Button.padding", {"sticky": "nsew", "children": [
            ("Button.label", {"sticky": "nsew"})]})]})])
        style.configure(stylename, foreground=fg, padding=(px(14), px(6)), anchor="center")
        style.map(stylename, foreground=[("disabled", fg_dis)])

    button("TButton", "Uf.Button.bg", c["card2"], c["hover"], c["press"], c["card"], c["text"], c["disabled"],
           stroke=c["border"], dis_stroke=c["border"], focus=c["accent"])
    button("Accent.TButton", "Uf.Accent.bg", c["accent"], c["accent_hover"], c["accent_press"], c["bar"],
           c["on_accent"], c["disabled"])
    style.configure("Accent.TButton", font="UfBold")
    button("Danger.TButton", "Uf.Danger.bg", c["danger"], c["danger_hover"], c["danger_press"], c["bar"],
           c["on_danger"], c["disabled"])
    style.configure("Danger.TButton", font="UfBold")
    button("Ghost.TButton", "Uf.Ghost.bg", c["card"], c["hover"], c["press"], c["card"], c["muted"], c["disabled"],
           focus=c["border2"])
    style.configure("Ghost.TButton", padding=(px(8), px(5)))
    button("Nav.TButton", "Uf.Nav.bg", c["card"], c["hover"], c["press"], c["card"], c["muted"], c["disabled"])
    style.configure("Nav.TButton", anchor="w", padding=(px(12), px(8)))
    button("NavSel.TButton", "Uf.NavSel.bg", c["accent_bg"], c["accent_bg"], c["accent_bg"], c["accent_bg"],
           c["accent"], c["disabled"])
    style.configure("NavSel.TButton", anchor="w", padding=(px(12), px(8)), font="UfBold")
    # Reiter (An der PS5): wie die Seitenleiste, aber mittig; "Formatieren" in Warnfarbe
    button("Tab.TButton", "Uf.Tab.bg", c["card"], c["hover"], c["press"], c["card"], c["muted"], c["disabled"])
    style.configure("Tab.TButton", anchor="center", padding=(px(16), px(8)))
    button("TabSel.TButton", "Uf.TabSel.bg", c["accent_bg"], c["accent_bg"], c["accent_bg"], c["accent_bg"],
           c["accent"], c["disabled"])
    style.configure("TabSel.TButton", anchor="center", padding=(px(16), px(8)), font="UfBold")
    button("TabDanger.TButton", "Uf.TabDanger.bg", c["danger_bg"], c["danger_bg"], c["danger_bg"], c["danger_bg"],
           c["danger"], c["disabled"])
    style.configure("TabDanger.TButton", anchor="center", padding=(px(16), px(8)), font="UfBold")
    # Auswahl-Kacheln ("Wofür soll der Datenträger dienen?"): wählbar wie Optionsfelder
    button("Chip.TButton", "Uf.Chip.bg", c["card2"], c["hover"], c["press"], c["card"], c["text"], c["disabled"],
           stroke=c["border"], dis_stroke=c["border"], focus=c["border2"])
    style.configure("Chip.TButton", padding=(px(10), px(7)))
    chip_sel = img("chipsel", ctl_sz, ctl_sz, rb, c["accent_bg"], c["accent"], px(2))
    chip_sel_d = img("chipseld", ctl_sz, ctl_sz, rb, c["card2"], c["border2"], px(2))
    style.element_create("Uf.ChipSel.bg", "image", chip_sel, ("disabled", chip_sel_d), border=rb + 1, padding=0,
                         sticky="nsew", width=ctl_min, height=ctl_min)
    style.layout("ChipSel.TButton", [("Uf.ChipSel.bg", {"sticky": "nsew", "children": [("Button.padding", {
        "sticky": "nsew", "children": [("Button.label", {"sticky": "nsew"})]})]})])
    style.configure("ChipSel.TButton", foreground=c["accent"], font="UfBold", padding=(px(10), px(7)), anchor="center")
    style.map("ChipSel.TButton", foreground=[("disabled", c["disabled"])])

    # Eingabefelder und Auswahllisten
    fld = {n: img(f"field-{n}", ctl_sz, ctl_sz, rb, f, st, sw) for n, f, st, sw in (
        ("n", c["field"], c["border"], 1), ("h", c["field"], c["border2"], 1), ("f", c["field"], c["accent"], px(2)),
        ("d", c["card2"], c["border"], 1))}
    style.element_create("Uf.Field.bg", "image", fld["n"], ("disabled", fld["d"]), ("focus", fld["f"]),
                         ("hover", fld["h"]), border=rb + 1, padding=0, sticky="nsew", width=ctl_min,
                         height=ctl_min)
    style.layout("TEntry", [("Uf.Field.bg", {"sticky": "nsew", "children": [("Entry.padding", {"sticky": "nsew", "children": [
        ("Entry.textarea", {"sticky": "nsew"})]})]})])
    style.configure("TEntry", padding=(px(10), px(6)), foreground=c["text"], insertcolor=c["text"])
    style.map("TEntry", foreground=[("disabled", c["disabled"])])
    style.configure("Placeholder.TEntry", foreground=c["hint"])

    aw = px(28)
    chev = _image(root, k + "chev", aw, px(18), [(c["muted"], lambda x, y: _cov(min(
        _sd_seg(x, y, aw / 2 - px(5) - px(4), px(7), aw / 2 - px(4), px(11.5)),
        _sd_seg(x, y, aw / 2 - px(4), px(11.5), aw / 2 + px(5) - px(4), px(7))) - px(1.4) / 2))])
    chev_d = _image(root, k + "chevd", aw, px(18), [(c["disabled"], lambda x, y: _cov(min(
        _sd_seg(x, y, aw / 2 - px(5) - px(4), px(7), aw / 2 - px(4), px(11.5)),
        _sd_seg(x, y, aw / 2 - px(4), px(11.5), aw / 2 + px(5) - px(4), px(7))) - px(1.4) / 2))])
    style.element_create("Uf.Combobox.arrow", "image", chev, ("disabled", chev_d), sticky="")
    style.layout("TCombobox", [("Uf.Field.bg", {"sticky": "nsew", "children": [
        ("Uf.Combobox.arrow", {"side": "right", "sticky": "ns"}),
        ("Combobox.padding", {"sticky": "nsew", "children": [("Combobox.textarea", {"sticky": "nsew"})]})]})])
    style.configure("TCombobox", padding=(px(10), px(6), 0, px(6)), foreground=c["text"], arrowsize=px(12))
    style.map("TCombobox", foreground=[("disabled", c["disabled"])],
              fieldbackground=[("readonly", c["field"])], selectbackground=[("readonly", c["field"])],
              selectforeground=[("readonly", c["text"])])

    style.configure("TSpinbox", fieldbackground=c["field"], background=c["card2"], foreground=c["text"],
                    bordercolor=c["border"], lightcolor=c["field"], darkcolor=c["field"], arrowcolor=c["muted"],
                    padding=(px(8), px(5)), insertcolor=c["text"])
    style.map("TSpinbox", bordercolor=[("focus", c["accent"])], foreground=[("disabled", c["disabled"])])

    # Kästchen, Schalter, Optionsfelder
    bs, gap = px(18), px(8)

    def check_img(key, on, dis):
        fill = (c["disabled"] if dis else c["accent"]) if on else c["field"]
        stroke = None if on else (c["border"] if dis else c["border2"])
        layers = [(stroke or fill, lambda x, y: _cov(_sd_rrect(x, y, 0, 0, bs, bs, px(4))))]
        if not on:
            layers.append((c["card2"] if dis else c["field"],
                           lambda x, y: _cov(_sd_rrect(x, y, 1.2, 1.2, bs - 1.2, bs - 1.2, px(3)))))
        if on:
            mark = c["card"] if dis else c["on_accent"]
            layers.append((mark, lambda x, y: _cov(min(_sd_seg(x, y, bs * .25, bs * .52, bs * .43, bs * .70),
                                                       _sd_seg(x, y, bs * .43, bs * .70, bs * .76, bs * .32))
                                                   - px(1.8) / 2)))
        return _image(root, k + key, bs + gap, bs, layers)

    ck = {n: check_img("chk" + n, "1" in n, "d" in n) for n in ("0", "1", "0d", "1d")}
    style.element_create("Uf.Check.indicator", "image", ck["0"], ("disabled", "selected", ck["1d"]),
                         ("disabled", ck["0d"]), ("selected", ck["1"]), sticky="w")
    style.layout("TCheckbutton", [("Checkbutton.padding", {"sticky": "nsew", "children": [
        ("Uf.Check.indicator", {"side": "left", "sticky": ""}),
        ("Checkbutton.label", {"side": "left", "sticky": "nsew"})]})])
    style.configure("TCheckbutton", padding=(0, px(3)))

    sw_w, sw_h = px(36), px(20)

    def switch_img(key, on, dis):
        track = (c["disabled"] if dis else c["accent"]) if on else c["field"]
        stroke = None if on else (c["border"] if dis else c["border2"])
        knob = (c["card"] if dis else c["on_accent"]) if on else (c["border"] if dis else c["muted"])
        kr = sw_h / 2 - px(4)
        kx = sw_w - sw_h / 2 if on else sw_h / 2
        layers = [(stroke or track, lambda x, y: _cov(_sd_rrect(x, y, 0, 0, sw_w, sw_h, sw_h / 2)))]
        if stroke:
            layers.append((track, lambda x, y: _cov(_sd_rrect(x, y, 1.2, 1.2, sw_w - 1.2, sw_h - 1.2, sw_h / 2 - 1.2))))
        layers.append((knob, lambda x, y: _cov(math.hypot(x - kx, y - sw_h / 2) - kr)))
        return _image(root, k + key, sw_w + gap, sw_h, layers)

    swi = {n: switch_img("sw" + n, "1" in n, "d" in n) for n in ("0", "1", "0d", "1d")}
    style.element_create("Uf.Switch.indicator", "image", swi["0"], ("disabled", "selected", swi["1d"]),
                         ("disabled", swi["0d"]), ("selected", swi["1"]), sticky="w")
    style.layout("Switch.TCheckbutton", [("Checkbutton.padding", {"sticky": "nsew", "children": [
        ("Uf.Switch.indicator", {"side": "left", "sticky": ""}),
        ("Checkbutton.label", {"side": "left", "sticky": "nsew"})]})])
    style.configure("Switch.TCheckbutton", padding=(0, px(4)))

    def radio_img(key, on, dis):
        ring = c["border"] if dis else (c["accent"] if on else c["border2"])
        layers = [(ring, lambda x, y: _cov(math.hypot(x - bs / 2, y - bs / 2) - bs / 2)),
                  (c["card2"] if dis else c["field"], lambda x, y: _cov(math.hypot(x - bs / 2, y - bs / 2) - bs / 2 + 1.5))]
        if on:
            layers.append((c["disabled"] if dis else c["accent"],
                           lambda x, y: _cov(math.hypot(x - bs / 2, y - bs / 2) - bs * .24)))
        return _image(root, k + key, bs + gap, bs, layers)

    ra = {n: radio_img("rad" + n, "1" in n, "d" in n) for n in ("0", "1", "0d", "1d")}
    style.element_create("Uf.Radio.indicator", "image", ra["0"], ("disabled", "selected", ra["1d"]),
                         ("disabled", ra["0d"]), ("selected", ra["1"]), sticky="w")
    style.layout("TRadiobutton", [("Radiobutton.padding", {"sticky": "nsew", "children": [
        ("Uf.Radio.indicator", {"side": "left", "sticky": ""}),
        ("Radiobutton.label", {"side": "left", "sticky": "nsew"})]})])
    style.configure("TRadiobutton", padding=(0, px(3)))

    # Fortschritt
    def bar_style(stylename, thick, fill):
        t = px(thick)
        tr = img(f"tr{thick}", t + 4, t, t / 2, c["bar"])
        pb = img(f"pb{thick}{fill}", t + 4, t, t / 2, fill)
        style.element_create(f"{stylename}.trough", "image", tr, border=(t // 2 + 1, 0), padding=0, sticky="ew")
        style.element_create(f"{stylename}.pbar", "image", pb, border=(t // 2 + 1, 0), padding=0, sticky="nsew")
        style.layout(f"{stylename}.Horizontal.TProgressbar", [(f"{stylename}.trough", {"sticky": "nsew", "children": [
            (f"{stylename}.pbar", {"side": "left", "sticky": "ns"})]})])
        style.configure(f"{stylename}.Horizontal.TProgressbar", thickness=t)

    bar_style("Uf", 8, c["accent"])
    style.layout("Horizontal.TProgressbar", style.layout("Uf.Horizontal.TProgressbar"))
    style.configure("Horizontal.TProgressbar", thickness=px(8))
    bar_style("Usage", 5, c["accent"])
    bar_style("UsageWarn", 5, c["warn"])

    # Bildlaufleiste: schmal, ohne Pfeile
    th = px(8)
    thumb = img("thumb", th, th * 3, th / 2, c["border2"])
    thumb_h = img("thumbh", th, th * 3, th / 2, c["muted"])
    style.element_create("Uf.Vthumb", "image", thumb, ("active", thumb_h), ("pressed", thumb_h),
                         border=(0, th // 2 + 1), padding=0, sticky="ns")
    style.layout("Vertical.TScrollbar", [("Vertical.Scrollbar.trough", {"sticky": "ns", "children": [
        ("Uf.Vthumb", {"expand": "1", "sticky": "nswe"})]})])
    style.configure("Vertical.TScrollbar", troughcolor=c["card"], background=c["card"], bordercolor=c["card"],
                    lightcolor=c["card"], darkcolor=c["card"], width=th, arrowsize=0)

    # Listen (Spiele, Partitionen)
    style.configure("Treeview", background=c["card"], fieldbackground=c["card"], foreground=c["text"],
                    bordercolor=c["card"], lightcolor=c["card"], darkcolor=c["card"], rowheight=px(28))
    style.map("Treeview", background=[("selected", c["accent_bg"])], foreground=[("selected", c["text"])])
    style.configure("Treeview.Heading", background=c["card2"], foreground=c["muted"], relief="flat",
                    bordercolor=c["border"], lightcolor=c["card2"], darkcolor=c["card2"], font="UfSection",
                    padding=(px(8), px(5)))
    style.map("Treeview.Heading", background=[("active", c["hover"])])
    style.layout("Treeview", [("Treeview.field", {"sticky": "nswe", "border": "0", "children": [
        ("Treeview.padding", {"sticky": "nswe", "children": [("Treeview.treearea", {"sticky": "nswe"})]})]})])


def icon(root, name, color="muted", size=16):
    """kleines Symbol (sun, moon, refresh, check, clock, help) in einer Theme-Farbe"""
    s = _state["scale"]
    n = max(8, round(size * s))
    col = colors().get(color, color)
    c = n / 2
    w = max(1.0, 1.6 * s)
    if name == "sun":
        rays = [(c + math.cos(a) * n * .32, c + math.sin(a) * n * .32, c + math.cos(a) * n * .46,
                 c + math.sin(a) * n * .46) for a in (i * math.pi / 4 for i in range(8))]
        f = lambda x, y: max(_cov(math.hypot(x - c, y - c) - n * .2),  # noqa: E731
                             max(_cov(_sd_seg(x, y, *r) - w / 2) for r in rays))
    elif name == "moon":
        f = lambda x, y: _cov(max(math.hypot(x - c, y - c) - n * .38,  # noqa: E731
                                  -(math.hypot(x - c - n * .2, y - c + n * .16) - n * .3)))
    elif name == "refresh":
        def f(x, y):
            d = abs(math.hypot(x - c, y - c) - n * .33)
            ang = math.atan2(y - c, x - c)
            arc = d - w / 2 if not (-0.9 < ang < -0.1) else 9
            tip = (c + n * .33 * math.cos(-0.1), c + n * .33 * math.sin(-0.1))
            head = min(_sd_seg(x, y, tip[0], tip[1], tip[0] - n * .02, tip[1] - n * .22),
                       _sd_seg(x, y, tip[0], tip[1], tip[0] + n * .2, tip[1] - n * .06))
            return max(_cov(arc), _cov(head - w / 2))
    elif name == "auto":   # Kreis, rechte Hälfte gefüllt: hell/dunkel wie das System
        def f(x, y):
            d = math.hypot(x - c, y - c)
            ring = _cov(abs(d - n * .38) - w / 2)
            return max(ring, _cov(d - n * .38) * _cov(c - x + .5))
    elif name == "clock":   # Kreis mit Zeigern (Verlauf)
        def f(x, y):
            ring = _cov(abs(math.hypot(x - c, y - c) - n * .38) - w / 2)
            hands = min(_sd_seg(x, y, c, c, c, n * .26), _sd_seg(x, y, c, c, c + n * .17, c + n * .1))
            return max(ring, _cov(hands - w / 2))
    elif name == "check":
        f = lambda x, y: _cov(min(_sd_seg(x, y, n * .2, n * .52, n * .42, n * .72),  # noqa: E731
                                  _sd_seg(x, y, n * .42, n * .72, n * .8, n * .3)) - w / 2)
    else:   # help: Kreis
        f = lambda x, y: _cov(abs(math.hypot(x - c, y - c) - n * .4) - w / 2)  # noqa: E731
    return _image(root, f"icon-{name}-{col}-{n}", n, n, [(col, f)])


def app_tile(root, size=30):
    """Programmsymbol für die Kopfzeile: abgerundete Kachel in der Akzentfarbe mit USB-Stecker"""
    c = colors()
    n = max(16, round(size * _state["scale"]))
    w = max(1.0, 1.7 * _state["scale"])
    layers = [(c["accent"], lambda x, y: _cov(_sd_rrect(x, y, 0, 0, n, n, n * .27))),
              (c["on_accent"], lambda x, y: _cov(abs(_sd_rrect(x, y, n * .36, n * .2, n * .64, n * .5, n * .03))
                                                 - w / 2)),
              (c["on_accent"], lambda x, y: _cov(_sd_rrect(x, y, n * .29, n * .5, n * .71, n * .8, n * .08))),
              (c["accent"], lambda x, y: max(_cov(_sd_rrect(x, y, n * .42, n * .29, n * .47, n * .37, 0.5)),
                                             _cov(_sd_rrect(x, y, n * .53, n * .29, n * .58, n * .37, 0.5))))]
    return _image(root, f"tile-{_state['mode']}-{n}", n, n, layers)


def list_checks(root):
    """Kästchen leer/angehakt für Listen (Spiele) in den Farben des Themes - (leer, angehakt)"""
    c, bs = colors(), max(12, round(18 * _state["scale"]))
    k = f"lchk-{_state['mode']}-{bs}-"
    off = _image(root, k + "0", bs, bs, [(c["border2"], lambda x, y: _cov(_sd_rrect(x, y, 0, 0, bs, bs, bs * .22))),
                                         (c["field"], lambda x, y: _cov(_sd_rrect(x, y, 1.3, 1.3, bs - 1.3, bs - 1.3,
                                                                                  bs * .18)))])
    w = max(1.4, 1.8 * _state["scale"])
    on = _image(root, k + "1", bs, bs, [(c["accent"], lambda x, y: _cov(_sd_rrect(x, y, 0, 0, bs, bs, bs * .22))),
                                        (c["on_accent"], lambda x, y: _cov(min(
                                            _sd_seg(x, y, bs * .25, bs * .52, bs * .43, bs * .70),
                                            _sd_seg(x, y, bs * .43, bs * .70, bs * .76, bs * .32)) - w / 2))])
    return off, on


def px(v):
    """Länge in Pixeln für die aktuelle Bildschirmskalierung (Entwurf bei 96 dpi)"""
    return max(1, round(v * _state["scale"]))


def pad(v):
    """Abstand bzw. Abstands-Tupel skalieren"""
    return tuple(px(x) for x in v) if isinstance(v, (tuple, list)) else px(v)


def card(parent, style="Card.TFrame", padding=(16, 14), **kw):
    """Karte (abgerundet, mit Rand); padding wird skaliert - ttk nimmt das Polster nur vom Widget, nicht vom Stil"""
    return ttk.Frame(parent, style=style, padding=pad(padding), **kw)
