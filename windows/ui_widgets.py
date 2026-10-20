"""Bausteine der modernen Oberfläche: Laufwerke als Karten (DriveList, scrollbar), Auswahl-Kacheln (ChoiceChips),
Kurzinfos (Tooltip) und die Kopfzeile der Programme (app_header). Alles ttk-Widgets im Theme von ui_theme."""
import tkinter as tk
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

    def __init__(self, master, empty_text="", max_height=400, **kw):
        super().__init__(master, takefocus=True, **kw)
        self.items, self.cards, self.sel, self.off = [], [], -1, False
        self.max_h = ui_theme.px(max_height)   # so hoch höchstens von sich aus - mehr Platz nimmt sie gern
        bg = ui_theme.colors()["card"]
        self.canvas = tk.Canvas(self, highlightthickness=0, bd=0, bg=bg, height=ui_theme.px(80))
        self.sbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.sbar.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.inner = ttk.Frame(self.canvas)
        self.win = self.canvas.create_window(0, 0, window=self.inner, anchor="nw")
        self.inner.bind("<Configure>", lambda e: self._layout())
        self.canvas.bind("<Configure>", lambda e: (self.canvas.itemconfigure(self.win, width=e.width),
                                                   self._layout(resize=False)))
        self.empty = ttk.Label(self.inner, text=empty_text, style="Hint.TLabel", wraplength=ui_theme.px(360),
                               justify="left")
        self.empty.pack(anchor="w", pady=ui_theme.px(6))
        self.bind("<Up>", lambda e: self._step(-1))
        self.bind("<Down>", lambda e: self._step(1))
        for w in (self.canvas, self.inner):
            w.bind("<Enter>", lambda e: self._wheel(True), add="+")
            w.bind("<Leave>", lambda e: self._wheel(False), add="+")

    def _layout(self, resize=True):
        """Bildlaufbereich anpassen; Leiste nur, wenn nicht alles hineinpasst"""
        need = self.inner.winfo_reqheight()
        self.canvas.configure(scrollregion=(0, 0, 0, need))
        if resize:
            self.canvas.configure(height=max(ui_theme.px(40), min(need, self.max_h)))
        if need > self.canvas.winfo_height() + 1:
            if not self.sbar.winfo_ismapped():
                self.sbar.pack(side="right", fill="y", padx=(ui_theme.px(4), 0))
        elif self.sbar.winfo_ismapped():
            self.sbar.pack_forget()
            self.canvas.yview_moveto(0)

    def _wheel(self, on):
        if on:
            self.bind_all("<MouseWheel>", lambda e: self._scroll(-1 if e.delta > 0 else 1))
            self.bind_all("<Button-4>", lambda e: self._scroll(-1))
            self.bind_all("<Button-5>", lambda e: self._scroll(1))
        else:
            for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
                self.unbind_all(seq)

    def _scroll(self, d):
        if self.sbar.winfo_ismapped():
            self.canvas.yview_scroll(d * 2, "units")

    def _see(self, i):
        """Karte i in den sichtbaren Bereich holen"""
        if not (0 <= i < len(self.cards)) or not self.sbar.winfo_ismapped():
            return
        self.update_idletasks()
        total = max(1, self.inner.winfo_height())
        y0, y1 = self.cards[i].winfo_y(), self.cards[i].winfo_y() + self.cards[i].winfo_height()
        top, bottom = (f * total for f in self.canvas.yview())
        if y0 < top:
            self.canvas.yview_moveto(y0 / total)
        elif y1 > bottom:
            self.canvas.yview_moveto((y1 - (bottom - top)) / total)

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
            card = ui_theme.card(self.inner, style="Drive.TFrame", padding=(12, 9))
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
            _bind_all(card, "<Enter>", lambda e: self._wheel(True))
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
        self._see(self.sel)
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


class Tooltip:
    """Kurzinfo beim Verweilen über einem Widget. text: Text oder Funktion ohne Argumente (leer = keine Kurzinfo) -
    so lässt sich zeigen, warum ein Knopf gerade gesperrt ist."""
    DELAY = 550

    def __init__(self, widget, text):
        self.widget, self.text, self.tip, self.after_id = widget, text, None, None
        widget.bind("<Enter>", self._enter, add="+")
        widget.bind("<Leave>", self._leave, add="+")
        widget.bind("<ButtonPress>", self._leave, add="+")
        widget.bind("<Destroy>", self._leave, add="+")

    def _enter(self, _e=None):
        self._cancel()
        self.after_id = self.widget.after(self.DELAY, self._show)

    def _cancel(self):
        if self.after_id:
            try:
                self.widget.after_cancel(self.after_id)
            except tk.TclError:
                pass
            self.after_id = None

    def _leave(self, _e=None):
        self._cancel()
        if self.tip is not None:
            try:
                self.tip.destroy()
            except tk.TclError:
                pass
            self.tip = None

    def _show(self):
        self.after_id = None
        try:
            text = self.text() if callable(self.text) else self.text
            if not text or not self.widget.winfo_ismapped():
                return
            c = ui_theme.colors()
            tip = self.tip = tk.Toplevel(self.widget)
            tip.wm_overrideredirect(True)
            tip.configure(bg=c["border2"])
            tk.Label(tip, text=text, bg=c["card2"], fg=c["text"], font="UfSmall", justify="left",
                     wraplength=ui_theme.px(320), padx=ui_theme.px(9), pady=ui_theme.px(5)).pack(padx=1, pady=1)
            x = self.widget.winfo_rootx() + ui_theme.px(8)
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + ui_theme.px(4)
            tip.update_idletasks()
            x = min(x, self.widget.winfo_screenwidth() - tip.winfo_reqwidth() - 4)
            tip.geometry(f"+{max(0, x)}+{y}")
        except tk.TclError:
            self.tip = None


THEME_NEXT = {"auto": "light", "light": "dark", "dark": "auto"}


def app_header(parent, title, version, theme_choice, on_theme, lang_box_factory, on_help):
    """Kopfzeile: Symbol und Name mit Version links; rechts Hell/Dunkel, Sprache und Hilfe.
    lang_box_factory(parent) baut die Sprachauswahl; liefert (karte, sprachauswahl)."""
    bar = ui_theme.card(parent, padding=(14, 9))
    tile = ttk.Label(bar, image=ui_theme.app_tile(bar, 30))
    tile.pack(side="left")
    ttk.Label(bar, text=title, style="H2.TLabel").pack(side="left", padx=(ui_theme.px(10), ui_theme.px(8)))
    if version:
        ttk.Label(bar, text="v" + version, style="Pill.TLabel").pack(side="left")
    box = header_tools(bar, theme_choice, on_theme, lang_box_factory, on_help)
    return bar, box


def header_tools(bar, theme_choice, on_theme, lang_box_factory, on_help):
    """rechts in einer Kopfzeile: Design (Automatisch/Hell/Dunkel, Klick schaltet weiter), Sprache, Hilfe (von rechts
    nach links gepackt). theme_choice: die Einstellung ("auto", "light", "dark"). Liefert die Sprachauswahl."""
    hb = ttk.Button(bar, text="?", style="Ghost.TButton", width=2, command=on_help)
    hb.pack(side="right")
    Tooltip(hb, _("Anleitung öffnen (F1)"))
    box = lang_box_factory(bar)
    box.pack(side="right", padx=(ui_theme.px(6), ui_theme.px(4)))
    choice = theme_choice if theme_choice in THEME_NEXT else "auto"
    names = {"auto": _("Automatisch"), "light": _("Hell"), "dark": _("Dunkel")}
    ico = ui_theme.icon(bar, {"auto": "auto", "light": "sun", "dark": "moon"}[choice], "muted", 16)
    tb = ttk.Button(bar, text=" " + names[choice], image=ico, compound="left", style="Ghost.TButton", command=on_theme)
    tb.image = ico
    tb.pack(side="right")
    Tooltip(tb, _("Design: {now} – Klick wechselt zu „{next}“. Automatisch folgt dem System.", now=names[choice],
                  next=names[THEME_NEXT[choice]]))
    return box
