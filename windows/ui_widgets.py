"""Bausteine der modernen Oberfläche: Laufwerke als Karten (DriveList), Auswahl-Kacheln (ChoiceChips) und die
Kopfzeile der Programme (app_header). Alles gewöhnliche ttk-Widgets im Theme von ui_theme."""
from tkinter import ttk

import ui_theme
from i18n import _


def _bind_all(widget, seq, fn):
    widget.bind(seq, fn, add="+")
    for child in widget.winfo_children():
        _bind_all(child, seq, fn)


class DriveList(ttk.Frame):
    """Laufwerke als Karten wie in der Weboberfläche - Auswahl per Klick oder Pfeiltasten. Nach außen wie eine
    ttk.Combobox, soweit Programm und Tests sie brauchen: current(), cget("values"), state(), <<ComboboxSelected>>.
    set_items(items): je Laufwerk dict(text, title, size, sub, used (0..1 oder None), pills [(text, art)]) -
    art: "", "Accent", "Ok", "Warn", "Danger"."""

    def __init__(self, master, empty_text="", **kw):
        super().__init__(master, takefocus=True, **kw)
        self.items, self.cards, self.sel, self.off = [], [], -1, False
        self.empty = ttk.Label(self, text=empty_text, style="Hint.TLabel", wraplength=ui_theme.px(360),
                               justify="left")
        self.empty.pack(anchor="w", pady=ui_theme.px(6))
        self.bind("<Up>", lambda e: self._step(-1))
        self.bind("<Down>", lambda e: self._step(1))

    def set_empty_text(self, text):
        self.empty.configure(text=text)

    def set_items(self, items):
        for c in self.cards:
            c.destroy()
        self.items, self.cards, self.sel = list(items), [], -1
        if items:
            self.empty.pack_forget()
        else:
            self.empty.pack(anchor="w", pady=ui_theme.px(6))
        for i, it in enumerate(items):
            card = ui_theme.card(self, style="Drive.TFrame", padding=(12, 9))
            card.pack(fill="x", pady=(0, ui_theme.px(8)))
            top = ttk.Frame(card)
            top.pack(fill="x")
            ttk.Label(top, text=it.get("title", ""), style="Bold.TLabel").pack(side="left")
            ttk.Label(top, text=it.get("size", ""), style="Muted.TLabel").pack(side="right")
            if it.get("sub"):
                ttk.Label(card, text=it["sub"], style="Hint.TLabel", wraplength=ui_theme.px(340),
                          justify="left").pack(anchor="w", pady=(ui_theme.px(2), 0))
            if it.get("pills"):
                row = ttk.Frame(card)
                row.pack(anchor="w", pady=(ui_theme.px(5), 0))
                for text, kind in it["pills"]:
                    ttk.Label(row, text=text, style=f"Pill{kind}.TLabel").pack(side="left", padx=(0, ui_theme.px(4)))
            if it.get("used") is not None:
                bar = ttk.Progressbar(card, style=("UsageWarn" if it["used"] > .9 else "Usage") +
                                      ".Horizontal.TProgressbar", maximum=1000, value=round(it["used"] * 1000))
                bar.pack(fill="x", pady=(ui_theme.px(7), 0))
            _bind_all(card, "<Button-1>", lambda e, i=i: self._click(i))
            _bind_all(card, "<Enter>", lambda e, i=i: self._hover(i, True))
            _bind_all(card, "<Leave>", lambda e, i=i: self._hover(i, False))
            self.cards.append(card)

    def refresh(self, items):
        """wie set_items, aber nur bei Änderungen neu aufbauen; die Auswahl bleibt (gleiche Stelle)"""
        if list(items) == self.items:
            return
        sel = self.sel
        self.set_items(items)
        self.current(sel)

    def _style(self, i, hover=False):
        if 0 <= i < len(self.cards):
            self.cards[i].configure(style="DriveSel.TFrame" if i == self.sel else
                                    "DriveHover.TFrame" if hover and not self.off else "Drive.TFrame")

    def _hover(self, i, on):
        self._style(i, on)

    def _click(self, i):
        if self.off:
            return
        self.focus_set()
        if i != self.sel:
            self.current(i)
            self.event_generate("<<ComboboxSelected>>")

    def _step(self, d):
        if self.off or not self.items:
            return
        i = min(len(self.items) - 1, max(0, self.sel + d))
        self._click(i)

    def current(self, i=None):
        if i is None:
            return self.sel
        old, self.sel = self.sel, i if 0 <= i < len(self.items) else -1
        self._style(old)
        self._style(self.sel)
        return self.sel

    def cget(self, key):
        if key == "values":
            return tuple(it.get("text", it.get("title", "")) for it in self.items)
        return super().cget(key)

    def state(self, spec=None):
        if spec is not None:
            self.off = "disabled" in spec
        return super().state(spec) if spec is not None else super().state()


class ChoiceChips(ttk.Frame):
    """Auswahl-Kacheln (eine gewählt) - für "Wofür soll der Datenträger dienen?". options: [(schlüssel, text)];
    command(schlüssel) beim Anklicken."""

    def __init__(self, master, options, command, columns=3, **kw):
        super().__init__(master, **kw)
        self.command, self.selected, self.buttons = command, None, {}
        for i, (key, text) in enumerate(options):
            b = ttk.Button(self, text=text, style="Chip.TButton", command=lambda k=key: self._pick(k))
            b.grid(row=i // columns, column=i % columns, sticky="ew",
                   padx=(0 if i % columns == 0 else ui_theme.px(6), 0), pady=(0 if i < columns else ui_theme.px(6), 0))
            self.buttons[key] = b
        for col in range(columns):
            self.columnconfigure(col, weight=1, uniform="chip")

    def _pick(self, key):
        self.select(key)
        self.command(key)

    def select(self, key):
        self.selected = key
        for k, b in self.buttons.items():
            b.configure(style="ChipSel.TButton" if k == key else "Chip.TButton")

    def enable(self, key, on):
        b = self.buttons.get(key)
        if b is not None:
            b.state(["!disabled"] if on else ["disabled"])


def app_header(parent, title, version, theme_mode, on_theme, lang_box_factory, on_help):
    """Kopfzeile: Symbol und Name mit Version links; rechts Hell/Dunkel, Sprache und Hilfe.
    lang_box_factory(parent) baut die Sprachauswahl; liefert (karte, sprachauswahl)."""
    bar = ui_theme.card(parent, padding=(14, 9))
    tile = ttk.Label(bar, image=ui_theme.app_tile(bar, 30))
    tile.pack(side="left")
    ttk.Label(bar, text=title, style="H2.TLabel").pack(side="left", padx=(ui_theme.px(10), ui_theme.px(8)))
    if version:
        ttk.Label(bar, text="v" + version, style="Pill.TLabel").pack(side="left")
    box = header_tools(bar, theme_mode, on_theme, lang_box_factory, on_help)
    return bar, box


def header_tools(bar, theme_mode, on_theme, lang_box_factory, on_help):
    """rechts in einer Kopfzeile: Hell/Dunkel, Sprache, Hilfe (von rechts nach links gepackt). Liefert die
    Sprachauswahl."""
    ttk.Button(bar, text="?", style="Ghost.TButton", width=2, command=on_help).pack(side="right")
    box = lang_box_factory(bar)
    box.pack(side="right", padx=(ui_theme.px(6), ui_theme.px(4)))
    dark = theme_mode == "dark"
    ico = ui_theme.icon(bar, "sun" if dark else "moon", "muted", 16)
    tb = ttk.Button(bar, text=" " + (_("Hell") if dark else _("Dunkel")), image=ico, compound="left",
                    style="Ghost.TButton", command=on_theme)
    tb.image = ico
    tb.pack(side="right")
    return box
