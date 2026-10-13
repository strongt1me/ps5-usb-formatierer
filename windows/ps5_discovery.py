"""Findet PS5-Konsolen im lokalen Netz.

1. Device Discovery Protocol (DDP) der PS5: UDP-Broadcast an Port 9302, die Konsole antwortet
   mit host-type/host-name - auch im Ruhemodus (Status 620).
2. Rueckfallebene: Suche nach einem bereits laufenden usb_format-Payload (TCP 8090) im /24-Netz.
"""
import json
import socket
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

DDP_PORT = 9302
DDP_SEARCH = b"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n"


def local_ipv4s():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    try:  # Adresse der Standard-Route; connect() auf UDP sendet nichts
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            ips.add(s.getsockname()[0])
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith(("127.", "169.254.", "0.")))


def parse_ddp(data):
    lines = data.decode("utf-8", "replace").replace("\r", "").split("\n")
    if not lines or not lines[0].startswith("HTTP/1.1"):
        return None
    fields = {}
    for line in lines[1:]:
        key, sep, val = line.partition(":")
        if sep:
            fields[key.strip().lower()] = val.strip()
    if fields.get("host-type", "").upper() != "PS5":
        return None
    return {"name": fields.get("host-name") or "PS5", "standby": " 620 " in lines[0] + " "}


def ddp_discover(local_ips, timeout=2.0, port=DDP_PORT, targets=None):
    found = {}
    if targets is None:
        targets = {"255.255.255.255"} | {ip.rsplit(".", 1)[0] + ".255" for ip in local_ips}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.bind(("", 0))
        s.settimeout(0.2)
        end = time.time() + timeout
        next_send = 0.0
        while time.time() < end:
            if time.time() >= next_send:  # zweimal senden, UDP kann verloren gehen
                for t in targets:
                    try:
                        s.sendto(DDP_SEARCH, (t, port))
                    except OSError:
                        pass
                next_send = time.time() + timeout / 2
            try:
                data, (ip, _) = s.recvfrom(2048)
            except (socket.timeout, OSError):
                continue
            info = parse_ddp(data)
            if info and ip not in local_ips:
                found[ip] = info
    return found


def is_formatter(ip, port, timeout=0.4):
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            pass
    except OSError:
        return False
    try:
        with urllib.request.urlopen(f"http://{ip}:{port}/api/devices", timeout=2) as r:
            return "devices" in json.loads(r.read().decode("utf-8"))
    except (OSError, ValueError):
        return False


def scan_formatters(local_ips, port, hosts=None):
    if hosts is None:
        hosts = []
        for subnet in {ip.rsplit(".", 1)[0] for ip in local_ips}:
            hosts += [f"{subnet}.{i}" for i in range(1, 255) if f"{subnet}.{i}" not in local_ips]
    with ThreadPoolExecutor(max_workers=128) as pool:
        results = pool.map(lambda h: (h, is_formatter(h, port)), hosts)
        return [h for h, ok in results if ok]


def discover(api_port, timeout=2.0):
    """Liste von {ip, name, standby, formatter}, PS5 mit laufendem Formatierer zuerst."""
    ips = local_ipv4s()
    with ThreadPoolExecutor(max_workers=2) as pool:
        ddp = pool.submit(ddp_discover, ips, timeout)
        fmt = pool.submit(scan_formatters, ips, api_port)
        consoles, formatters = ddp.result(), set(fmt.result())
    result = {}
    for ip, info in consoles.items():
        result[ip] = {"ip": ip, "name": info["name"], "standby": info["standby"], "formatter": ip in formatters}
    for ip in formatters:
        result.setdefault(ip, {"ip": ip, "name": "PS5", "standby": False, "formatter": True})
    return sorted(result.values(), key=lambda c: (not c["formatter"], c["standby"], c["ip"]))
