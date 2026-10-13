"""Erzeugt app.ico (USB-Stick-Symbol) fuer Fenster und EXE. Benoetigt Pillow."""
import os

from PIL import Image, ImageDraw

S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

d.rounded_rectangle((88, 18, 168, 92), radius=10, fill=(176, 180, 186), outline=(110, 114, 120), width=6)
d.rectangle((106, 40, 120, 58), fill=(90, 94, 100))
d.rectangle((136, 40, 150, 58), fill=(90, 94, 100))
d.rounded_rectangle((60, 82, 196, 240), radius=26, fill=(24, 95, 165))
d.rounded_rectangle((60, 82, 196, 240), radius=26, outline=(12, 68, 124), width=6)
cx = 128
d.line((cx, 112, cx, 210), fill="white", width=10)
d.polygon([(cx, 100), (cx - 14, 124), (cx + 14, 124)], fill="white")
d.line((cx, 184, cx - 34, 160), fill="white", width=9)
d.line((cx, 168, cx + 34, 146), fill="white", width=9)
d.ellipse((cx - 46, 148, cx - 22, 172), fill="white")
d.rectangle((cx + 24, 134, cx + 46, 156), fill="white")
d.ellipse((cx - 16, 200, cx + 16, 232), fill="white")

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
img.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("geschrieben:", out)
