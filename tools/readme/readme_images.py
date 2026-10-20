"""README-Bilder ins Repository: docs/images/logo.png (abgerundet, transparent) und docs/images/<sprache>/*.webp"""
import os
import sys

from PIL import Image, ImageDraw

RAW = sys.argv[1]
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
DST = os.path.join(ROOT, "docs", "images")
os.makedirs(DST, exist_ok=True)

icon = Image.open(os.path.join(ROOT, "usb_format", "app", "art", "icon.png")).convert("RGBA")
icon = icon.resize((320, 320), Image.LANCZOS)
mask = Image.new("L", icon.size, 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, 319, 319), radius=64, fill=255)
icon.putalpha(mask)
icon.save(os.path.join(DST, "logo.png"), optimize=True)

total = 0
for lang in ("de", "en", "it", "es", "fr"):
    d = os.path.join(DST, lang)
    os.makedirs(d, exist_ok=True)
    for name in ("web", "ps5", "dialog", "light", "help", "backup", "win_ps5", "win_std", "win_expert"):
        im = Image.open(os.path.join(RAW, f"{name}_{lang}.png")).convert("RGB")
        if name == "backup":   # Ausschnitt: Laufwerke, Inhalt und der Bereich "Sichern & wiederherstellen"
            pane = im.getpixel((140, 700))   # Hintergrund der linken Karte
            bottom = next(y for y in range(im.height - 1, 700, -1)
                          if sum(abs(a - b) for a, b in zip(im.getpixel((140, y)), pane)) < 12)
            im = im.crop((116, bottom + 14 - 723, 1164, bottom + 14))
        if name in ("ps5", "dialog", "light", "help"):   # Galerie: alle im selben Seitenverhältnis (1,45 : 1)
            h = min(im.height, round(im.width / 1.45))
            w = min(im.width, round(h * 1.45))
            x = (im.width - w) // 2
            im = im.crop((x, 0, x + w, h))
        out = os.path.join(d, f"{name}.webp")
        im.save(out, "WEBP", quality=88, method=6)
        total += os.path.getsize(out)
print(f"logo.png {os.path.getsize(os.path.join(DST, 'logo.png'))} Bytes, 45 Bilder zusammen {total / 1e6:.1f} MB")
