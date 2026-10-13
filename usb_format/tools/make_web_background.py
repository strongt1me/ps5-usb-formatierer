"""Erzeugt web/bg.jpg, den Hintergrund der Karte in der Web-UI, aus dem Hochformat-Motiv. Benötigt Pillow.
Aufruf: python make_web_background.py <Hochformat-Bild>"""
import os
import sys

from PIL import Image

img = Image.open(sys.argv[1]).convert("RGB")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web", "bg.jpg")
img.save(out, "JPEG", quality=82, optimize=True, progressive=True)
print("geschrieben:", os.path.abspath(out), img.size, os.path.getsize(out) // 1024, "KB")
