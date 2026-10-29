"""Findet PS5-Konsolen im lokalen Netz.

1. Device Discovery Protocol (DDP) der PS5: UDP-Broadcast an Port 9302, die Konsole antwortet
   mit host-type/host-name - auch im Ruhemodus (Status 620).
2. Rueckfallebene: Suche nach einem bereits laufenden usb_format-Payload (TCP 8090) im eigenen Netz (echte
   Netzmaske; Netze groesser als /22 nur im eigenen /24, sonst dauerte die Suche Minuten).
3. Nichts gefunden: Der Rechner steckt womöglich hinter einem NAT (WSL im Standardmodus, virtuelle Maschine) -
   Broadcasts erreichen das Heimnetz dann nicht, einzelne Adressen aber schon. Dann die üblichen Heimnetze direkt
   fragen: DDP an jede Adresse und TCP 8090.
"""
import ctypes
import http.client
import ipaddress
import json
import select
import socket
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

DDP_PORT = 9302
DDP_SEARCH = b"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:00030010\n"
# Übliche Heimnetze (Router-Voreinstellungen: FritzBox, Speedport, TP-Link, Netgear, Google, Asus, ...) - nur für
# Stufe 3, wenn im eigenen Netz nichts zu finden war
HOME_NETS = ["192.168.178.0/24", "192.168.0.0/24", "192.168.1.0/24", "192.168.2.0/24", "192.168.179.0/24",
             "192.168.188.0/24", "192.168.100.0/24", "192.168.10.0/24", "192.168.50.0/24", "192.168.68.0/24",
             "192.168.86.0/24", "192.168.8.0/24", "10.0.0.0/24", "10.0.1.0/24", "10.1.1.0/24"]


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
    extra = [ip for ip in nets if not ip.startswith(("127.", "169.254.", "0."))]   # alle Netzwerkkarten, nicht nur die der Standard-Route
    for ip in sorted(set(local_ipv4s()) | set(extra)):
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


def ddp_discover(local_ips, timeout=2.0, port=DDP_PORT, targets=None, nets=None, hosts=()):
    """hosts: zusätzlich jede dieser Adressen einzeln fragen - Antworten auf Broadcasts kommen nicht überall an
    (WSL, manche WLAN-Router mit Client-Isolierung für Broadcasts)"""
    found = {}
    if targets is None:   # gerichteter Broadcast je Netz (echte Netzmaske) und der allgemeine
        targets = {"255.255.255.255"} | {str(n.network.broadcast_address) for n in (nets or [])
                                          if n.network.prefixlen < 31}
        if not nets:
            targets |= {ip.rsplit(".", 1)[0] + ".255" for ip in local_ips}
        targets = sorted(targets) + list(hosts)
    targets = list(targets)
    # Linux: Pakete an Adressen ohne Gerät warten in der ARP-Schlange und belegen so lange den Sendepuffer des Sockets -
    # ein Socket fasste nur gut 200 davon und blockierte dann. Daher je Socket höchstens 128 Ziele, nicht blockierend.
    groups = [targets[i:i + 128] for i in range(0, len(targets), 128)]
    socks = []
    try:
        for g in groups:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            socks.append((s, g))
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            s.bind(("", 0))
            s.setblocking(False)
        end = time.time() + timeout
        next_send = 0.0
        while socks and time.time() < end:
            if time.time() >= next_send:  # zweimal senden, UDP kann verloren gehen
                for s, g in socks:
                    for t in g:
                        try:
                            s.sendto(DDP_SEARCH, (t, port))
                        except OSError:   # Puffer voll, Ziel nicht erreichbar
                            pass
                next_send = time.time() + timeout / 2
            try:
                ready, _w, _x = select.select([s for s, _g in socks], [], [], 0.2)
            except (OSError, ValueError):
                break
            for s in ready:
                while True:
                    try:
                        data, (ip, _) = s.recvfrom(2048)
                    except OSError:   # leer, oder Windows meldet ein ICMP "Port nicht erreichbar" (WSAECONNRESET)
                        break
                    info = parse_ddp(data)
                    if info and ip not in local_ips:
                        found[ip] = info
    finally:
        for s, _g in socks:
            s.close()
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
        data = json.loads(body.decode("utf-8", "replace"))
        return isinstance(data, dict) and "devices" in data
    except (OSError, ValueError, http.client.HTTPException):   # z. B. ein anderes Gerät, das kein HTTP spricht
        return False
    finally:
        conn.close()


def scan_formatters(local_ips, port, hosts=None, workers=128, timeout=0.4, tick=None):
    """tick() nach jeder abgefragten Adresse (für den Fortschritt)"""
    if hosts is None:
        hosts = scan_hosts([ipaddress.IPv4Interface(f"{ip}/24") for ip in local_ips])

    def probe(h):
        ok = is_formatter(h, port, timeout)
        if tick:
            tick()
        return h, ok

    with ThreadPoolExecutor(max_workers=workers) as pool:
        return [h for h, ok in pool.map(probe, hosts) if ok]


def _with_progress(report, stage, timeout, total, run):
    """run(tick) ausführen und dabei alle 0,15 s report(prozent, stage) melden: zur Hälfte die verstrichene Zeit der
    Rundfrage (DDP), zur Hälfte die abgefragten Adressen - beides zusammen ergibt 0 bis 95 %"""
    done, lock, stop, t0 = [0], threading.Lock(), threading.Event(), time.time()

    def tick():
        with lock:
            done[0] += 1

    def ticker():
        while not stop.wait(0.15):
            frac = (min(1.0, (time.time() - t0) / timeout) + min(1.0, done[0] / max(1, total))) / 2
            report(int(95 * frac), stage)

    report(0, stage)
    th = threading.Thread(target=ticker, daemon=True)
    th.start()
    try:
        return run(tick)
    finally:
        stop.set()
        th.join()


def home_hosts(nets):
    """Adressen der üblichen Heimnetze, die die normale Suche nicht schon abgedeckt hat"""
    covered = [n.network if n.network.prefixlen >= 22 else ipaddress.IPv4Interface(f"{n.ip}/24").network for n in nets]
    own = {str(n.ip) for n in nets}
    hosts = []
    for cidr in HOME_NETS:
        net = ipaddress.IPv4Network(cidr)
        if not any(net.overlaps(c) for c in covered):
            hosts += [str(h) for h in net.hosts() if str(h) not in own]
    return hosts


def discover(api_port, timeout=2.0, progress=None):
    """Liste von {ip, name, standby, formatter}, PS5 mit laufendem Formatierer zuerst.
    progress(prozent, stufe) aus einem Hintergrundthread; stufe "net" = eigenes Netz, "home" = weitere Heimnetze."""
    report = progress or (lambda pct, stage: None)
    nets = local_networks()
    ips = [str(n.ip) for n in nets]
    own = scan_hosts(nets)

    def search_own(tick):
        with ThreadPoolExecutor(max_workers=2) as pool:
            ddp = pool.submit(ddp_discover, ips, timeout, nets=nets, hosts=own)
            fmt = pool.submit(scan_formatters, ips, api_port, own, tick=tick)
            return ddp.result(), set(fmt.result())

    consoles, formatters = _with_progress(report, "net", timeout, len(own), search_own)
    stage = "net"
    if not consoles and not formatters:   # Stufe 3: hinter einem NAT? Heimnetze direkt fragen
        hosts = home_hosts(nets)
        if hosts:
            stage = "home"

            def search_home(tick):
                with ThreadPoolExecutor(max_workers=2) as pool:
                    ddp = pool.submit(ddp_discover, ips, timeout, targets=hosts)
                    # ~3800 Adressen in ~5 s
                    fmt = pool.submit(scan_formatters, ips, api_port, hosts, 256, 0.3, tick=tick)
                    return ddp.result(), set(fmt.result())

            consoles, formatters = _with_progress(report, stage, timeout, len(hosts), search_home)
    report(97, stage)
    # Die DDP-Antwort sagt nichts über den Formatierer, und die Abfrage von Port 8090 deckt nicht jede Adresse ab (der
    # allgemeine Broadcast erreicht auch fremde Netze): jede gefundene PS5 direkt fragen - sonst würde der Payload
    # womöglich ein zweites Mal geschickt
    for ip in consoles:
        if ip not in formatters and is_formatter(ip, api_port):
            formatters.add(ip)
    result = {}
    for ip, info in consoles.items():
        result[ip] = {"ip": ip, "name": info["name"], "standby": info["standby"], "formatter": ip in formatters}
    for ip in formatters:
        result.setdefault(ip, {"ip": ip, "name": "PS5", "standby": False, "formatter": True})
    report(100, stage)
    return sorted(result.values(), key=lambda c: (not c["formatter"], c["standby"], c["ip"]))
