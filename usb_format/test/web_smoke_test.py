#!/usr/bin/env python3
"""Weboberfläche im echten Browser (Playwright/Chromium) gegen den Host-Server: alle Reiter und die wichtigsten Dialoge werden
durchgeklickt; jeder JavaScript-Fehler (pageerror, console.error) lässt den Test scheitern.
Geprüft: Spiele (Suche, Sortierung, Duplikate mit älterer Version), Kopierdialog (Platz/FAT32-Hinweis, "Danach frei"), Spiele prüfen
(Prüfsumme), PKG (Version/Firmware-Hinweise), Werkzeuge, Formatieren, Spielstände, schmale Anzeige ohne Seitwärtsrollen.
Aufruf: python3 test/web_smoke_test.py <server-programm>     (Port 18124; pip install playwright && playwright install chromium)"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("ÜBERSPRUNGEN (playwright nicht installiert)")
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.abspath(sys.argv[1])
PORT = 18124
URL = "http://127.0.0.1:%d" % PORT
W = tempfile.mkdtemp(prefix="websmoke-")
fails = []
errors = []


def ok(t):
    print("OK      " + t)


def bad(t, why=""):
    fails.append(t)
    print("FEHLER  " + t + (" - " + str(why) if why else ""))


def check(cond, good, why_bad="", fail=None):
    """cond wahr: OK-Zeile, sonst Fehler (ok() liefert None - darum kein 'and ... or ...')"""
    if cond:
        ok(good)
    else:
        bad(fail or good, why_bad)


# ---------------------------------------------------------------- Testdaten
os.makedirs(W + "/mnt/usb0/homebrew", exist_ok=True)
os.makedirs(W + "/mnt/usb1", exist_ok=True)
H = W + "/mnt/usb0/homebrew"


def game(path, tid, name, version=None):
    os.makedirs(path + "/sce_sys", exist_ok=True)
    j = {"titleId": tid, "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": name}}}
    if version:
        j["contentVersion"] = version
    open(path + "/sce_sys/param.json", "w").write(json.dumps(j))
    open(path + "/data.bin", "wb").write(os.urandom(50000))


game(H + "/Neu", "PPSA00001", "Alpha Spiel", "01.005.000")
game(H + "/Alt", "PPSA00001", "Alpha Spiel", "01.003.000")      # Duplikat mit älterer Version
game(H + "/Beta", "PPSA00002", "Beta Spiel", "01.000.000")
game(H + "/Gross", "PPSA00003", "Grosses Spiel", "01.000.000")
with open(H + "/Gross/riesig.bin", "wb") as f:
    f.truncate(5 << 30)                                           # 5 GB (dünn besetzt): für FAT32 zu groß
open(H + "/gut.ffpfs", "wb").write(os.urandom(2000000))
import hashlib  # noqa: E402
open(H + "/ohne.ffpfs", "wb").write(os.urandom(1000000))
os.makedirs(W + "/mnt/usb1/.usbfmt-move/deadbeef", exist_ok=True)
open(W + "/mnt/usb1/.usbfmt-move/deadbeef/teil.bin", "wb").write(os.urandom(200000))   # liegengebliebene Teilkopie
open(H + "/gut.ffpfs.sha256", "w").write(hashlib.sha256(open(H + "/gut.ffpfs", "rb").read()).hexdigest() + "\n")


def cnt(entry_id, payload, ctype):
    h = bytearray(0x400)
    h[0:4] = b"\x7fCNT"
    struct.pack_into(">I", h, 0x10, 1)
    struct.pack_into(">I", h, 0x18, 0x100)
    struct.pack_into(">I", h, 0x70, 0x10)
    struct.pack_into(">I", h, 0x74, ctype)
    struct.pack_into(">6I", h, 0x100, entry_id, 0, 0, 0, 0x400, len(payload))
    return bytes(h) + payload


def ps5pkg(name, j):
    meta = cnt(0x2000, ("﻿" + json.dumps(j)).encode(), 0x20)
    fih = bytearray(0x1000)
    fih[0:4] = b"\x7fFIH"
    struct.pack_into("<Q", fih, 0x58, 0x1000)
    open(W + "/mnt/usb0/" + name, "wb").write(bytes(fih) + meta)


loc = {"defaultLanguage": "en-US", "en-US": {"titleName": "Alpha Spiel"}}
ps5pkg("neuer.pkg", {"titleId": "PPSA00001", "contentVersion": "01.010.000", "requiredSystemSoftwareVersion": "0x0900000000000000", "localizedParameters": loc})
ps5pkg("aelter.pkg", {"titleId": "PPSA00001", "contentVersion": "01.001.000", "targetContentVersion": "01.000.000", "requiredSystemSoftwareVersion": "0x0800000000000000", "localizedParameters": loc})
subprocess.run([sys.executable, HERE + "/saves_fixture.py", W], stdout=subprocess.DEVNULL, check=True)
for n in ("a", "b"):
    with open("%s/%s.img" % (W, n), "wb") as f:
        f.truncate(64 << 20)

env = dict(os.environ, USBFMT_IMAGES="%s/a.img:%s/b.img" % (W, W), USBFMT_MOUNTED="1,1", USBFMT_MOUNTROOT=W + "/mnt",
           USBFMT_DATA=W + "/cfg", USBFMT_FW="8.20", USBFMT_TARGET_FAT32="1", USBFMT_SAVES=W + "/home",
           USBFMT_INTERNAL=W + "/data/homebrew", USBFMT_INTERNAL_BASE=W + "/data")
os.makedirs(W + "/data/homebrew", exist_ok=True)
srv = subprocess.Popen([SERVER], env=env, stdout=open(W + "/log", "w"), stderr=subprocess.STDOUT)
try:
    for _ in range(60):
        try:
            urllib.request.urlopen(URL + "/api/info", timeout=1).read()
            break
        except OSError:
            time.sleep(0.2)
    for _ in range(60):
        lib = json.load(urllib.request.urlopen(URL + "/api/library?refresh=1"))
        if not lib["scanning"]:
            break
        time.sleep(0.2)

    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        ctx = browser.new_context(locale="de-DE", viewport={"width": 1100, "height": 1000})
        page = ctx.new_page()
        page.on("pageerror", lambda e: errors.append("pageerror: %s" % e))
        page.on("console", lambda m: errors.append("console.error: %s (%s)" % (m.text, m.location.get("url", ""))) if m.type == "error" else None)
        page.on("response", lambda r: print("HTTP %d %s" % (r.status, r.url)) if r.status >= 400 else None)
        page.goto(URL)
        page.wait_for_selector("#devs .dev", timeout=15000)
        ok("Seite lädt, Laufwerke erscheinen")

        def dev(name):
            page.locator("#devs .dev", has_text=name).first.click()
            page.wait_for_timeout(700)

        def tab(name):
            page.click('[data-tab="%s"]' % name)
            page.wait_for_timeout(500)

        def dlg_text():
            page.wait_for_selector("#dlg[open], #dlg:not([hidden])", timeout=5000)
            return page.inner_text("#dlg")

        def close_result():
            """Ergebnisfenster eines Auftrags abwarten und mit OK schließen (kommt erst, wenn der Auftrag fertig ist)"""
            try:
                page.wait_for_selector("#dlg:not([hidden])", timeout=12000)
            except Exception:
                return
            page.locator("#dlg button:visible").last.click()
            page.wait_for_timeout(400)

        def dlg_click(label):
            page.locator("#dlg button", has_text=label).first.click()
            page.wait_for_timeout(400)

        dev("da0")
        page.wait_for_selector("#content .game", timeout=10000)
        names = page.locator("#content .game").all_inner_texts()
        if len(names) >= 5:
            ok("Spiele-Reiter: %d Einträge" % len(names))
        else:
            bad("Spiele-Reiter", names)

        # Suche
        page.fill('#content input[type=search]', "Beta")
        page.wait_for_timeout(300)
        n = page.locator("#content .game").count()
        check(n == 1, "Suche „Beta“: ein Treffer", n, "Suche")
        page.fill('#content input[type=search]', "zzzzgibtsnicht")
        page.wait_for_timeout(300)
        check("Keine Treffer" in page.inner_text("#content"), "Suche ohne Treffer: Hinweis", "", "Keine Treffer")
        page.fill('#content input[type=search]', "")
        page.wait_for_timeout(300)

        # Sortierung
        page.select_option('#content select', "id")
        page.wait_for_timeout(300)
        first = page.locator("#content .game").first.inner_text()
        ok("Sortierung nach Titel-ID wählbar (%s …)" % first.split("\n")[0])

        # Duplikat mit älterer Version
        txt = page.inner_text("#content")
        check("ältere Version 01.003.000" in txt and "neueste Version 01.005.000" in txt, "Duplikate: ältere Version gekennzeichnet", txt[-400:], "Duplikate")

        # Kopierdialog mit FAT32-Hinweis
        page.click("#content button.tool:has-text('Spiele kopieren')")
        page.wait_for_timeout(300)
        page.locator("#content label.game.pick", has_text="Grosses Spiel").click()
        page.click("#content .pickbar button.solid")
        t = dlg_text()
        for _ in range(30):
            t = page.inner_text("#dlg")
            if "FAT32" in t:
                break
            page.wait_for_timeout(200)
        check("FAT32" in t and "riesig.bin" in t, "Kopierdialog: FAT32-Hinweis nennt die Datei über 4 GB", t, "FAT32-Hinweis")
        dlg_click("Abbrechen")
        page.locator("#content label.game.pick", has_text="Grosses Spiel").click()   # abwählen
        page.locator("#content label.game.pick", has_text="Beta").click()
        page.click("#content .pickbar button.solid")
        for _ in range(30):
            t = page.inner_text("#dlg")
            if "Danach frei" in t:
                break
            page.wait_for_timeout(200)
        check("Danach frei auf dem Ziel" in t, "Kopierdialog: „Danach frei“ wird angezeigt", t, "Danach frei")
        dlg_click("Abbrechen")
        page.click("#content .pickbar button:has-text('Abbrechen')")
        page.wait_for_timeout(300)

        # Spiele prüfen
        page.click("#content button.tool:has-text('Spiele prüfen')")
        page.wait_for_timeout(300)
        page.locator("#content label.game.pick", has_text="gut").click()
        page.click("#content .pickbar button.solid")
        dlg_text()
        dlg_click("Prüfen")
        for _ in range(60):
            if "stimmt" in page.inner_text("body"):
                break
            page.wait_for_timeout(250)
        check("stimmt" in page.inner_text("body"), "Spiele prüfen: Prüfsumme stimmt", page.inner_text("body")[-300:], "Spiele prüfen")
        for _ in range(5):   # Ergebnisfenster schließen
            if page.locator("#dlg.overlay:not([hidden])").count() and page.locator("#dlg button").count():
                page.locator("#dlg button").last.click()
                page.wait_for_timeout(400)

        # Spiel-Einzelheiten (Klick auf die Zeile)
        page.locator("#content .game", has_text="Beta").first.click()
        t = dlg_text()
        check("PPSA00002" in t and "Version" in t and "01.000.000" in t and "Größte Einträge" in t, "Klick auf ein Spiel: Einzelheiten mit Titel-ID, Version und Einträgen", t[:200])
        dlg_click("Schließen")

        # ältere Version vorwählen
        page.locator("#content button:has-text('Ältere Version auswählen')").first.click()
        page.wait_for_timeout(400)
        check("1 ausgewählt" in page.inner_text("#content"), "Duplikate: ältere Version ist vorgewählt (Löschen-Auswahl)", page.inner_text("#content")[:200])
        page.click("#content .pickbar button:has-text('Abbrechen')")
        page.wait_for_timeout(300)

        # Prüfsumme anlegen
        page.click("#content button.tool:has-text('Prüfsumme erstellen')")
        page.wait_for_timeout(300)
        page.locator("#content label.game.pick", has_text="ohne").click()
        page.click("#content .pickbar button.solid")
        dlg_text()
        dlg_click("Anlegen")
        for _ in range(60):
            if os.path.exists(H + "/ohne.ffpfs.sha256"):
                break
            page.wait_for_timeout(250)
        page.wait_for_timeout(500)
        check(os.path.exists(H + "/ohne.ffpfs.sha256") and hashlib.sha256(open(H + "/ohne.ffpfs", "rb").read()).hexdigest() in open(H + "/ohne.ffpfs.sha256").read(),
              "Prüfsumme erstellen: .sha256 neben dem Abbild, Summe stimmt")
        close_result()

        # Teilkopie anzeigen und verwerfen (liegt auf da1)
        dev("da1")
        txt = page.inner_text("#content")
        check("Teilkopie eines abgebrochenen Kopierens" in txt, "Teilkopie wird auf dem Laufwerk gemeldet", txt[:200])
        page.locator("#content button:has-text('Teilkopie verwerfen')").first.click()
        dlg_text()
        dlg_click("Verwerfen")
        for _ in range(60):
            if not os.path.exists(W + "/mnt/usb1/.usbfmt-move"):
                break
            page.wait_for_timeout(250)
        check(not os.path.exists(W + "/mnt/usb1/.usbfmt-move"), "Teilkopie verwerfen: Zwischenordner ist weg")
        close_result()
        dev("da0")

        # PKG-Hinweise
        tab("pkg")
        hints = " | ".join(page.locator(".pkghint").all_inner_texts())
        check("braucht Firmware 9.00, die PS5 hat 8.20" in hints and "neuer als die installierte Version" in hints and "älter als die installierte Version" in hints,
              "PKG-Reiter: Firmware- und Versionshinweise", hints, "PKG-Hinweise")

        # Werkzeuge, Formatieren, Spielstände
        tab("tools")
        check(page.is_visible("#tp-tools"), "Werkzeuge-Reiter öffnet", "", "Werkzeuge")
        if page.locator('[data-tab="format"]').is_visible():
            tab("format")
            check(page.is_visible("#tp-format"), "Formatieren-Reiter öffnet", "", "Formatieren")
        page.locator("#devs .dev", has_text="intern").first.click()
        page.wait_for_timeout(700)
        if page.locator('[data-tab="saves"]').is_visible():
            tab("saves")
            page.wait_for_timeout(1500)
            check(page.locator("#savespane .game, #savespane label").count() > 0, "Spielstände-Reiter zeigt Einträge", page.inner_text("#savespane")[:200], "Spielstände leer")
        else:
            bad("Spielstände-Reiter bei internem Speicher nicht sichtbar")

        # schmale Anzeige: kein Seitwärtsrollen
        page.set_viewport_size({"width": 375, "height": 800})
        page.wait_for_timeout(400)
        wide = page.evaluate("document.documentElement.scrollWidth > document.documentElement.clientWidth + 1")
        check(not wide, "375 px Breite: kein Seitwärtsrollen", "", "Seitwärtsrollen bei 375 px")
        ctx.close()

        # Layout in allen Sprachen und drei Breiten: nichts darf abgeschnitten oder über den Rand hinaus ragen
        FIND_CLIPPED = """() => {
          const bad = [], vw = document.documentElement.clientWidth;
          for (const e of document.querySelectorAll('button, [role=tab], .dev, select, input, label.chk, .stat')) {
            const r = e.getBoundingClientRect();
            if (!r.width || !r.height || e.closest('[hidden]') || getComputedStyle(e).visibility === 'hidden') continue;
            let sc = false;   // in einem waagerecht rollbaren Bereich (Reiterleiste) ist Überstehen gewollt
            for (let p = e.parentElement; p && p !== document.body; p = p.parentElement) { const ox = getComputedStyle(p).overflowX; if (ox === 'auto' || ox === 'scroll') { sc = true; break; } }
            if (sc) continue;
            const name = (e.innerText || e.value || e.className || e.tagName).toString().trim().slice(0, 40);
            if (r.right > vw + 1 || r.left < -1) bad.push('über den Rand: ' + name);
            else if (e.tagName === 'BUTTON' && e.scrollWidth > e.clientWidth + 2) bad.push('Text abgeschnitten: ' + name);
          }
          if (document.documentElement.scrollWidth > vw + 1) bad.push('Seitwärtsrollen');
          return bad;
        }"""
        problems = []
        for loc, code in (("de-DE", "de"), ("en-US", "en"), ("it-IT", "it"), ("es-ES", "es"), ("fr-FR", "fr")):
            for width in (375, 768, 1100):
                c2 = browser.new_context(locale=loc, viewport={"width": width, "height": 900})
                pg = c2.new_page()
                pg.on("pageerror", lambda e: errors.append("pageerror: %s" % e))
                pg.goto(URL)
                pg.wait_for_selector("#devs .dev", timeout=15000)
                for devname in ("da0", "intern"):
                    pg.locator("#devs .dev", has_text=devname).first.click()
                    pg.wait_for_timeout(500)
                    for tb in ("games", "pkg", "tools", "format", "saves"):
                        loc_tab = pg.locator('[data-tab="%s"]' % tb)
                        if not loc_tab.count() or not loc_tab.is_visible():
                            continue
                        loc_tab.click()
                        pg.wait_for_timeout(350)
                        for item in pg.evaluate(FIND_CLIPPED):
                            problems.append("%s %d px %s/%s: %s" % (code, width, devname, tb, item))
                c2.close()
        check(not problems, "Layout: in 5 Sprachen und 3 Breiten nichts abgeschnitten oder über den Rand", "; ".join(sorted(set(problems))[:6]))

        # Bedienung nur mit der Tastatur (wie mit dem Controller): jeder sichtbare Knopf ist per Tab erreichbar, Dialoge fangen den Fokus, Esc schließt
        c3 = browser.new_context(locale="de-DE", viewport={"width": 1100, "height": 1000})
        pg = c3.new_page()
        pg.on("pageerror", lambda e: errors.append("pageerror: %s" % e))
        pg.goto(URL)
        pg.wait_for_selector("#devs .dev", timeout=15000)
        pg.locator("#devs .dev", has_text="da0").first.click()
        pg.wait_for_timeout(700)
        want = pg.evaluate("""() => [...document.querySelectorAll('button, select, input, [role=tab], [role=button], a[href]')]
            .filter(e => { const r = e.getBoundingClientRect(); return r.width && r.height && !e.disabled && !e.closest('[hidden]') && getComputedStyle(e).visibility !== 'hidden'; })
            .length""")
        seen = set()
        pg.evaluate("document.body.focus()")
        for _ in range(want * 2 + 20):
            pg.keyboard.press("Tab")
            info = pg.evaluate("() => { const a = document.activeElement; return a ? a.tagName + '|' + (a.dataset.tab || a.dataset.key || a.id || (a.innerText || a.value || '').slice(0, 30)) : ''; }")
            seen.add(info)
        check(len(seen) >= want * 0.8, "Tastatur: per Tab erreichbar (%d verschiedene Ziele, %d sichtbare Bedienelemente)" % (len(seen), want), sorted(seen)[:8])
        # Dialog nur mit Tastatur: Zeile mit Enter öffnen, Fokus liegt im Dialog, Esc schließt
        pg.locator("#content .game.info").first.focus()
        pg.keyboard.press("Enter")
        pg.wait_for_selector("#dlg:not([hidden])", timeout=5000)
        in_dlg = pg.evaluate("() => document.getElementById('dlg').contains(document.activeElement)")
        check(in_dlg, "Tastatur: Einzelheiten-Dialog mit Enter geöffnet, Fokus liegt im Dialog")
        pg.keyboard.press("Escape")
        pg.wait_for_timeout(400)
        check(pg.locator("#dlg:not([hidden])").count() == 0, "Tastatur: Esc schließt den Dialog")
        c3.close()
        browser.close()

    if errors:
        bad("JavaScript-Fehler im Browser", "; ".join(sorted(set(errors))[:5]))
    else:
        ok("keine JavaScript-Fehler")
    check(srv.poll() is None, "Server läuft weiter", "", "Server abgestürzt")
finally:
    srv.terminate()
    shutil.rmtree(W, ignore_errors=True)

print("Alle Web-Oberflächen-Tests bestanden." if not fails else "%d Fehler" % len(fails))
sys.exit(1 if fails else 0)
