"""Die PS5 auf den Stand dieses Projekts bringen - nach einem Release (oder zum Testen eines frischen Baus):

    python tools/ps5_deploy.py status            nur lesen: Formatierer, Kachel, laufende Spiele, App-Ordner-Vergleich
    python tools/ps5_deploy.py update            App-Ordner (vorher gesichert) und Formatierer auf diesen Stand bringen
    python tools/ps5_deploy.py update --dry-run  nur zeigen, was update täte
    python tools/ps5_deploy.py tile              App-Kachel neu anmelden (fragt vorher; --yes ohne Frage)

PS5: --host 192.168.x.y oder Umgebungsvariable USBFMT_PS5_HOST. Gebraucht werden der FTP-Server (Port 2121) und für den
Formatierer der ELF-Loader (9021, wird nur zum Senden geöffnet, nie vorab abgetastet), dazu - wenn er läuft - das
Kernel-Log (klogsrv, 3232), das während update mitgeschnitten wird.

update in dieser Reihenfolge:
  1. Vorprüfung: gebaute ELF und App-Ordner (usb_format/dump/out/PPSA86920) haben die Version aus VERSION, laufen auf der
     PS5 ein Spiel/eine App (/mnt/sandbox) oder ein Auftrag des Formatierers: Abbruch (--ignore-running übergeht nur
     das Spiel, nie einen Auftrag). Antwortet Port 8090, aber nicht die API: Abbruch (Zustand unklar).
  2. App-Ordner /data/homebrew/PPSA86920: jede Datei, die anders ist, erst von der PS5 sichern, dann hochladen und
     zurücklesen. Scheitert FTP mittendrin: Abbruch mit Liste der schon ersetzten Dateien und dem Sicherungsordner. Sicherung und Kernel-Log unter ~/.usbfmt-ps5/<zeit>/ (oder --backup <ordner>).
  3. Formatierer: läuft er, per /api/quit beenden; die neue ELF an den ELF-Loader (weist er die Verbindung ab, wird bis
     --wait-loader Sekunden lang nachgefragt - ein abgewiesener Versuch erreicht ihn nicht, ein angenommener bekommt
     sofort die ELF); warten, bis /api/info die neue Version meldet. Läuft dieselbe Version schon, bleibt er (--resend-elf sendet trotzdem).
  4. Kachel: zeigt, ob sie neu angemeldet werden muss (dann "tile").
Exit-Code: 0 = fertig, 1 = Fehler/Abbruch, 2 = fertig, aber das Kernel-Log zeigt Auffälliges.
Nur Python-Standardbibliothek."""
import argparse
import ftplib
import hashlib
import io
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USB = os.path.join(ROOT, "usb_format")
APP_LOCAL = os.path.join(USB, "dump", "out", "PPSA86920")
APP_REMOTE = "/data/homebrew/PPSA86920"
OWN_TITLE = "PPSA86920"
FTP_PORT, API_PORT, LOADER_PORT, KLOG_PORT = 2121, 8090, 9021, 3232


def say(text):
    print(text, flush=True)


def fail(text):
    print(f"FEHLER: {text}", file=sys.stderr, flush=True)
    sys.exit(1)


def read_version():
    with open(os.path.join(ROOT, "VERSION"), encoding="ascii") as f:
        return f.read().strip()


def content_version(v):
    a, b, c = (int(x) for x in v.split("."))
    return f"{a:02d}.{b:03d}.{c:03d}"


def sha(data):
    return hashlib.sha256(data).hexdigest()


# ------------------------------------------------------------------ PS5-Verbindungen

def port_open(host, port, timeout=2):
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False


def api(host, path, body=None, timeout=10):
    """JSON vom Formatierer; None, wenn er nicht antwortet"""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f"http://{host}:{API_PORT}{path}", data=data,
                                 headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        try:
            return {"error": json.loads(e.read().decode("utf-8", "replace")).get("error"), "status": e.code}
        except (ValueError, OSError):
            return {"error": f"HTTP {e.code}", "status": e.code}
    except (OSError, ValueError):
        return None


def ftp_connect(host):
    f = ftplib.FTP()
    try:
        f.connect(host, FTP_PORT, timeout=30)
        f.login()
    except ftplib.all_errors as e:
        fail(f"FTP auf {host}:{FTP_PORT} nicht erreichbar ({e}) - läuft der FTP-Server des Jailbreaks?")
    return f


class FtpTrouble(Exception):
    """FTP-Fehler, der nicht "Datei fehlt" heißt (Zeitüberschreitung, Verbindung weg)"""


def ftp_get(f, path):
    """Inhalt der Datei; None nur, wenn sie fehlt (550). Andere Fehler: FtpTrouble - sonst würde eine nur kurz nicht
    lesbare Datei als "fehlt" gelten und ohne Sicherung überschrieben."""
    buf = io.BytesIO()
    try:
        f.retrbinary(f"RETR {path}", buf.write, blocksize=1 << 20)
    except ftplib.error_perm as e:
        if str(e).startswith("550"):
            return None
        raise FtpTrouble(f"{path}: {e}") from None
    except ftplib.all_errors as e:
        raise FtpTrouble(f"{path}: {e}") from None
    return buf.getvalue()


def running_titles(f):
    """Titel-IDs laufender Spiele/Apps (Sandboxen PPSA…/CUSA…, außer dem eigenen) - die PS5 spiegelt eingebundene
    USB-Laufwerke in ihre Sandbox; währenddessen nichts umstellen"""
    names = []
    try:
        f.retrlines("NLST /mnt/sandbox", names.append)
    except ftplib.all_errors as e:
        fail(f"/mnt/sandbox nicht lesbar ({e})")
    ids = {n.rsplit("/", 1)[-1][:9] for n in names}
    return sorted(i for i in ids if re.fullmatch(r"(PPSA|CUSA)\d{5}", i) and i != OWN_TITLE)


# ------------------------------------------------------------------ lokaler Stand

def local_app():
    """{relativer Pfad: Inhalt} des gebauten App-Ordners"""
    if not os.path.isdir(APP_LOCAL):
        fail(f"{APP_LOCAL} fehlt - erst bauen (usb_format/dump/build.ps1 bzw. tools/release.py)")
    files = {}
    for base, _dirs, names in os.walk(APP_LOCAL):
        for n in sorted(names):
            p = os.path.join(base, n)
            with open(p, "rb") as fh:
                files[os.path.relpath(p, APP_LOCAL).replace(os.sep, "/")] = fh.read()
    return files


def check_local(version):
    """gebaute ELF und App-Ordner passen zu VERSION - sonst landete ein alter Bau auf der PS5"""
    elf = os.path.join(USB, "usb_format.elf")
    if not os.path.isfile(elf):
        fail("usb_format/usb_format.elf fehlt - erst bauen")
    with open(elf, "rb") as fh:
        data = fh.read()
    if version.encode() not in data:
        fail(f"usb_format.elf enthält Version {version} nicht - neu bauen")
    files = local_app()
    param = files.get("sce_sys/param.json", b"").decode("utf-8", "replace")
    if f'"contentVersion": "{content_version(version)}"' not in param:
        fail(f"App-Ordner hat nicht contentVersion {content_version(version)} - neu bauen")
    app_eboot = os.path.join(USB, "app", "eboot.bin")
    if os.path.isfile(app_eboot):
        with open(app_eboot, "rb") as fh:
            if sha(fh.read()) != sha(files.get("eboot.bin", b"")):
                fail("App-Ordner passt nicht zur gebauten App (eboot.bin) - usb_format/dump/build.ps1 erneut")
    return data, files


def compare(f, files):
    """[(pfad, inhalt, entfernter inhalt oder None = fehlt)]"""
    try:
        return [(rel, data, ftp_get(f, f"{APP_REMOTE}/{rel}")) for rel, data in files.items()]
    except FtpTrouble as e:
        fail(f"App-Ordner auf der PS5 nicht lesbar ({e}) - nichts geändert")


# ------------------------------------------------------------------ Kernel-Log

class Klog:
    """Kernel-Log der PS5 (klogsrv) während des Updates mitschneiden - zur Diagnose, falls die PS5 aus dem Tritt kommt"""

    def __init__(self, host, path):
        self.path, self.stop, self.size = path, threading.Event(), 0
        try:
            self.sock = socket.create_connection((host, KLOG_PORT), timeout=5)
        except OSError:
            self.sock = None
            return
        self.sock.settimeout(1)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        with open(self.path, "wb") as out:
            while not self.stop.is_set():
                try:
                    d = self.sock.recv(65536)
                    if not d:
                        break
                    out.write(d)
                    self.size += len(d)
                except socket.timeout:
                    continue
                except OSError:
                    break

    def finish(self, linger=5):
        if not self.sock:
            return None
        time.sleep(linger)
        self.stop.set()
        self.thread.join(5)
        self.sock.close()
        with open(self.path, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
        return [line for line in text.splitlines() if re.search(r"panic|fatal|SIGSEGV|abort", line, re.I)]


# ------------------------------------------------------------------ Befehle

def cmd_status(host):
    version = read_version()
    info = api(host, "/api/info")
    say(f"Projekt:      {version}")
    say(f"Formatierer:  {info.get('version') if info else 'läuft nicht (Port 8090 zu)'}")
    tile = api(host, "/api/tile") if info else None
    if tile and "registered" in tile:
        say(f"Kachel:       angemeldet {tile.get('registered')}, App-Ordner {tile.get('folder')}")
    f = ftp_connect(host)
    titles = running_titles(f)
    say(f"Spiele/Apps:  {', '.join(titles) if titles else 'keine laufen'}")
    diff = [(rel, data, remote) for rel, data, remote in compare(f, local_app()) if remote != data]
    f.quit()
    say("App-Ordner:   " + ("gleich wie der Bau" if not diff else
                            ", ".join(f"{rel} ({'fehlt' if r is None else 'anders'})" for rel, _d, r in diff)))


def loader_connect(host, wait):
    """Verbindung zum ELF-Loader; weist er ab (läuft noch nicht), alle 5 s erneut bis `wait` Sekunden. Kein Abtasten:
    die erste angenommene Verbindung ist die, über die die ELF geht."""
    end, told = time.time() + wait, False
    while True:
        try:
            return socket.create_connection((host, LOADER_PORT), timeout=10)
        except OSError as e:
            if time.time() >= end:
                raise
            if not told:
                say(f"  ELF-Loader (Port {LOADER_PORT}) nimmt noch nichts an ({e}) - Jailbreak/ELF-Loader auf der PS5 "
                    f"starten; ich frage bis zu {wait} s lang nach")
                told = True
            time.sleep(5)


def cmd_update(host, dry_run, ignore_running, resend, backup, wait_loader=300):
    version = read_version()
    elf, files = check_local(version)
    say(f"Stand {version}: usb_format.elf {len(elf) / 1e6:.2f} MB, App-Ordner {len(files)} Dateien")
    f = ftp_connect(host)
    titles = running_titles(f)
    say(f"Spiele/Apps: {', '.join(titles) if titles else 'keine laufen'}")
    if titles and not ignore_running:
        f.quit()
        fail(f"auf der PS5 läuft {', '.join(titles)} - erst beenden (oder --ignore-running, wenn es sicher ist)")
    info = api(host, "/api/info")
    if info is None and port_open(host, API_PORT):   # läuft, antwortet aber nicht: nicht blind ersetzen
        f.quit()
        fail("Port 8090 ist offen, der Formatierer antwortet aber nicht - Zustand unklar, nichts geändert")
    st = api(host, "/api/status") if info else None
    if info and (st is None or st.get("busy")):
        f.quit()
        fail("der Formatierer arbeitet gerade an einem Auftrag (oder meldet seinen Zustand nicht) - abwarten")
    diff = [(rel, data, remote) for rel, data, remote in compare(f, files) if remote != data]
    send_elf = not info or info.get("version") != version or resend
    say(f"Formatierer: {info.get('version') if info else 'läuft nicht'} -> "
        + (f"{version} (ELF senden)" if send_elf else "bleibt (gleiche Version)"))
    say("App-Ordner: " + (", ".join(rel for rel, _d, _r in diff) if diff else "schon aktuell"))
    if dry_run:
        f.quit()
        say("Probelauf - nichts geändert.")
        return
    os.makedirs(backup, exist_ok=True)
    klog = Klog(host, os.path.join(backup, "kernel.log"))
    say(f"Sicherung und Kernel-Log: {backup}" + ("" if klog.sock else " (Kernel-Log nicht erreichbar)"))

    for rel, data, remote in diff:   # erst alles sichern, dann schreiben und zurücklesen
        if remote is not None:
            dst = os.path.join(backup, "PPSA86920", *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as fh:
                fh.write(remote)
    done = []
    for rel, data, _remote in diff:
        try:
            sub = os.path.dirname(rel)
            if sub:
                try:
                    f.mkd(f"{APP_REMOTE}/{sub}")
                except ftplib.error_perm:
                    pass
            f.storbinary(f"STOR {APP_REMOTE}/{rel}", io.BytesIO(data), blocksize=1 << 20)
            same = ftp_get(f, f"{APP_REMOTE}/{rel}") == data
        except (FtpTrouble, *ftplib.all_errors) as e:
            same, err = None, e
        if not same:
            try:
                f.close()
            except OSError:
                pass
            klog.finish(0)
            why = f"FTP-Fehler: {err}" if same is None else "zurückgelesen anders als gesendet"
            fail(f"{rel}: {why}. Schon ersetzt: {', '.join(done) or 'nichts'}; {rel} ist womöglich unvollständig. "
                 f"Die bisherigen Dateien liegen in {os.path.join(backup, 'PPSA86920')} - erneut versuchen oder per "
                 "FTP zurückspielen. Der Formatierer wurde nicht angefasst.")
        done.append(rel)
        say(f"  {rel}: hochgeladen, zurückgelesen identisch")
    f.quit()

    if send_elf:
        if info:
            r = api(host, "/api/quit", {})
            if not r or r.get("error"):
                klog.finish(0)
                fail(f"Formatierer lässt sich nicht beenden: {r}")
            for _ in range(30):
                if not port_open(host, API_PORT):
                    break
                time.sleep(0.5)
            else:
                klog.finish(0)
                fail("der alte Formatierer läuft noch")
            time.sleep(1)
        try:
            s = loader_connect(host, wait_loader)
        except OSError as e:
            klog.finish(0)
            fail(f"ELF-Loader (Port {LOADER_PORT}) nimmt nichts an ({e}) - Jailbreak/ELF-Loader auf der PS5 starten. "
                 + ("Der App-Ordner ist schon aktualisiert; " if diff else "") + "derselbe Befehl sendet danach nur noch die ELF.")
        try:   # die 10 s galten fürs Verbinden; Senden darf bei schwachem WLAN dauern (bis 8 MB)
            s.settimeout(180)
            s.sendall(elf)
            s.shutdown(socket.SHUT_WR)
        except OSError as e:
            klog.finish(0)
            fail(f"Senden an den ELF-Loader abgebrochen ({e}) - die PS5 hat womöglich eine unvollständige ELF bekommen; "
                 "ELF-Loader neu starten und erneut versuchen")
        finally:
            s.close()
        say(f"  ELF gesendet ({len(elf)} Bytes)")
        new = None
        for _ in range(60):
            time.sleep(0.5)
            new = api(host, "/api/info", timeout=3)
            if new:
                break
        if not new or new.get("version") != version:
            klog.finish(0)
            fail(f"der neue Formatierer meldet {new.get('version') if new else 'nichts'} statt {version}")
        say(f"  Formatierer läuft: {new.get('version')}")

    bad = klog.finish()
    if bad:
        say(f"Kernel-Log: {len(bad)} auffällige Zeile(n), z. B.: {bad[0][:160]} - bitte {backup} ansehen")
    elif bad is not None:
        say("Kernel-Log: unauffällig")
    tile = api(host, "/api/tile")
    if tile and tile.get("registered") and tile.get("registered") != tile.get("folder"):
        say(f"Kachel: angemeldet {tile['registered']}, App-Ordner {tile['folder']} - "
            "neu anmelden mit: python tools/ps5_deploy.py tile")
    elif tile and tile.get("registered"):
        say(f"Kachel: {tile['registered']} - aktuell")
    broken = selftest(host, version)
    if broken:
        say(f"Selbsttest: {broken} Fehler - der Formatierer läuft, aber nicht alles antwortet wie erwartet (Exit-Code 3).")
    else:
        say("Selbsttest: alles antwortet.")
    say("Fertig." if not bad else "Fertig - aber das Kernel-Log ist auffällig (Exit-Code 2).")
    if bad:
        sys.exit(2)
    if broken:
        sys.exit(3)


def selftest(host, version):
    """Nach dem Update alle Lesewege des Formatierers einmal abfragen (nichts wird verändert): Zahl der Fehler"""
    say("Selbsttest (nur Lesen):")
    bad = 0

    def check(name, ok, detail=""):
        nonlocal bad
        bad += 0 if ok else 1
        say(f"  {'ok     ' if ok else 'FEHLER '} {name}" + (f" - {detail}" if detail else ""))

    info = api(host, "/api/info")
    check("/api/info", bool(info) and info.get("version") == version, str(info and info.get("version")))
    st = api(host, "/api/status")
    check("/api/status", bool(st) and "busy" in st and not st.get("busy"), "bereit" if st and not st.get("busy") else str(st))
    dv = api(host, "/api/devices")
    check("/api/devices", bool(dv) and isinstance(dv.get("devices"), list), f"{len(dv.get('devices', []))} Laufwerk(e)" if dv else "keine Antwort")
    lib = api(host, "/api/library?refresh=1", timeout=15)
    for _ in range(90):   # die Suche über alle Laufwerke läuft im Hintergrund
        if lib and not lib.get("scanning"):
            break
        time.sleep(1)
        lib = api(host, "/api/library", timeout=15)
    good = bool(lib) and not lib.get("scanning") and all(k in lib for k in ("titles", "pkgfiles", "duplicates", "roots", "fw"))
    check("/api/library", good, (f"{len(lib['titles'])} Spiele, {len(lib['pkgfiles'])} PKG, Firmware {lib.get('fw') or '?'}" if good else "unvollständig oder Suche nicht fertig"))
    if lib and lib.get("titles"):
        check("Spielversionen und Größen im Datensatz", all("version" in t for t in lib["titles"]))
    hist = api(host, "/api/history")
    check("/api/history", hist is not None and "entries" in hist, f"{len(hist.get('entries', []))} Einträge" if hist and "entries" in hist else str(hist))
    check("/api/tile", bool(api(host, "/api/tile")) and "registered" in api(host, "/api/tile"))
    sv = api(host, "/api/saves")
    check("/api/saves", sv is not None and ("saves" in sv or sv.get("status") in (400, 404)), f"{len(sv.get('saves', []))} Spielstände" if sv and "saves" in sv else str(sv))
    try:
        with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(f"http://{host}:{API_PORT}/", timeout=15) as r:
            page = r.read().decode("utf-8", "replace")
        check("Weboberfläche (HTML)", "I18N" in page and "</html>" in page and len(page) > 100000, f"{len(page) // 1024} KB")
    except OSError as e:
        check("Weboberfläche (HTML)", False, str(e))
    return bad


def cmd_tile(host, yes):
    tile = api(host, "/api/tile")
    if not tile or "registered" not in tile:
        fail("der Formatierer antwortet nicht oder kennt keine Kachel (ShadowMountPlus?)")
    say(f"Kachel: angemeldet {tile.get('registered')}, App-Ordner {tile.get('folder')}")
    if tile.get("registered") == tile.get("folder"):
        say("schon aktuell - nichts zu tun")
        return
    if not yes and input("Kachel jetzt neu anmelden? Sie steht danach womöglich an anderer Stelle [j/N] ").strip().lower() \
            not in ("j", "ja", "y", "yes"):
        say("nicht angemeldet")
        return
    r = api(host, "/api/tile", {})
    if not r or r.get("error"):
        fail(f"abgelehnt: {r.get('error') if r else 'keine Antwort'}")
    for _ in range(120):
        st = api(host, "/api/status")
        if st and not st.get("busy"):
            break
        time.sleep(1)
    say(f"{'ok' if st and st.get('ok') else 'FEHLER'}: {st.get('message') if st else 'keine Antwort'}")
    if not (st and st.get("ok")):
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=("status", "update", "tile"))
    ap.add_argument("--host", default=os.environ.get("USBFMT_PS5_HOST", ""), help="Adresse der PS5")
    ap.add_argument("--dry-run", action="store_true", help="update: nur zeigen")
    ap.add_argument("--ignore-running", action="store_true", help="update: trotz laufendem Spiel/laufender App")
    ap.add_argument("--resend-elf", action="store_true", help="update: ELF auch bei gleicher Version senden")
    ap.add_argument("--backup", help="update: Ordner für Sicherung und Kernel-Log")
    ap.add_argument("--wait-loader", type=int, default=300, metavar="SEK",
                    help="update: so lange nachfragen, wenn der ELF-Loader noch nicht annimmt (0 = sofort aufgeben)")
    ap.add_argument("--yes", action="store_true", help="tile: ohne Rückfrage")
    a = ap.parse_args()
    if not a.host:
        fail("PS5-Adresse fehlt: --host <ip> oder USBFMT_PS5_HOST setzen")
    if a.command == "status":
        cmd_status(a.host)
    elif a.command == "update":
        backup = a.backup or os.path.join(os.path.expanduser("~"), ".usbfmt-ps5", time.strftime("%Y%m%d-%H%M%S"))
        cmd_update(a.host, a.dry_run, a.ignore_running, a.resend_elf, backup, max(0, a.wait_loader))
    else:
        cmd_tile(a.host, a.yes)


if __name__ == "__main__":
    main()
