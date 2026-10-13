"""Erzeugt den Hintergrund der App-Kachel auf dem PS5-Startbildschirm: app/sce_sys/pic0.png und pic1.png
(1920x1080) aus einem Querformat-Bild; weicht das Seitenverhältnis ab, wird mittig zugeschnitten. Dazu pic0.dds
(BC7, siehe bc7.py): Der Startbildschirm liest den Hintergrund aus dieser Textur, die sonst der PKG-Installer
anlegt. Benötigt Pillow und numpy.
Aufruf: python make_background.py <Querformat-Bild>"""
import os
import sys

from PIL import Image, ImageOps

import bc7

src = Image.open(sys.argv[1]).convert("RGB")
bg = ImageOps.fit(src, (1920, 1080), Image.LANCZOS)

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "app", "sce_sys")
for name in ("pic0.png", "pic1.png"):
    bg.save(os.path.join(out, name), optimize=True)
bc7.save_dds(bg, os.path.join(out, "pic0.dds"))
print("geschrieben:", os.path.abspath(out), bg.size, "pic0.png, pic1.png, pic0.dds")
