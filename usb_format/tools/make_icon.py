"""Erzeugt app/sce_sys/icon0.png (512x512), das Symbol der App. Benoetigt Pillow.
Aufruf: python make_icon.py [Bild]   - mit Bild wird es mittig quadratisch zugeschnitten und verkleinert,
                                       ohne Bild wird das einfache gezeichnete Symbol erzeugt."""
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageOps

S = 512
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "app", "sce_sys", "icon0.png")

if len(sys.argv) > 1:
    ImageOps.fit(Image.open(sys.argv[1]).convert("RGB"), (S, S), Image.LANCZOS).save(OUT, optimize=True)
    print("geschrieben:", os.path.abspath(OUT))
    sys.exit(0)

BG = (19, 52, 92)
img = Image.new("RGB", (S, S), BG)
d = ImageDraw.Draw(img)


def font(size):
    for name in ("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        for base in (os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts"), "/usr/share/fonts/truetype/dejavu"):
            try:
                return ImageFont.truetype(os.path.join(base, name), size)
            except OSError:
                pass
    return ImageFont.load_default()


# USB-Stick
cx = S // 2
d.rounded_rectangle((cx - 58, 46, cx + 58, 140), radius=12, fill=(190, 194, 200))
d.rectangle((cx - 32, 76, cx - 12, 100), fill=(95, 99, 105))
d.rectangle((cx + 12, 76, cx + 32, 100), fill=(95, 99, 105))
d.rounded_rectangle((cx - 96, 128, cx + 96, 352), radius=34, fill=(55, 138, 221))

# USB-Symbol auf dem Stick
w = (255, 255, 255)
d.line((cx, 170, cx, 314), fill=w, width=14)
d.polygon([(cx, 154), (cx - 20, 188), (cx + 20, 188)], fill=w)
d.line((cx, 276, cx - 46, 242), fill=w, width=12)
d.line((cx, 254, cx + 46, 222), fill=w, width=12)
d.ellipse((cx - 64, 226, cx - 32, 258), fill=w)
d.rectangle((cx + 32, 204, cx + 62, 234), fill=w)
d.ellipse((cx - 22, 300, cx + 22, 344), fill=w)

# Beschriftung
for text, y, size, col in (("Formatierer", 374, 58, (255, 255, 255)), ("exFAT · FAT32", 440, 34, (181, 212, 244))):
    f = font(size)
    tw = d.textlength(text, font=f)
    d.text(((S - tw) / 2, y), text, font=f, fill=col, anchor="la")

img.save(OUT, optimize=True)
print("geschrieben:", os.path.abspath(OUT))
