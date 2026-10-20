"""README-Bilder der Windows-Programme in einer Sprache - nur das Programmfenster (PrintWindow), ohne echte
Datenträger und ohne PS5: Der PS5-Modus spricht mit readme_proxy.py (127.0.0.1:8095), der PC-Teil bekommt eine
gestellte Laufwerksliste. Einstellungen nur im Speicher, nie ein Auftrag.
Aufruf: python readme_win_shots.py <sprache> <ausgabeordner>"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "windows"))
sys.path.insert(0, HERE)
os.environ["USBFMT_NO_UPDATE_CHECK"] = "1"   # kein Update-Hinweis in den Bildern
from winshot import grab_window  # noqa: E402

import ps5_discovery  # noqa: E402
import standard_panel as sp  # noqa: E402
import usb_formatierer as uf  # noqa: E402
import usb_standard  # noqa: E402

LANG, OUT = sys.argv[1], sys.argv[2]
GB = 1024 ** 3
DEVICES = [
    {"disk": 3, "size": 61530439680, "ss": 512, "removable": True, "model": "SanDisk Ultra",
     "volumes": [{"letters": "E:", "fs": "exFAT", "label": "SANDISK", "total": 61 * 10 ** 9, "free": 48 * 10 ** 9}]},
    {"disk": 4, "size": 1000204886016, "ss": 512, "removable": False, "model": "Samsung PSSD T7",
     "volumes": [{"letters": "F:", "fs": "NTFS", "label": "T7", "total": 10 ** 12, "free": 7 * 10 ** 11}]},
]


class NoJob:   # nie ein echter Auftrag
    def __init__(self, *a, **k):
        raise RuntimeError("Vorschau: keine Aufträge")


def no_payload(*a, **k):
    raise OSError("Vorschau: kein Payload")


class ProxyApi(uf.Api):
    def __init__(self, host):
        self.base = "http://127.0.0.1:8095"


uf.load_settings = lambda: {"lang": LANG, "mode": "ps5", "host": "192.168.1.50", "theme": "light", "page": "games"}
uf.save_settings = lambda data: None
uf.Api = ProxyApi
uf.send_payload = no_payload
uf.PcJob = sp.PcJob = NoJob
ps5_discovery.discover = lambda port, progress=None: [{"ip": "192.168.1.50", "name": "PS5", "standby": False, "formatter": True}]
uf.pc_list = sp.pc_list = lambda progress=None: [dict(d, volumes=[dict(v) for v in d["volumes"]]) for d in DEVICES]
usb_standard.load_settings = lambda name: {"lang": LANG, "theme": "light"}
usb_standard.save_settings = lambda name, data: None
uf.enable_dpi_awareness()


def pump(app, cond=lambda: False, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        app.update()
        if cond():
            return True
        time.sleep(0.02)
    return cond()


# ------------------------------------------------------------------ PS5-USB-Formatierer, Modus "USB an der PS5"
app = uf.App()
app.attributes("-topmost", True)
pump(app, lambda: app.api is not None and app.devices and app.games, timeout=12)
names = list(app.dev_combo.cget("values"))
idx = next((i for i, n in enumerate(names) if "Realtek" in n), 0)
app.dev_combo.current(idx)
app._update()
app._show_games()
app._nav("ps5:games")
pump(app, timeout=0.8)
grab_window(app, os.path.join(OUT, f"win_ps5_{LANG}.png"))
app._on_close()
try:
    app.destroy()
except Exception:
    pass

# ------------------------------------------------------------------ USB-Formatierer (eigenständig)
std = usb_standard.StandardApp()
std.attributes("-topmost", True)
p = std.panel
pump(std, lambda: len(p.devices) == len(DEVICES) and not p.busy)
p.dev_combo.current(0)
p._device_changed()
p.fs_var.set("ext4 (Linux)")
p._refresh_options()
p.label_var.set("LINUX")
p._update()
pump(std, timeout=0.6)
grab_window(std, os.path.join(OUT, f"win_std_{LANG}.png"))

# Expertenmodus: GPT mit vier Partitionen
p.expert_var.set(True)
p._expert_changed()
pump(std, timeout=0.3)
p.scheme_combo.current([k for k, _t in sp.SCHEMES].index("gpt"))
p._scheme_changed()
ed = p.editor
ed.parts = [{"bytes": 16 * GB, "fs": "exFAT", "cluster": 0, "label": "DATEN", "ptype": "auto", "active": False},
            {"bytes": 12 * GB, "fs": "NTFS", "cluster": 0, "label": "WINDOWS", "ptype": "auto", "active": False},
            {"bytes": 8 * GB, "fs": "HFS+ (Mac OS Extended)", "cluster": 0, "label": "Mac", "ptype": "auto",
             "active": False},
            {"bytes": 0, "fs": "ext4 (Linux)", "cluster": 0, "label": "linux", "ptype": "auto", "active": False}]
ed.refresh()
p._update()
pump(std, timeout=0.6)
grab_window(std, os.path.join(OUT, f"win_expert_{LANG}.png"))
std.destroy()
print(LANG, "fertig")
