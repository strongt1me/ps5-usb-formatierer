"""Erzeugt Vorschläge für den Seitenhintergrund der Weboberfläche: je Motiv ein dunkles und ein helles Bild
(1920x1080, JPEG) in den Blautönen der Kachel. Die Mitte bleibt ruhig, dort liegt die Karte. Benötigt Pillow und numpy.
Aufruf: python make_page_background.py <Zielordner> [Motiv ...]   (Vorschläge)
       python make_page_background.py --web 5_platine              (web/page-dark.jpg, web/page-light.jpg)"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

W, H = 1920, 1080
SS = 2  # Überabtastung für glatte Kanten

THEMES = {
    "dunkel": dict(top=(8, 12, 26), bottom=(3, 5, 13), edge=(1, 2, 6),
                   line=(55, 135, 255), core=(170, 215, 255), glow=(20, 85, 230), la=0.6, ga=0.5),
    # gedämpftes Blaugrau statt fast Weiß, damit das helle Design nicht blendet
    "hell": dict(top=(198, 208, 224), bottom=(182, 194, 214), edge=(158, 172, 196),
                 line=(40, 92, 186), core=(28, 68, 160), glow=(140, 164, 212), la=0.5, ga=0.45),
}


def grid(scale=1):
    y, x = np.mgrid[0:H * scale, 0:W * scale].astype(np.float32)
    return (x + 0.5) / scale, (y + 0.5) / scale  # in Bildpunkten des Endbilds


def down(a):
    return a.reshape(H, SS, W, SS).mean(axis=(1, 3))


def canvas():
    img = Image.new("L", (W * SS, H * SS))
    return img, ImageDraw.Draw(img)


def from_canvas(img):
    return np.asarray(img.resize((W, H), Image.BOX), np.float32) / 255


def blur(a, sigma):
    p = int(3 * sigma)
    b = np.pad(a, p, mode="reflect")
    fy = np.fft.fftfreq(b.shape[0])[:, None]
    fx = np.fft.rfftfreq(b.shape[1])[None, :]
    k = np.exp(-2 * (np.pi * sigma) ** 2 * (fx ** 2 + fy ** 2))
    return np.fft.irfft2(np.fft.rfft2(b) * k, s=b.shape)[p:-p, p:-p].astype(np.float32)


def g2(u, v, cx, cy, sx, sy):
    return np.exp(-(((u - cx) / sx) ** 2 + ((v - cy) / sy) ** 2))


def verlauf(rng):
    """Nur ein Farbverlauf mit sanftem Lichtschein."""
    x, y = grid()
    u, v = x / W, y / H
    glow = 0.9 * g2(u, v, 0.5, 1.12, 0.62, 0.42) + 0.45 * g2(u, v, 1.02, -0.05, 0.32, 0.38) \
        + 0.2 * g2(u, v, -0.05, 0.6, 0.25, 0.45)
    return np.zeros_like(glow), glow


def horizont(rng):
    """Leuchtender Bogen am unteren Rand, wie ein Planetenhorizont."""
    x, y = grid()
    cx, cy, rx, ry = W / 2, H * 1.75, W * 1.05, H * 0.93
    d = (np.sqrt(((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2) - 1) * ry  # Abstand zum Rand, außen positiv
    fall = np.exp(-((x - cx) / (W * 0.42)) ** 2)
    lines = np.exp(-(d / 1.4) ** 2) * (0.3 + 0.7 * fall)
    glow = np.where(d > 0, 0.75 * np.exp(-d / 60) + 0.35 * np.exp(-d / 260), 0.45 * np.exp(d / 28))
    return lines, glow * (0.25 + 0.75 * fall)


def raster(rng):
    """Perspektivisches Gitter, das zum Horizont hin verblasst."""
    x, y = grid(SS)
    h = H * 0.6
    c = H - h  # Tiefe 1 am unteren Rand
    dy = np.maximum(y - h, 1e-3)
    z = c / dy
    sp, s = 110.0, 0.32
    xl = (x - W / 2) * z / sp
    dxp = np.abs(xl - np.round(xl)) * sp / z  # Bildpunkte bis zur nächsten Längslinie
    zl = z / s
    dyp = np.abs(zl - np.round(zl)) * s * dy * dy / c  # Bildpunkte bis zur nächsten Querlinie
    w = 0.45 + 0.5 / z
    below = y > h
    side = np.exp(-((x - W / 2) / (W * 0.55)) ** 2)
    lines = ((dxp < w) * np.exp(-(z - 1) / 4.5) + (dyp < w) * np.exp(-(z - 1) / 2.5)) * below
    lines = down(np.minimum(lines, 1) * (0.35 + 0.65 * side))
    x, y = grid()
    side = np.exp(-((x - W / 2) / (W * 0.4)) ** 2)
    glow = (0.9 * np.exp(-((y - h) / 30) ** 2) + 0.45 * np.exp(-np.abs(y - h) / 150)) * side
    return lines, glow


def wellen(rng):
    """Bündel aus Lichtwellen mit Punktpartikeln, wie in der Kachel."""
    img, d = canvas()
    xs = np.linspace(-20, W + 20, 700)

    def curve(t, x, y0, amp, phase):
        ph = phase + t * 0.9
        return y0 + t * 70 + amp * (1 + 0.6 * t) * np.sin(2 * np.pi * x / W * 0.85 + ph) \
            + amp * 0.45 * np.sin(2 * np.pi * x / W * 1.9 + ph * 1.7 + 1.3)

    def bundle(y0, amp, n, phase, alpha):
        for i in range(n):
            t = i / (n - 1)
            val = alpha * (0.3 + 0.7 * math.sin(math.pi * t))
            pts = list(zip((xs * SS).tolist(), (curve(t, xs, y0, amp, phase) * SS).tolist()))
            d.line(pts, fill=int(200 * val), width=SS)
            if i % 3:
                continue
            for xp in np.arange(rng.uniform(0, 16), W, 16):
                yp = curve(t, xp, y0, amp, phase) + rng.normal(0, 4)
                rad = rng.uniform(0.6, 2.0) * SS
                b = int(255 * val * rng.uniform(0.4, 1))
                d.ellipse((xp * SS - rad, yp * SS - rad, xp * SS + rad, yp * SS + rad), fill=b)

    bundle(H * 0.68, 60, 26, 0.3, 1.0)
    bundle(H * 0.12, 40, 14, 2.4, 0.45)
    lines = from_canvas(img)
    return lines, np.minimum(blur(lines, 12) * 2.5 + blur(lines, 50) * 4, 1)


def platine(rng):
    """Feine Leiterbahnen an den Seiten, die Mitte bleibt frei."""
    g = 24
    cols, rows = W // g + 1, H // g + 1
    used = np.zeros((rows + 1, cols + 1), bool)
    img, d = canvas()
    dirs = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]

    def calm(c):
        return abs(c * g - W / 2) < W * 0.16

    def free(c, r):
        return 0 <= c <= cols and 0 <= r <= rows and not used[r, c] and not calm(c)

    traces = tries = 0
    while traces < 90 and tries < 6000:
        tries += 1
        c, r = int(rng.integers(0, cols)), int(rng.integers(0, rows))
        if not free(c, r):
            continue
        di = 0 if c * g > W / 2 else 4  # vorwiegend nach außen
        if rng.random() < 0.35:
            di = 2 if rng.random() < 0.5 else 6
        pts = [(c, r)]
        used[r, c] = True
        for _ in range(int(rng.integers(2, 7))):
            dx, dy = dirs[di]
            n = int(rng.integers(1, 4)) if di % 2 else int(rng.integers(3, 12))
            ok = 0
            while ok < n and free(c + dx, r + dy):
                c, r = c + dx, r + dy
                used[r, c] = True
                ok += 1
            if ok:
                pts.append((c, r))
            if ok < n:
                break
            di = (di + (1 if rng.random() < 0.5 else -1)) % 8  # 45°-Knick
        if len(pts) < 2:
            continue
        val = int(255 * rng.uniform(0.45, 1))
        pp = [(pc * g * SS, pr * g * SS) for pc, pr in pts]
        d.line(pp, fill=val, width=int(1.6 * SS), joint="curve")
        (x0, y0), (x1, y1) = pp[0], pp[-1]
        d.ellipse((x0 - 3 * SS, y0 - 3 * SS, x0 + 3 * SS, y0 + 3 * SS), fill=val)
        d.ellipse((x1 - 5 * SS, y1 - 5 * SS, x1 + 5 * SS, y1 + 5 * SS), outline=val, width=int(1.6 * SS))
        traces += 1
    lines = from_canvas(img)
    x, _ = grid()
    lines *= np.clip((np.abs(x / W - 0.5) - 0.16) / 0.2, 0, 1) ** 0.7
    return lines, np.minimum(blur(lines, 4) * 1.2 + blur(lines, 40) * 3, 1)


def waben(rng):
    """Wabenraster, zur Mitte hin ausgeblendet, einzelne Waben leuchten leicht."""
    x, y = grid(SS)
    s, r3 = 44.0, math.sqrt(3)
    q = (2 / 3 * x) / s
    r = (-1 / 3 * x + r3 / 3 * y) / s
    cy_ = -q - r
    rx, ry, rz = np.round(q), np.round(cy_), np.round(r)
    ex, ey, ez = np.abs(rx - q), np.abs(ry - cy_), np.abs(rz - r)
    m1 = (ex > ey) & (ex > ez)
    m2 = ~m1 & (ey > ez)
    rx = np.where(m1, -ry - rz, rx)
    rz = np.where(~m1 & ~m2, -rx - ry, rz)
    px = np.abs(x - s * 1.5 * rx)
    py = np.abs(y - s * r3 * (rz + rx / 2))
    edge = s * r3 / 2 - np.maximum(py, px * r3 / 2 + py / 2)
    hsh = np.modf(np.abs(np.sin(rx * 12.9898 + rz * 78.233) * 43758.5453))[0]
    fill = np.where(hsh < 0.08, 0.2 + hsh * 5, 0)
    u, v = x / W, y / H
    dist = np.sqrt(((u - 0.5) * W / H) ** 2 + (v - 0.45) ** 2)
    mask = np.clip((dist - 0.32) / 0.45, 0, 1) * (0.6 + 0.4 * v)
    lines = down((edge < 0.7) * mask * 0.8 + fill * mask * 0.6)
    fills = down(fill * mask)
    x, y = grid()
    glow = blur(fills, 25) * 2.5 + 0.35 * g2(x / W, y / H, 1.0, 1.0, 0.35, 0.45) + 0.25 * g2(x / W, y / H, 0, 0, 0.3, 0.4)
    return lines, np.minimum(glow, 1)


MOTIVE = {"1_verlauf": verlauf, "2_horizont": horizont, "3_raster": raster,
          "4_wellen": wellen, "5_platine": platine, "6_waben": waben}


def compose(lines, glow, th, rng):
    x, y = grid()
    u, v = (x / W)[..., None], (y / H)[..., None]
    top, bottom, edge = (np.array(th[k], np.float32) for k in ("top", "bottom", "edge"))
    img = top + (bottom - top) * v
    vig = np.clip((u - 0.5) ** 2 * 1.6 + (v - 0.5) ** 2 * 1.2, 0, 1)
    img = img + (edge - img) * vig * 0.9
    for color, a in ((th["glow"], np.clip(glow, 0, 1) * th["ga"]),
                     (th["line"], np.clip(lines, 0, 1) * th["la"]),
                     (th["core"], np.clip(lines, 0, 1) ** 3 * th["la"] * 0.6)):
        img = img + (np.array(color, np.float32) - img) * a[..., None]
    img += rng.random(img.shape, np.float32) - rng.random(img.shape, np.float32)  # gegen Farbstufen
    return Image.fromarray(np.clip(np.round(img), 0, 255).astype(np.uint8))


WEB_NAMES = {"dunkel": "page-dark.jpg", "hell": "page-light.jpg"}


def main():
    if sys.argv[1] == "--web":  # gewähltes Motiv als Seitenhintergrund der Weboberfläche (web/page-*.jpg)
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")
        names = sys.argv[2:3]
    else:
        out = sys.argv[1]
        names = sys.argv[2:] or MOTIVE
    os.makedirs(out, exist_ok=True)
    for name in names:
        rng = np.random.default_rng(7)
        lines, glow = MOTIVE[name](rng)
        for theme, th in THEMES.items():
            fn = WEB_NAMES[theme] if sys.argv[1] == "--web" else f"seite_{name}_{theme}.jpg"
            path = os.path.join(out, fn)
            compose(lines, glow, th, rng).save(path, quality=88, optimize=True, progressive=True)
            print(path, os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    main()
