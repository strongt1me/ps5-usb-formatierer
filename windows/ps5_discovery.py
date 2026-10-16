"""Findet PS5-Konsolen im lokalen Netz.

1. Device Discovery Protocol (DDP) der PS5: UDP-Broadcast an Port 9302, die Konsole antwortet
   mit host-type/host-name - auch im Ruhemodus (Status 620).
2. Rueckfallebene: Suche nach einem bereits laufenden usb_format-Payload (TCP 8090) im eigenen Netz (echte
   Netzmaske; Netze groesser als /22 nur im eigenen /24, sonst dauerte die Suche Minuten).
"""
import ctypes
import http.client
import ipaddress
import json
import socket
import subprocess
import sys
import time
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


def local_networks():
    """Eigene IPv4-Netze als ipaddress.IPv4Interface (Adresse mit echter Netzmaske); ohne Angabe /24"""
    nets = {}
    try:
        if sys.platform == "win32":   # GetIpAddrTable: Adresse und Maske je Schnittstelle
            class Row(ctypes.Structure):
                _fields_ = [("addr", ctypes.c_uint32), ("index", ctypes.c_uint32), ("mask", ctypes.c_uint32),
                            ("bcast", ctypes.c_uint32), ("reasm", ctypes.c_uint32), ("unused", ctypes.c_ushort),
                            ("type", ctypes.c_ushort)]
            size = ctypes.c_ulong(0)
            iphlp = ctypes.WinDLL("iphlpapi")
            iphlp.GetIpAddrTable(None, ctypes.byref(size), False)
            buf = ctypes.create_string_buffer(size.value)
            if iphlp.GetIpAddrTable(buf, ctypes.byref(size), False) == 0:
                count = ctypes.c_uint32.from_buffer(buf).value
                rows = (Row * count).from_buffer(buf, 4)
                for r in rows:
                    ip = socket.inet_ntoa(r.addr.to_bytes(4, "little"))
                    mask = socket.inet_ntoa(r.mask.to_bytes(4, "little"))
                    nets[ip] = ipaddress.IPv4Interface(f"{ip}/{mask}")
        else:   # Linux: "2: eth0    inet 192.168.1.5/23 brd ..."
            out = subprocess.run(["ip", "-4", "-o", "addr", "show"], capture_output=True, text=True, timeout=2).stdout
            for line in out.splitlines():
                parts = line.split()
                if "inet" in parts:
                    iface = ipaddress.IPv4Interface(parts[parts.index("inet") + 1])
                    nets[str(iface.ip)] = iface
    except (OSError, ValueError, AttributeError, subprocess.SubprocessError):
        pass
    result = []
    for ip in local_ipv4s():
        result.append(nets.get(ip) or ipaddress.IPv4Interface(f"{ip}/24"))
    return result


def scan_hosts(nets):
    """Zu durchsuchende Adressen: das ganze Netz bis /22, darüber nur das eigene /24"""
    own = {str(n.ip) for n in nets}
    hosts = []
    for n in nets:
        net = n.network if n.network.prefixlen >= 22 else ipaddress.IPv4Interface(f"{n.ip}/24").network
        hosts += [str(h) for h in net.hosts() if str(h) not in own and str(h) not in hosts]
    return hosts


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


def ddp_discover(local_ips, timeout=2.0, port=DDP_PORT, targets=None, nets=None):
    found = {}
    if targets is None:   # gerichteter Broadcast je Netz (echte Netzmaske) und der allgemeine
        targets = {"255.255.255.255"} | {str(n.network.broadcast_address) for n in (nets or [])
                                          if n.network.prefixlen < 31}
        if not nets:
            targets |= {ip.rsplit(".", 1)[0] + ".255" for ip in local_ips}
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
    # direkt, nicht über urllib: kein System-Proxy (der fände Geräte im eigenen Netz nicht), keinen Umleitungen folgen
    # (ein fremdes Gerät könnte auf Port 9021 der PS5 verweisen - den ELF-Loader) und nur begrenzt lesen (ein Gerät
    # mit endlosem Datenstrom hielte die Suche sonst für immer an)
    conn = http.client.HTTPConnection(ip, port, timeout=2)
    try:
        conn.request("GET", "/api/devices")
        r = conn.getresponse()
        if r.status != 200:
            return False
        body, deadline = b"", time.time() + 5   # Gesamtfrist, auch bei tröpfelnder Antwort
        while True:
            chunk = r.read1(65536)
            if not chunk:
                break
            body += chunk
            if len(body) > 256 * 1024 or time.time() > deadline:
                return False
        data = json.loads(body.decode("utf-8"))
        return isinstance(data, dict) and "devices" in data
    except (OSError, ValueError, http.client.HTTPException):   # z. B. ein anderes Gerät, das kein HTTP spricht
        return False
    finally:
        conn.close()


def scan_formatters(local_ips, port, hosts=None):
    if hosts is None:
        hosts = scan_hosts([ipaddress.IPv4Interface(f"{ip}/24") for ip in local_ips])
    with ThreadPoolExecutor(max_workers=128) as pool:
        results = pool.map(lambda h: (h, is_formatter(h, port)), hosts)
        return [h for h, ok in results if ok]


def discover(api_port, timeout=2.0):
    """Liste von {ip, name, standby, formatter}, PS5 mit laufendem Formatierer zuerst."""
    nets = local_networks()
    ips = [str(n.ip) for n in nets]
    with ThreadPoolExecutor(max_workers=2) as pool:
        ddp = pool.submit(ddp_discover, ips, timeout, nets=nets)
        fmt = pool.submit(scan_formatters, ips, api_port, scan_hosts(nets))
        consoles, formatters = ddp.result(), set(fmt.result())
    result = {}
    for ip, info in consoles.items():
        result[ip] = {"ip": ip, "name": info["name"], "standby": info["standby"], "formatter": ip in formatters}
    for ip in formatters:
        result.setdefault(ip, {"ip": ip, "name": "PS5", "standby": False, "formatter": True})
    return sorted(result.values(), key=lambda c: (not c["formatter"], c["standby"], c["ip"]))
