"""Sprache der Oberfläche: Deutsch (die Texte im Code), English, Italiano, Español, Français.
Übersetzungen in i18n_app.json - Schlüssel ist der deutsche Text, Platzhalter {name} bleiben in allen Sprachen gleich.
tools/i18n_check.py prüft, dass jeder Text in allen Sprachen vorliegt."""
import ctypes
import json
import locale
import os
import sys

LANGS = [("de", "Deutsch"), ("en", "English"), ("it", "Italiano"), ("es", "Español"), ("fr", "Français")]
_lang = "de"
_cat = None


def _catalog():
    global _cat
    if _cat is None:
        base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
        try:
            with open(os.path.join(base, "i18n_app.json"), encoding="utf-8") as f:
                _cat = json.load(f)
        except (OSError, ValueError):
            _cat = {}
    return _cat


def system_lang():
    """Sprache des Systems, sonst Englisch"""
    code = ""
    if sys.platform == "win32":
        try:
            primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF
            code = {0x07: "de", 0x09: "en", 0x10: "it", 0x0A: "es", 0x0C: "fr"}.get(primary, "")
        except (AttributeError, OSError):
            pass
    if not code:
        loc = os.environ.get("LC_ALL") or os.environ.get("LC_MESSAGES") or os.environ.get("LANG") or ""
        if not loc:
            try:
                loc = locale.getlocale()[0] or ""
            except ValueError:
                loc = ""
        code = loc[:2].lower()
    return code if code in dict(LANGS) else "en"


def set_lang(code):
    global _lang
    _lang = code if code in dict(LANGS) else "en"


def lang():
    return _lang


def _(text, **kw):
    """Übersetzen; mit kw werden die Platzhalter {name} danach ausgefüllt"""
    if _lang != "de":
        text = (_catalog().get(text) or {}).get(_lang) or text
    return text.format(**kw) if kw else text


def unit(name):
    """Einheiten: Französisch schreibt Ko/Mo/Go/To und octets"""
    if _lang == "fr":
        return {"Bytes": "octets", "KB": "Ko", "MB": "Mo", "GB": "Go", "TB": "To"}.get(name, name)
    return name


def quote(text):
    """Anführungszeichen der Sprache"""
    return {"de": f"„{text}“", "en": f"“{text}”", "fr": f"« {text} »"}.get(_lang, f"«{text}»")


def number(text):
    """Dezimaltrennzeichen: Englisch Punkt, sonst Komma"""
    return text if _lang == "en" else text.replace(".", ",")
