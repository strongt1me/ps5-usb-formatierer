"""Release in einem Ablauf (unter Windows mit WSL): Version anheben, ELF/App/Dump und Helfer bauen, Version prüfen,
README-Bilder erneuern - und erst mit --publish einchecken, taggen, hochladen und das GitHub-Release anlegen.

    python tools/release.py 2.5.12                       vorbereiten (Version, Bauen, Prüfen, Bilder), nichts einchecken
    python tools/release.py 2.5.12 --title "Kurztext" --notes hinweise.md --publish --skip-build --skip-images
                                                         danach: Commit, Tag, Push, Release mit ELF und Kachel-ZIP

Die Windows-Programme und die Linux-Fassungen (Archive und AppImages, x86_64 und aarch64) baut und hängt die CI an
das Release (release: published), danach die SHA256SUMS (checksums.yml). Die eigene PS5 danach mit
tools/ps5_deploy.py update (und tile) auf denselben Stand bringen.
Voraussetzungen: WSL mit dem ps5-payload-sdk (PS5_PAYLOAD_SDK, sonst ~/ps5-build/boilerplate-sdk044/...), .NET-SDK für
den Dump, Python mit Pillow, Microsoft Edge für die Bilder der Weboberfläche, gh (angemeldet) zum Veröffentlichen.
Nie gleichzeitig mit den Host-Tests laufen lassen: beide brauchen Port 8090."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USB = os.path.join(ROOT, "usb_format")
READMES = ["README.md", "README.de.md", "README.it.md", "README.es.md", "README.fr.md"]
REPO = "strongt1me/ps5-usb-formatierer"
LANGS = ("de", "en", "it", "es", "fr")
DEFAULT_SDK = "$HOME/ps5-build/boilerplate-sdk044/.deps/native/ps5-payload-sdk"
ELF_MAX = 8 * 1024 * 1024


def step(text):
    print(f"\n== {text}", flush=True)


def fail(text):
    print(f"FEHLER: {text}", file=sys.stderr)
    sys.exit(1)


def run(cmd, cwd=ROOT, check=True, capture=False):
    r = subprocess.run(cmd, cwd=cwd, text=True, encoding="utf-8", errors="replace",
                       stdout=subprocess.PIPE if capture else None, stderr=subprocess.STDOUT if capture else None)
    if check and r.returncode:
        if capture:
            print(r.stdout[-3000:])
        fail(f"{' '.join(cmd)} endete mit {r.returncode}")
    return r.stdout if capture else r.returncode


def wsl_path(p):
    p = os.path.abspath(p)
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


def wsl(script, check=True, capture=False):
    return run(["wsl", "-e", "bash", "-lc", script], check=check, capture=capture)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    """schreiben - mit kurzen Wiederholungen: Virenscanner/Indexdienst sperren frisch geänderte Dateien manchmal kurz"""
    for attempt in range(6):
        try:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            return
        except OSError:
            if attempt == 5:
                raise
            time.sleep(0.5)


def content_version(v):
    """2.5.12 -> 02.005.012 (contentVersion der App-Kachel)"""
    a, b, c = (int(x) for x in v.split("."))
    return f"{a:02d}.{b:03d}.{c:03d}"


# ------------------------------------------------------------------ Schritte

def preflight(version, publish):
    step("Vorprüfung")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        fail("Version bitte als X.Y.Z angeben")
    branch = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], capture=True).strip()
    if branch != "main":
        fail(f"nicht auf main (sondern {branch})")
    tag = "v" + version
    if run(["git", "tag", "-l", tag], capture=True).strip():
        fail(f"Tag {tag} gibt es schon")
    old = read(os.path.join(ROOT, "VERSION")).strip()
    if old != version:   # beim ersten Lauf muss alles eingecheckt sein - danach liegen die eigenen Änderungen an
        if run(["git", "status", "--porcelain"], capture=True).strip():
            fail("Arbeitskopie nicht sauber - erst einchecken oder wegräumen")
        if tuple(map(int, version.split("."))) <= tuple(map(int, old.split("."))):
            fail(f"{version} ist nicht neuer als {old}")
    if publish and not shutil.which("gh"):
        fail("gh (GitHub CLI) fehlt")
    print(f"main, {old} -> {version}")


def bump(version):
    step(f"Version {version} eintragen")
    write(os.path.join(ROOT, "VERSION"), version + "\n")
    for name in READMES:
        p = os.path.join(ROOT, name)
        t, n = re.subn(r"(img\.shields\.io/badge/[^)\s]*?-)\d+\.\d+\.\d+(-[0-9a-f]{6})", rf"\g<1>{version}\g<2>", read(p))
        if n != 1:
            fail(f"{name}: Versions-Abzeichen nicht eindeutig gefunden ({n})")
        write(p, t)
    p = os.path.join(USB, "app", "sce_sys", "param.json")
    t, n = re.subn(r'("contentVersion"\s*:\s*")[\d.]+(")', rf"\g<1>{content_version(version)}\g<2>", read(p))
    if n != 1:
        fail("param.json: contentVersion nicht gefunden")
    write(p, t)
    print("VERSION, 5 READMEs, app/sce_sys/param.json")


def build(version):
    step("PS5-Formatierer (ELF) und App bauen (WSL)")
    sdk = os.environ.get("PS5_PAYLOAD_SDK", DEFAULT_SDK)
    wsl(f'cd "{wsl_path(USB)}" && make PS5_PAYLOAD_SDK="{sdk}" all app')
    elf = os.path.join(USB, "usb_format.elf")
    data = open(elf, "rb").read()
    if len(data) > ELF_MAX:
        fail(f"usb_format.elf ist {len(data)} Bytes groß (höchstens 8 MB)")
    if version.encode() not in data:
        fail("usb_format.elf enthält die neue Version nicht")
    print(f"usb_format.elf {len(data) / 1e6:.2f} MB")
    step("Dump-Ordner PPSA86920")
    run(["pwsh", "-NoProfile", "-File", os.path.join(USB, "dump", "build.ps1")])
    step("Windows-Helfer")
    run(["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "windows", "build_helper.ps1")])
    step("Version über /api/info prüfen")
    out = wsl(f'cd "{wsl_path(USB)}" && bash test/format_settings_test.sh', capture=True, check=False)
    if f"Version {version}" not in out or "FEHLER" in out:
        print(out[-2000:])
        fail("format_settings_test")
    print(f"/api/info meldet {version}")


def images():
    step("README-Bilder")
    tools = os.path.join(ROOT, "tools", "readme")
    raw = os.path.join(tempfile.gettempdir(), "usbfmt_readme_raw")
    os.makedirs(raw, exist_ok=True)
    wsl(f'bash "{wsl_path(os.path.join(tools, "preview_server.sh"))}"')
    proxy = subprocess.Popen([sys.executable, "-I", os.path.join(tools, "readme_proxy.py")])
    try:
        for _ in range(50):
            try:
                with urllib.request.urlopen("http://127.0.0.1:8095/api/info", timeout=2) as r:
                    print("Testserver:", json.loads(r.read())["version"])
                    break
            except OSError:
                time.sleep(0.2)
        else:
            fail("Vorschau-Proxy antwortet nicht")
        run(["pwsh", "-NoProfile", "-File", os.path.join(tools, "readme_web_shots.ps1"), "-Out", raw])
        for lang in LANGS:
            run([sys.executable, os.path.join(tools, "readme_win_shots.py"), lang, raw])
        run([sys.executable, os.path.join(tools, "readme_images.py"), raw])
    finally:
        proxy.terminate()
        try:
            req = urllib.request.Request("http://127.0.0.1:8090/api/quit", data=b"{}",
                                         headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5).close()
        except OSError:
            pass


def package(version, out):
    """Release-Dateien: usb_format.elf und PPSA86920.zip (Dump-Ordner, passend zur gebauten App)"""
    dump = os.path.join(USB, "dump", "out", "PPSA86920")
    sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()  # noqa: E731
    if sha(os.path.join(dump, "eboot.bin")) != sha(os.path.join(USB, "app", "eboot.bin")):
        fail("Dump-Ordner passt nicht zur gebauten App (eboot.bin) - ohne --skip-build neu bauen")
    if f'"contentVersion": "{content_version(version)}"' not in read(os.path.join(dump, "sce_sys", "param.json")):
        fail("Dump-Ordner hat nicht die neue contentVersion")
    zpath = os.path.join(out, "PPSA86920.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _dirs, files in os.walk(dump):
            for f in sorted(files):
                p = os.path.join(base, f)
                z.write(p, "PPSA86920/" + os.path.relpath(p, dump).replace(os.sep, "/"))
    elf = os.path.join(out, "usb_format.elf")
    shutil.copy2(os.path.join(USB, "usb_format.elf"), elf)
    return [zpath, elf]


# was der Release-Commit enthalten darf - alles andere gehört in einen eigenen Commit davor
RELEASE_PATHS = ("VERSION", "README", "usb_format/app/sce_sys/param.json", "docs/images/")


def publish(version, title, notes, message):
    tag = "v" + version
    out = tempfile.mkdtemp(prefix="usbfmt-release-")
    try:
        # erst alles prüfen, dann einchecken: ein öffentlicher Tag ohne Release ließe sich nicht einfach wiederholen
        step("Release-Dateien prüfen")
        files = package(version, out)
        elf = open(os.path.join(USB, "usb_format.elf"), "rb").read()
        if version.encode() not in elf or len(elf) > ELF_MAX:
            fail("usb_format.elf hat nicht die neue Version oder ist zu groß - ohne --skip-build neu bauen")
        if subprocess.run(["gh", "auth", "status"], capture_output=True).returncode:
            fail("gh ist nicht angemeldet (gh auth login)")
        changed = [line[3:] for line in run(["git", "status", "--porcelain"], capture=True).splitlines() if line]
        stray = [p for p in changed if not p.strip('"').startswith(RELEASE_PATHS)]
        if stray:
            fail("nicht zum Release gehörende Änderungen - erst einchecken oder wegräumen: " + ", ".join(stray))
        print(f"{len(files)} Dateien, ELF {len(elf) / 1e6:.2f} MB, gh angemeldet")
        step("Einchecken, Tag, Push")
        run(["git", "add", "-A"])
        if message:
            run(["git", "commit", "-q", "--allow-empty", "-F", message])
        else:
            run(["git", "commit", "-q", "--allow-empty", "-m", f"{version}: {title}"])
        run(["git", "tag", tag])
        run(["git", "push", "-q", "origin", "main"])
        run(["git", "push", "-q", "origin", tag])
        step("GitHub-Release")
        run(["gh", "release", "create", tag, "--repo", REPO, "--title", f"USB-Formatierer {version}",
             "--notes-file", notes, *files])
    finally:
        shutil.rmtree(out, ignore_errors=True)
    print("Die CI baut jetzt die Windows-Programme und die Linux-Fassungen und hängt sie an das Release.")
    print("PS5 aktualisieren: python tools/ps5_deploy.py update --host <ps5> (danach ggf. tile)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("version", help="neue Version X.Y.Z")
    ap.add_argument("--title", help="Kurztext für den Commit (\"X.Y.Z: <Kurztext>\")")
    ap.add_argument("--message", help="Datei mit der ganzen Commit-Nachricht (statt --title)")
    ap.add_argument("--notes", help="Markdown-Datei mit den Release-Hinweisen (für --publish)")
    ap.add_argument("--skip-build", action="store_true", help="nicht bauen (schon gebaut)")
    ap.add_argument("--skip-images", action="store_true", help="README-Bilder nicht erneuern")
    ap.add_argument("--publish", action="store_true", help="einchecken, taggen, pushen und das Release anlegen")
    a = ap.parse_args()
    if a.publish and not (a.notes and os.path.isfile(a.notes)):
        fail("--publish braucht --notes <datei>")
    if a.publish and not (a.title or a.message):
        fail("--publish braucht --title oder --message")
    preflight(a.version, a.publish)
    bump(a.version)
    if not a.skip_build:
        build(a.version)
    if not a.skip_images:
        images()
    step("Geändert")
    print(run(["git", "status", "--short"], capture=True) or "(nichts)")
    if a.publish:
        publish(a.version, a.title, a.notes, a.message)
    else:
        print("Vorbereitet - nichts eingecheckt. Prüfen, dann mit --title/--notes --publish --skip-build --skip-images "
              "veröffentlichen.")


if __name__ == "__main__":
    main()
