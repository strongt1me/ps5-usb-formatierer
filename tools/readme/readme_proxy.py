"""Vorschau-Proxy für README-Bilder (Port 8095) vor dem lokalen Testserver (127.0.0.1:8090).
Macht die Testdaten realistisch (Belegung, /mnt/usbN-Pfade, Spielgrößen, gefälschter Stick, ein freier NTFS-Stick)
und lässt nur Lesen sowie /api/settings und /api/diag durch - Aufträge (Formatieren, Löschen, ...) werden abgelehnt.
Auf "/" lässt sich per Abfrage steuern, was nach dem Laden geklickt wird: ?pick=<n>&start=1 (Formatieren-Dialog)."""
import http.server
import json
import re
import urllib.error
import urllib.parse
import urllib.request

UP = "http://127.0.0.1:8090"
PREFIX = re.compile(r"/home/[^/\"]+/usbfmt_preview/mnt/")   # Einhängepunkte des Testservers in der WSL
GB = 1000 ** 3
SIZES = {"PPSA01325": 58.4, "PPSA02433": 37.9, "PPSA06328": 3.2, "PPSA01316": 112.6, "PPSA01284": 61.8,
         "PPSA03117": 1.1}
ALLOWED_POST = ("/api/settings", "/api/diag")


def fix_devices(obj):
    used = sum(SIZES.values()) * GB * 1.15 + 4 * 1.6 * GB   # Spiele (+Doppel) und PKG-Dateien
    for d in obj.get("devices", []):
        if d["name"] == "da0":
            for m in d["mounts"]:
                m["total"] = 511_084_806_144
                m["free"] = int(m["total"] - used)
        elif d["name"] == "da1":   # gefälschter Stick: meldet 124 GB, echt 15,7 GB, nur der echte Bereich formatiert
            d.update(format="exFAT", label="PS5 USB", fake_real=16883122176, part_end=16882073600)
            for m in d["mounts"]:
                m["total"], m["free"] = 16_877_000_000, 16_861_000_000
    obj["devices"].append({"name": "da2", "model": "SanDisk Ultra", "format": "NTFS", "label": "BACKUP",
                           "scheme": "MBR", "size": 61530439680, "sector": 512, "locked": False,
                           "part_end": 61530439680, "fake_real": 0, "mounts": []})
    return obj


IMAGES = {
    "locations": [
        {"dir": "/data/usbfmt-backup", "root": "/data", "device": "", "fs": "", "label": "",
         "free": 1_336_483_221_504, "total": 1_995_416_338_432},
        {"dir": "/mnt/usb0/usbfmt-backup", "root": "/mnt/usb0", "device": "da0", "fs": "exfatfs", "label": "",
         "free": 180_000_000_000, "total": 511_084_806_144},
        {"dir": "/mnt/usb1/usbfmt-backup", "root": "/mnt/usb1", "device": "da1", "fs": "exfatfs", "label": "PS5 USB",
         "free": 16_861_000_000, "total": 16_877_000_000}],
    "images": [
        {"path": "/data/usbfmt-backup/SanDisk_Ultra_SPIELE_2026-10-12_18_40.img.zst",
         "name": "SanDisk_Ultra_SPIELE_2026-10-12_18_40.img.zst", "dir": "/data/usbfmt-backup", "device": "",
         "bytes": 21_400_000_000, "size": 61530439680, "used": 23_100_000_000, "own": True, "model": "SanDisk Ultra",
         "label": "SPIELE", "fs": "exFAT", "created": "2026-10-12 18:40", "sha256": True},
        {"path": "/data/usbfmt-backup/General_UDisk_PS5_USB_2026-10-15_09_12.img.zst",
         "name": "General_UDisk_PS5_USB_2026-10-15_09_12.img.zst", "dir": "/data/usbfmt-backup", "device": "",
         "bytes": 3_900_000_000, "size": 133169152000, "used": 4_200_000_000, "own": True, "model": "General UDisk",
         "label": "PS5 USB", "fs": "exFAT", "created": "2026-10-15 09:12", "sha256": True}],
}


def fix_library(obj):
    for t in obj.get("titles", []):
        t["size"] = int(SIZES.get(t.get("id"), 1.0) * GB)
    for r in obj.get("roots", []):
        if r.get("pkgs"):
            r["pkg_bytes"] = int(4 * 1.6 * GB)
    return obj


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def relay(self, data=None):
        url = urllib.parse.urlparse(self.path)
        if data is not None and url.path not in ALLOWED_POST:
            return self.send(403, "application/json", b'{"error":"Vorschau: Auftraege sind gesperrt"}')
        if url.path == "/api/images":   # gestellte Orte und Sicherungen
            return self.send(200, "application/json", json.dumps(IMAGES).encode("utf-8"))
        req = urllib.request.Request(UP + self.path, data=data,
                                     headers={"Content-Type": self.headers.get("Content-Type", "application/json")})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                code, body, ctype = r.status, r.read(), r.headers.get("Content-Type", "application/octet-stream")
        except urllib.error.HTTPError as e:
            code, body, ctype = e.code, e.read(), e.headers.get("Content-Type", "application/json")
        if "json" in ctype and url.path in ("/api/devices", "/api/library"):
            obj = json.loads(PREFIX.sub("/mnt/", body.decode("utf-8")))
            obj = fix_devices(obj) if url.path == "/api/devices" else fix_library(obj)
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        if url.path == "/":
            q = urllib.parse.parse_qs(url.query)
            pick = q.get("pick", [""])[0]
            js = "<script>setTimeout(function(){"
            if pick:
                js += "var d=document.querySelectorAll('.dev')[%d];if(d)d.click();" % int(pick)
            if q.get("all", ["0"])[0] == "1":
                js += "setTimeout(function(){var m=document.getElementById('more');if(m&&!m.hidden)m.click();},300);"
            if q.get("img", ["0"])[0] == "1":   # Bereich "Sichern & wiederherstellen" öffnen, in die Mitte holen
                js += "setTimeout(function(){document.getElementById('bimage').click();},400);"
            if q.get("start", ["0"])[0] == "1":
                js += "setTimeout(function(){document.getElementById('start').click();},600);"
            js += "},1500)</script>"
            body = body.replace(b"</body>", js.encode() + b"</body>", 1)
        self.send(code, ctype, body)

    def do_GET(self):
        self.relay()

    def do_POST(self):
        self.relay(self.rfile.read(int(self.headers.get("Content-Length", 0))))


http.server.ThreadingHTTPServer(("127.0.0.1", 8095), H).serve_forever()
