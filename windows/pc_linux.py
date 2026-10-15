"""Linux-Teil für USB-Datenträger am PC: den Helfer usbfmt_linux mit Root-Rechten starten (pkexec - das System
fragt nach dem Passwort) und seine Ausgabedatei mitlesen. Gegenstück zur UAC-Abfrage unter Windows (pc_common).

Der Helfer liegt (PyInstaller) in einem Ordner, in den der Benutzer schreiben kann. Damit ihn niemand zwischen Prüfung
und Start austauscht, kopiert ein kurzes Startskript ihn als root in einen eigenen Ordner, den nur root lesen kann,
prüft dort die Prüfsumme und startet erst dann diese Kopie."""
import json
import os
import shutil
import subprocess
import tempfile
import time

from i18n import _, lang

# als root: kopieren, Prüfsumme der Kopie prüfen, starten; Exit 99 = verändert, 98 = Kopie scheiterte.
# Befehle ohne festen Pfad (liegen je nach Verteilung in /bin oder /usr/bin) - pkexec setzt einen sicheren PATH.
_START = r'''
PATH=/usr/sbin:/usr/bin:/sbin:/bin
d=
for base in /var/lib /var/tmp /root /tmp /run; do   # erster root-Ordner, in dem Programme laufen dürfen (kein noexec)
    [ -d "$base" ] || continue
    if command -v findmnt >/dev/null 2>&1 && findmnt -no OPTIONS --target "$base" | grep -qw noexec; then continue; fi
    d=$(mktemp -d "$base/usbfmt-root.XXXXXX") && break
done
[ -n "$d" ] || exit 98
trap 'rm -rf "$d"' EXIT
cp -- "$1" "$d/h" && chmod 700 "$d/h" || exit 98
want=$2
shift 2
if [ -n "$want" ]; then
    got=$(sha256sum "$d/h" | cut -d' ' -f1)
    [ "$got" = "$want" ] || exit 99
fi
"$d/h" "$@"
'''

EXIT_DENIED = (126, 127)   # pkexec: Abfrage abgebrochen bzw. nicht berechtigt


def pkexec_path():
    for p in ("/usr/bin/pkexec", "/bin/pkexec"):
        if os.path.isfile(p):
            return p
    return shutil.which("pkexec")


class LinuxJob:
    """Startet den Helfer mit Root-Rechten (pkexec) und liest seine Ausgabedatei mit"""
    STALL_S = 30 * 60   # so lange ohne neue Ausgabe: Helfer gilt als hängend

    def __init__(self, args, hwnd=None, helper=None, digest=""):
        from pc_common import TEMP_PREFIX, find_helper, helper_digest   # (Kreisbezug: pc_common lädt dieses Modul)
        exe = helper or find_helper()
        if not exe:
            raise RuntimeError(_("Der Helfer {name} fehlt neben dem Programm.", name="usbfmt_linux"))
        digest = digest or helper_digest()
        # eigener Ordner (nur für diesen Benutzer): die Ausgabedatei legt der Helfer als root darin an
        self.tmpdir = tempfile.mkdtemp(prefix=TEMP_PREFIX + "-")
        self.out = os.path.join(self.tmpdir, "auftrag.txt")
        self.cancel_file = os.path.join(self.tmpdir, "abbruch")
        self.cancellable = False
        hargs = ["--out", self.out, "--cancel", self.cancel_file, "--lang", lang()] + list(args)
        if os.geteuid() == 0:   # schon root (z. B. per sudo gestartet): keine Abfrage nötig
            cmd = ["/bin/sh", "-c", _START, "sh", exe, digest] + hargs
        else:
            pk = pkexec_path()
            if not pk:
                self._cleanup()
                raise PermissionError(_("pkexec fehlt – bitte das Paket polkit (pkexec) installieren. Ohne "
                                        "Administratorrechte lässt Linux keine Datenträger bearbeiten."))
            cmd = [pk, "/bin/sh", "-c", _START, "sh", exe, digest] + hargs
        try:
            self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL, close_fds=True)
        except OSError as e:
            self._cleanup()
            raise OSError(_("Helfer konnte nicht gestartet werden: {e}", e=e)) from None

    def _cleanup(self):
        for p in (getattr(self, "out", ""), getattr(self, "cancel_file", "")):
            try:
                os.remove(p)
            except OSError:
                pass
        try:
            os.rmdir(self.tmpdir)
        except OSError:
            pass

    def cancel(self):
        try:
            open(self.cancel_file, "w").close()
        except OSError:
            pass

    def running(self):
        return self.proc.poll() is None

    def _exit_text(self, code):
        if code in EXIT_DENIED:
            return _("Ohne Administratorrechte lässt Linux keine Datenträger bearbeiten.")
        if code == 99:
            return _("Der Helfer wurde verändert – aus Sicherheitsgründen nicht gestartet. Bitte das Programm neu "
                     "herunterladen.")
        if code == 10:
            return _("Der Helfer konnte seine Ergebnisdatei nicht anlegen (Code {code}).", code=code)
        return _("Der Helfer wurde unerwartet beendet (Code {code}).", code=code)

    def follow(self, on_progress):
        """Bis zum Ende mitlesen; liefert (ok, meldung, json)"""
        pos, result, last_change = 0, None, time.time()
        try:
            while result is None:
                alive = self.running()
                try:
                    with open(self.out, "rb") as f:
                        f.seek(pos)
                        data = f.read()
                except OSError:
                    data = b""
                end = data.rfind(b"\n") + 1
                pos += end
                if end:
                    last_change = time.time()
                for line in data[:end].decode("utf-8", "replace").splitlines():
                    parts = line.split("\t")
                    if parts[0] == "P" and len(parts) >= 3:
                        try:
                            on_progress(int(parts[1]), parts[2])
                        except ValueError:
                            pass
                    elif parts[0] == "R" and len(parts) >= 4:
                        try:
                            info = json.loads(parts[3])
                        except ValueError:
                            info = {}
                        result = (parts[1] == "1", parts[2], info if isinstance(info, dict) else {})
                if result is None and not alive:
                    result = (False, self._exit_text(self.proc.returncode), {})
                elif result is None and time.time() - last_change > self.STALL_S:
                    # pkexec läuft als root - beenden darf ihn nur er selbst; abbrechen lassen und aufgeben
                    self.cancel()
                    result = (False, _("Der Helfer meldet seit 30 Minuten nichts mehr – abgebrochen."), {})
                elif result is None:
                    time.sleep(0.3)
        finally:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            self._cleanup()
        return result
