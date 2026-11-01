# Änderungen

Alle Versionen mit ihren Release-Hinweisen, die neueste oben. Die Dateien zu jeder Version liegen unter [Releases](https://github.com/strongt1me/ps5-usb-formatierer/releases).

## [2.6.64](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.64) – 2026-10-31

- Weboberfläche: Der Reiterwechsel mit L1/R1 ist wieder entfernt – er funktionierte auf der PS5 nicht. L2/R2 scrollen weiterhin.

## [2.6.63](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.63) – 2026-10-31

- Weboberfläche: Der Knopf oben rechts heißt jetzt „Formatierer ausschalten“ statt „Formatierer stoppen“, ebenso der Dialog („Ausschalten“ / „Weiterlaufen lassen“) und die Meldung danach – in allen Sprachen.
- Hilfe: Der Hinweis auf den Reiterwechsel mit L1/R1 ist entfernt.

## [2.6.62](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.62) – 2026-10-31

- Eingehängte Spiele: Kennzeichen „eingehängt“ in der Spieleliste, dazu in der Weboberfläche der Filter „Nur eingehängte“ und im Programm die Sortierung „Eingehängt“.
- Kachel neu anmelden: Scheitert es an einem eingehängten Spiel, nennt die Meldung das Spiel, und „Aushängen und fortfahren“ erledigt beides in einem Schritt.
- Weboberfläche an der PS5: L1 und R1 wechseln den Reiter.
- Weboberfläche: „Beenden“ heißt jetzt „Formatierer stoppen“. Der Dialog erklärt, dass zum Schließen der App die PS-Taste reicht – der Formatierer läuft dann weiter.
- Die PS5-Benachrichtigung „beendet“ sagt, woher der Befehl kam (Weboberfläche, PC-Programm, Adresse). Jeder Stopp landet in /data/usb_format/quit.log.
- Diagnose-Bericht: enthält die letzten Stopp-Befehle (ohne Adresse) und auffällige Zeilen aus dem Kernel-Log.
- Linux-AppImage: „Jetzt aktualisieren“ lädt nur die geänderten Teile (meist etwa ein Siebtel), prüft die Prüfsummen und ersetzt die Datei. Die AppImages enthalten dafür Update-Informationen, auch für AppImageUpdate.
- ShadowMountPlus-Meldung „ein Spiel ist noch eingehängt“ wird jetzt verständlich angezeigt statt als englische Fehlermeldung.

## [2.6.61](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.61) – 2026-10-30

- Weboberfläche an der PS5: L2 und R2 scrollen die Seite nach oben bzw. unten – je fester gedrückt, desto schneller; in Listen und Dialogen zuerst dort.
- Spiele und PKG: Nach dem Verbinden ist das erste Laufwerk mit Spielen gewählt; leere Listen sagen, was zu tun ist; die Knöpfe zum Kopieren, Verschieben und Löschen erscheinen erst beim Anhaken.
- Formatieren: erst rot, wenn ein Laufwerk gewählt ist – vorher nur der Hinweis, links (bzw. oben) eins zu wählen (Programm und Weboberfläche). Eine für Spiele vorgewählte SSD wird dort nie übernommen.
- Spielstände: kürzere Angaben („geändert seit …“, „1 ungesichert“), Datum in der Schreibweise der Sprache statt US-Format, die Spalte reicht bis zum Rand.
- Werkzeuge: alle Knöpfe in einer Reihe, darunter der Grund, wenn sie gesperrt sind (Programm und Weboberfläche).
- Das Fenster springt nicht mehr: gleich groß in allen Betriebsarten und auf allen Reitern, auch beim Umschalten auf „Eigene Aufteilung“ (Partitionstabelle mit Knöpfen darunter).
- Behoben: Die Erklärung zu fPKG/PKG unter der PKG-Liste verschwand, sobald Pakete geladen waren.

## [2.6.60](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.60) – 2026-10-30

- Spiel-Einzelheiten (Spiel anklicken): zeigt, ob ShadowMountPlus das Spiel eingehängt hat, dazu Patch und DLCs – mit Knöpfen zum Aus- und Einhängen sowie zum Entfernen von Patch bzw. einzelnen DLCs (Weboberfläche und PS5-USB-Formatierer).
- Doppelte Spiele: „Ältere Version deaktivieren …“ bzw. „Überzählige Kopie deaktivieren …“ – die Abbilder bekommen .off, statt gelöscht zu werden; im PS5-USB-Formatierer „Ältere deaktivieren …“.
- Nach dem Auswerfen steht beim Laufwerk „kann abgezogen werden“, bis es abgezogen (oder wieder eingebunden) ist; ein wieder eingesteckter Stick erscheint von selbst.
- Weboberfläche: mehr Abstand der Auswahlkästchen zum Rand; ruhigere Liste beim Aktivieren/Deaktivieren.
- Entwicklung: release.py lässt vor dem Veröffentlichen auch den Linux-Oberflächentest laufen.

## [2.6.59](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.59) – 2026-10-29

- Neu: „Aktivieren / Deaktivieren …“ (Weboberfläche und PS5-USB-Formatierer): Angehakte Abbilder (.ffpfs, .ffpfsc, .ffpkg, .exfat) bekommen die Endung .off – ShadowMountPlus bindet sie dann nicht mehr ein – bzw. verlieren sie wieder. Es wird nur umbenannt: Inhalt, Rechte, Besitzer und Zeiten bleiben unverändert. Bereits deaktivierte Spiele werden erkannt und als „deaktiviert“ angezeigt; nach dem Aktivieren sucht ShadowMountPlus gleich neu.
- Deaktivierte Kopien zählen nicht mehr als doppelte Spiele; Prüfen, Prüfsumme anlegen, Kopieren, Löschen und Aufräumen behandeln sie und die Prüfsummen-Datei daneben richtig. Ein laufendes Spiel wird nicht deaktiviert, und eine vorhandene Datei gleichen Namens wird nie überschrieben.
- Hilfe in allen Sprachen ergänzt.

## [2.6.58](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.58) – 2026-10-29

- PS5-USB-Formatierer: Die Spieleliste stürzte beim Laden der Spielbilder ab, wenn noch keine Verbindung zur PS5 bestand (in 2.6.57 fehlen deshalb die Linux-Fassungen).
- Enthält alles aus 2.6.57:
  - Robuster gegen manipulierte Abbilder: „Inhalt vergleichen“ (Abbild gegen Ordner) erkennt Ordnerschleifen und FAT-Schleifen sofort, lässt sich jederzeit abbrechen und begrenzt Speicher und Umfang; Bereiche hinter der gültigen Länge einer exFAT-Datei zählen als Nullen (wie beim Betriebssystem).
  - Spielbilder: auch große Bilder in UFS2-Abbildern (.ffpkg) werden angezeigt; Spiele ohne Bild fragt die Weboberfläche nicht bei jedem Neuaufbau erneut an.
  - Auswerfen: der Hinweis „braucht ShadowMountPlus 1.7“ erscheint nur noch, wenn das feststeht; wird ShadowMountPlus erst später gestartet, merkt die Weboberfläche das; die Seriennummer wird mitgeprüft.
  - PS5-USB-Formatierer: Einstellungen gehen weder bei einer kaputten Einstellungsdatei noch nach einem gescheiterten Speichern (gesperrte Datei) verloren; nach dem Wechsel zu einer anderen PS5 gelten deren Angaben (Auswerfen, Fehlerbericht); nach dem Neuverbinden bleiben Häkchen und Bilder in der Spieleliste sichtbar.
  - Tests: neue Belastungstests für den Abbild-Leser (zufällig und gezielt verstümmelte exFAT-/UFS2-Abbilder) und die HTTP-API (tausende kaputte Anfragen, fehlerhafte Antworten von ShadowMountPlus) – beide unter AddressSanitizer in der CI.

## [2.6.57](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.57) – 2026-10-29

- Robuster gegen manipulierte Abbilder: „Inhalt vergleichen“ (Abbild gegen Ordner) erkennt Ordnerschleifen und FAT-Schleifen sofort, lässt sich jederzeit abbrechen und begrenzt Speicher und Umfang; Bereiche hinter der gültigen Länge einer exFAT-Datei zählen als Nullen (wie beim Betriebssystem).
- Spielbilder: auch große Bilder in UFS2-Abbildern (.ffpkg) werden angezeigt; Spiele ohne Bild fragt die Weboberfläche nicht bei jedem Neuaufbau erneut an.
- Auswerfen: der Hinweis „braucht ShadowMountPlus 1.7“ erscheint nur noch, wenn das feststeht; wird ShadowMountPlus erst später gestartet, merkt die Weboberfläche das; die Seriennummer wird mitgeprüft.
- PS5-USB-Formatierer: Einstellungen gehen weder bei einer kaputten Einstellungsdatei noch nach einem gescheiterten Speichern (gesperrte Datei) verloren; nach dem Wechsel zu einer anderen PS5 gelten deren Angaben (Auswerfen, Fehlerbericht); nach dem Neuverbinden bleiben Häkchen und Bilder in der Spieleliste sichtbar.
- Tests: neue Belastungstests für den Abbild-Leser (zufällig und gezielt verstümmelte exFAT-/UFS2-Abbilder) und die HTTP-API (tausende kaputte Anfragen, fehlerhafte Antworten von ShadowMountPlus) – beide unter AddressSanitizer in der CI.

## [2.6.56](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.56) – 2026-10-29

- Laufwerk sicher auswerfen: Neuer Knopf „Auswerfen“ (Weboberfläche und PS5-USB-Formatierer) meldet ein eingebundenes Laufwerk über ShadowMountPlus 1.7 sicher ab – danach kann es abgezogen werden. Belegte Laufwerke (laufendes Spiel, eingebundenes Abbild) werden verständlich gemeldet.
- Spielbilder: Vor jedem Spiel in der Bibliothek steht sein Bild – die Vorschau von ShadowMountPlus oder das icon0.png des Spiels (auch aus exFAT-/UFS2-Abbildern).
- ShadowMountPlus-Version unten neben der Versionsnummer; Funktionen, die ShadowMountPlus 1.7 brauchen, sind ohne es gesperrt und sagen warum.
- Tempo-Verlauf: Das Verlaufsfenster zeigt das Lesetempo jedes Datenträgers mit mindestens zwei Messungen als kleine Kurve; ein deutlicher Einbruch ist rot markiert.
- Diagnose- und Fehlerbericht enthalten jetzt Firmware, ShadowMountPlus-Version samt Fähigkeiten und erkannte Kernel-Erweiterungen (kstuff, etaHEN).
- Das Protokoll von ShadowMountPlus (debug.log) wird nur noch gelesen, wenn dessen API nicht antwortet (Fassungen vor 1.7).
- Hilfe in allen Sprachen ergänzt.

## [2.6.55](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.55) – 2026-10-29

- Verschleiß erkennen: Liest ein Stick bei „Tempo messen“ (oder beim Formatieren mit Prüfung) deutlich langsamer als bei früheren Messungen, weisen Formatierer und PC-Programme auf möglichen Verschleiß hin. Die PC-Programme tragen das Lesetempo jetzt auch in ihren Verlauf ein.
- „Inhalt vergleichen“ vergleicht jetzt auch ein Abbild mit einem Spielordner: exFAT- und UFS2-Abbilder (.exfat, .ffpkg) werden Datei für Datei gelesen. Der Abbild-Leser kann dafür große Dateien (UFS2 mit indirekten Blöcken) und Superblöcke an allen üblichen Stellen lesen.
- ShadowMountPlus 1.7: Spielordner mit eboot2.bin statt eboot.bin gelten nicht mehr als „unvollständig“.
- Gebaut mit dem PS5-Payload-SDK 0.45 (Kernel-Offsets für Firmware 14.00/14.10).
- Hilfe (alle Sprachen): „Inhalt vergleichen“ und der Verschleiß-Hinweis beschrieben.
- Entwicklung: Linux-Fassungen werden in einem Ubuntu-22.04-Container gebaut (GitHub schaltet die 22.04-Läufer ab) und laufen weiter ab glibc 2.35; die CI prüft den PS5-Code zusätzlich mit AddressSanitizer und UBSan; mke2fs.exe wird neu gebaut, sobald sich Quellen oder zig-Fassung ändern; die Übersetzungsprüfungen melden auch deutsche Texte, die gar nicht zum Übersetzen markiert sind.

## [2.6.54](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.54) – 2026-10-29

- PS5-USB-Formatierer: Einstellungen von „Am PC – frei formatieren“ (Dateisystem, Bezeichnung, Ordner für Sicherungen …) werden wieder gespeichert – seit 2.6.52 gingen sie verloren, sobald vorher etwas anderes gespeichert wurde. Zwei offene Fenster führen ihre Einstellungen jetzt auch innerhalb eines Bereichs zusammen.
- Hilfe (alle Sprachen): Tastenkürzel richtig (Strg+1…5 Reiter, Strg+6/7 Betriebsarten, Strg+Umschalt+B Fehlerbericht), „Eigene Aufteilung“ statt „Expertenmodus“, Namen der Betriebsarten wie in der Seitenleiste.
- „Inhalt vergleichen“ lehnt zweimal dasselbe Spiel ab; die Windows-Warteschlange findet ihr Ziel auch nach einem Wechsel der Betriebsart; der Verlauf als CSV aus der Weboberfläche steht in Ortszeit.
- Pakete: Die Lizenz des Projekts liegt jetzt auch den Windows-Programmen und den AppImages bei. release.py prüft, dass die App die veröffentlichte ELF enthält.
- Aufgeräumt: nicht genutzte Entwurfsbilder, ein altes Diagnosewerkzeug und Debug-Dateien aus dem Repository entfernt.

## [2.6.53](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.53) – 2026-10-29

Optik-Korrekturen in allen Oberflächen:

- Weboberfläche: Das Suchfeld über der Spieleliste hat jetzt dasselbe Aussehen wie die übrigen Felder (vorher zeichnete der PS5-Browser es eckig und zu niedrig). Angehakte Kästchen zeigen ein echtes Häkchen statt eines Doppelrings, im dunklen Design gut lesbar. Größenangaben kleben nicht mehr an der Bildlaufleiste.
- Weboberfläche: Dialogknöpfe brechen mit Abstand um, die Knöpfe im Duplikat-Hinweis haben Abstand, der Hinweis „Danach frei“ stößt nicht mehr an den Text darunter, PKG- und Spielstände-Knöpfe sind nicht mehr eingerückt.
- PC-Programme: Bildlaufleisten erscheinen nur noch, wenn es etwas zu scrollen gibt. Während der Suche nach USB-Datenträgern steht „Suche …“ statt „kein USB-Datenträger gefunden“. Baugleiche Sticks ohne Seriennummer werden auch in „Am PC – für die PS5“ nach dem Neuladen nicht still wieder ausgewählt.

## [2.6.52](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.52) – 2026-10-29

- PC-Programme: Laufen zwei Fenster desselben Programms, überschreibt keines mehr die Einstellungen des anderen (Adresse, Sprache, Warteschlange …).
- PC-Formatierer: Hängen mehrere baugleiche Sticks ohne Seriennummer am PC, springt die Auswahl nach dem Neuladen nicht mehr still auf einen davon – neu wählen.
- Linux-Fassung: btrfs-Partitionen auf dem Stick werden erkannt und ausgehängt (vorher dauerhaft „wird gerade benutzt“).
- Windows: Ein Image, das über ein subst-Laufwerk oder eine Freigabe dieses Rechners (\\localhost\E$\…) auf dem Ziel-Stick selbst liegt, wird erkannt und nicht auf sich selbst zurückgeschrieben.

## [2.6.51](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.51) – 2026-10-29

Fehlerbehebungen aus einer Durchsicht des ganzen Projekts:

- Aufräumen: Solange die Spieleliste nach dem Start noch gelesen wird, gibt es keine Liste mehr (vorher galten dann alle Backport-Ordner als verwaist). In sehr vollen Ordnern (über 600 Einträge) werden Prüfsummen-Dateien nicht mehr fälschlich als verwaist gemeldet.
- Große Auswahl: Kopieren, Prüfen, Prüfsummen und Aufräumen gehen jetzt mit bis zu 256 bzw. 300 Einträgen auf einmal (vorher scheiterten mehr als etwa 56 mit einer irreführenden Meldung).
- Spielstand hochladen: Ein abgebrochener Upload ließ eine Datei auf der PS5 geöffnet.
- Image prüfen: Eine beschädigte oder präparierte Image-Datei mit unsinniger Größe brachte den Formatierer zum Absturz.
- Fortschritt: Der Balken läuft über übersprungene Spiele hinweg weiter, statt unter 100 % stehen zu bleiben.
- Warteschlange (Web und Windows-App): Das Ziel wird über die Seriennummer gefunden – nach dem Umstecken wird nicht auf einen anderen Stick kopiert. Die Web-Warteschlange bleibt nicht mehr auf „läuft“ hängen, wenn sie während des Wartens geleert wird.
- Weboberfläche: Ergebnis von „Inhalt vergleichen“ als Fenster; Größen der Ordner-Spiele werden nach einem schnellen Laufwerkswechsel nachgeholt.
- Windows-App: Der Kopierdialog friert bei langsamer PS5 nicht mehr ein; Schließen während einer Abfrage an die PS5 gibt keinen Fehler mehr; eine fehlgeschlagene „Teilsicherung verwerfen“ meldet sich nicht mehr als erfolgreich.
- Linux-Fassung: Vor dem Schreiben prüft der Helfer noch einmal, ob unter dem Gerätenamen derselbe Datenträger steckt (Größe, Modell, Seriennummer, Gerätenummer) – auch direkt vor mkntfs/mke2fs. Ein während des Aushängens umgesteckter Stick wird nicht mehr beschrieben.
- PC-Programme: Die PS5-Suche unter Linux durchsucht alle Netzwerkkarten, nicht nur die der Standard-Route. Schließen während der Rechte-Abfrage fragt nach. Eine Image-Sicherung überschreibt keine Datei, nach der der Speichern-Dialog nicht gefragt hat. Ein beschädigtes eigenes Image mit verschobenen Datenrahmen wird erkannt.
- Weboberfläche: Abbrechen einer fremden Image-Sicherung fragt wieder nach dem Behalten der Teilsicherung; „Warteschlange starten“ ist während eines laufenden Auftrags ausgeblendet.
- Entwicklung: CI-Testliste repariert (ein wörtliches „\n“ rief einen nicht vorhandenen Test auf), Exit-Codes in Pipes, leere SHA256SUMS werden nie hochgeladen, release.py prüft vorab den Stand von GitHub und ob SHA256SUMS alle Dateien nennt.

## [2.6.50](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.50) – 2026-10-29

- Neu: „Inhalt vergleichen“ prüft zwei Kopien desselben Spiels Byte für Byte (Abbilder über SHA-256, Ordner Datei für Datei) – in der Weboberfläche am Duplikat-Hinweis, in der Windows-App im Prüfsummen-Menü.
- Neu: Ordner-Spiele ohne eboot.bin oder Titelbild werden als „unvollständig“ gekennzeichnet; die Einzelheiten nennen, was fehlt.
- Ziel-Auswahl beim Kopieren: das zuletzt gemessene Lesetempo (Tempo messen) steht beim Laufwerk, unter 40 MB/s als „langsam“; jetzt auch in der Windows-App mit „zu wenig Platz“ und FAT32-Hinweis.
- Verlauf lässt sich als CSV-Datei speichern (Web und Windows-App); die Windows-App benennt jetzt alle Auftragsarten.
- Windows-App: die Warteschlange für Kopieraufträge übersteht einen Neustart des Programms.
- Entwicklung: tools/ps5_deploy.py wartet bis zu 5 Minuten (--wait-loader), wenn der ELF-Loader noch nicht annimmt, statt abzubrechen.

## [2.6.49](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.49) – 2026-10-28

- Weboberfläche: Die Warteschlange für Kopieraufträge übersteht ein Neuladen der Seite (im Browser gemerkt).
- Weboberfläche: In der Ziel-Auswahl stehen „zu wenig Platz“ und „FAT32: keine Dateien über 4 GB“ direkt beim Laufwerk; vorgewählt wird das erste Ziel, auf das es passt.
- Weboberfläche: Doppelte Spiele mit gleicher Version und genau gleicher Größe werden als wahrscheinliche Kopie erkannt; die überzählige Kopie lässt sich zum Löschen vorwählen.
- Entwicklung: tools/release.py wartet nach dem Veröffentlichen auf die CI, startet gescheiterte Läufe neu und prüft die angehängten Dateien (--check für ein fertiges Release); der Download der mke2fs-Quellen wiederholt sich bei Netzfehlern.

## [2.6.48](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.48) – 2026-10-28

- Neu: „Aufräumen“ findet Verwaistes (Prüfsummen/Dump-Infos ohne Spiel, Backport-Ordner ohne Spiel, leere Spielordner) und löscht nur, was angehakt wird (Web und Windows-App).
- Neu: Ordner-Spiele bekommen eine Prüfsummenliste (<Name>.manifest.sha256, sha256sum-Format); „Spiele prüfen“ meldet geänderte, fehlende und neue Dateien. Die Liste wird mit dem Spiel kopiert und gelöscht.
- Neu: Kopieraufträge lassen sich in eine Warteschlange stellen und laufen nacheinander (Web und Windows-App); ein Abbruch leert den Rest.
- Weboberfläche: Diagnose-Bericht (kopierbar, ohne Seriennummern/Adressen) in der Fußzeile; der Verlauf zeigt die neuen Auftragsarten.
- Fortschritt: Tempo und Restzeit beginnen bei jedem Schritt (Kopieren, Prüfen, nächstes Spiel) neu.
- Entwicklung: Selbsttest der gebauten Programme (--selftest: Übersetzungen, Helfer-Prüfsumme, ELF, alle Reiter) in der CI; Layout-, Tastatur- und Browser-Tests.

## [2.6.47](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.47) – 2026-10-28

- Teilkopien eines abgebrochenen Kopierens werden angezeigt (mit Größe) und lassen sich verwerfen (Web und Windows-App).
- Neu: „Prüfsumme erstellen“ legt neben Abbildern ohne Prüfsumme eine .sha256 an (Format von sha256sum); vorhandene bleiben unberührt.
- Neu: Spiel-Einzelheiten per Klick/Enter (Web) bzw. Doppelklick (Windows): Titel-ID, Version, Art, Größe, größte Einträge, zugehörige Dateien, Prüfsumme.
- Duplikate: „Ältere Version auswählen“ bzw. „Ältere Duplikate anhaken“ wählt die ältere Version zum Löschen vor.
- Weboberfläche: Knopfleisten brechen bei schmaler Anzeige um (PKG-Reiter ragte über den Rand).
- Entwicklung: Layout-Tests in allen Sprachen (Web und Windows), Tastatur-Test der Weboberfläche, Browser-Test in der CI.

## [2.6.46](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.46) – 2026-10-27

- Kopieren: Ein abgebrochenes oder fehlgeschlagenes Kopieren lässt seine Teilkopie liegen; dieselbe Auswahl erneut kopieren setzt fort (gleiche Dateien werden per Prüfsumme übernommen, abweichende neu kopiert).
- Neu: „Spiele prüfen“ – Abbilder werden gegen ihre .sha256-Datei geprüft (Web und Windows-App).
- Dialoge zeigen den freien Platz danach (Kopieren, Löschen); bei Duplikaten wird die ältere Version gekennzeichnet; die Spieleliste der Weboberfläche ist durchsuchbar.
- Windows-App: unerwartete Fehler kommen in fehler.log, „Bericht kopieren“ (auch Strg+Umschalt+B).
- Entwicklung: Browser-Test der Weboberfläche und weitere Tests in der CI; Selbsttest nach dem PS5-Update.

## [2.6.45](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.45) – 2026-10-27

- Gleicher Inhalt wie 2.6.44 (Windows-Oberfläche gegen Absturz bei aufgeklappter Auswahlliste abgesichert); der neue Oberflächen-Test in der CI findet seine Knöpfe jetzt sprachunabhängig, damit die Windows-Programme wieder am Release hängen.

## [2.6.44](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.44) – 2026-10-27

- Windows: Weitere Absicherung gegen Abstürze bei aufgeklappter Auswahlliste in Dialogen (Tastaturfokus im Meldungsfenster); der Fehlerdialog fällt nicht mehr selbst aus – die Ursache bleibt in der Fehlerausgabe.
- Neuer Oberflächen-Test (Fokusfang, Meldungen, Kopierdialog) läuft in der CI vor dem Bau.

## [2.6.43](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.43) – 2026-10-27

- Weboberfläche: Der Reiter „Spiele“ wird in den Fremdsprachen groß geschrieben („Games“, „Giochi“, „Juegos“, „Jeux“).
- README-Bilder auf den Stand 2.6.42 erneuert.

## [2.6.42](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.42) – 2026-10-27

- Windows: Absturz („Unhandled exception … KeyError: 'popdown'“) behoben, der auftrat, wenn im Kopierdialog die Liste der Ziel-Laufwerke aufgeklappt blieb. Betraf alle Fenster mit Dialogen (auch das Standard-Fenster).

## [2.6.41](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.41) – 2026-10-26

- Vorabprüfung vor dem Kopieren: Der Dialog zeigt sofort, wenn das Ziel zu klein ist oder FAT32 Dateien über 4 GB nicht aufnimmt (mit Dateinamen); passt es nicht, startet nichts. Gilt für Spiele und PKG-Dateien, in der Weboberfläche und in der Windows-App.
- PKG-Dateien: Hinweise im Vergleich mit der PS5 – neuer / schon installiert / älter als die installierte Version, und „braucht Firmware X, die PS5 hat Y“. Die Bibliothek liefert dafür die installierte Spielversion und die PS5-Firmware.
- Fortschritt: Beim Kopieren zeigt die Zeile unter dem Balken zusätzlich das Tempo (MB/s); das Löschen von PKG-Dateien lässt sich abbrechen.
- Neuer Test (Vorabprüfung, Versionsvergleich), README in allen Sprachen ergänzt.

## [2.6.40](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.40) – 2026-10-26

- Wichtige Fehlerbehebung: Eine einzelne Anfrage mit einem Pfad ohne Schrägstrich beim Wiederherstellen von Spielständen konnte den Formatierer auf der PS5 zum Absturz bringen.
- Spielstände wiederherstellen: Das Kopf-Kästchen wählt nur noch die neueste Version je Spielstand (vorher konnten ältere mit angehakt werden); beim PS4-Spielstand wird bei einem Fehler der alte Schlüssel wiederhergestellt; der freie Platz wird für alle Spielstände zusammen geprüft.
- Spielstände sichern: ausgeblendete Spielstände (anderer Benutzer) werden nicht mehr mitgesichert; Fehlermeldungen beim Laden bleiben sichtbar.
- Formatieren: Vor dem Bootsektor wird alles andere sicher auf den Datenträger geschrieben.
- Weitere Korrekturen: Speicherleck bei Spielstand-Aufträgen, Thread-Sicherheit, Prüfung der Echtheitsprüfung (Löschen der GPT-Sicherung am Plattenende), Zeitstempel-Grenzen, Namenswahl bei behaltenen Teilsicherungen, Meldung bei exFAT-Zielen, einzelne Unicode-Zeichen aus fremden Datenträgern.
- Ältere Browser (PS5): Typ-Kennzeichen und Formatieren-Rahmen haben Ersatzfarben.
- Windows: Schreibschutz-Erkennung prüft nur den echten Schreibschutz-Fehler; Linux: Helfer wird ausgeliefert nur mit Prüfsumme gestartet.

## [2.6.39](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.39) – 2026-10-26

- Windows-Programm: Die Spielstände-Liste ist breiter, lange Spielnamen werden nicht mehr abgeschnitten.
- Spiele kopieren mit Prüfsumme und Löschen der Kopie an der echten PS5 geprüft.

## [2.6.38](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.38) – 2026-10-26

- PKG-Reiter: Spielname, Version, Art (Spiel/Update/DLC) und nötige Firmware werden aus den Paketen gelesen (PS4: param.sfo, PS5: param.json) und angezeigt.
- Neuer Knopf „Kopieren nach …“ für PKG-Dateien (Web und Windows).
- Löschen: Bricht es mittendrin ab, wird „nur teilweise gelöscht“ gemeldet.
- Zugehörige Dateien (Backport, Prüfsummen) bleiben stehen, solange eine andere Kopie des Spiels sie nutzt; Löschen nur innerhalb des durchsuchten Ordners.
- Kopieren/Verschieben prüft Quelle und Ziel, ob sie sich geändert haben; ein Abbruch wird als Abbruch gemeldet.
- Windows-Programm: Listen nach Theme-/Sprachwechsel wieder gefüllt, interner Speicher auch ohne USB-Stick sichtbar, Größen der Ordner-Spiele bleiben erhalten, Netzabfragen frieren das Fenster nicht mehr ein.
- Weboberfläche: Controller-Fokus bleibt erhalten, Zeitlimit für Anfragen, bessere Barrierefreiheit (Reiter und Laufwerksliste).

## [2.6.37](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.37) – 2026-10-25

- Reiter je nach Auswahl: Der interne PS5-Speicher zeigt „Spiele“, „PKG“ und „Spielstände auf der PS5“. Laufwerke zeigen „Spiele“, „PKG“, „Werkzeuge“ und „Formatieren“. Der Eintrag „Interner PS5-Speicher“ ist immer da (Web und Windows).
- Weboberfläche: Der Warnhinweis zum Abziehen von Datenträgern steht dezent unter den Laufwerken, wie im Windows-Programm.

## [2.6.36](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.36) – 2026-10-25

- Fehler behoben: Beim Fortsetzen einer Image-Sicherung konnte der Formatierer auf bereits freigegebenen Speicher zugreifen und abstürzen.
- Interner PS5-Speicher gewählt: Der Reiter „Formatieren“ wird nicht mehr angezeigt (Web und Windows).
- Schneller: Zugehörige Dateien (Backport, Prüfsummen) werden einmal je Suche ermittelt, nicht bei jedem Abruf der Spieleliste.
- README in allen Sprachen um Kopieren/Verschieben und den internen Speicher ergänzt.

## [2.6.35](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.35) – 2026-10-25

- Neuer Knopf „Spiele kopieren …“ in der Weboberfläche und im Windows-Programm: wie Verschieben, aber die Originale bleiben und es kommt keine Frage zum Löschen.
- Windows-Programm: Der Zähler der angehakten Spiele steht in einer eigenen Zeile über den Knöpfen und wird nicht mehr abgeschnitten.

## [2.6.34](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.34) – 2026-10-25

- Weboberfläche: Der Reiter „Spielstände“ heißt jetzt „Spielstände auf der PS5“ (auch in Englisch, Italienisch, Spanisch und Französisch).

## [2.6.33](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.33) – 2026-10-25

- Korrektur zu 2.6.32: PS4-Pakete werden wieder als PKG angezeigt, nicht als fPKG. Die Erkennung über den Image-Schlüssel war falsch, weil der Fake-Schlüssel auch offizielle PS4-Pakete öffnet.
- PS5-Pakete zeigen weiter PKG (offiziell) oder fPKG (Debug-Signatur im Kopf).

## [2.6.32](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.32) – 2026-10-25

- PS4-Pakete: fPKG und PKG werden jetzt unterschieden. Der Image-Schlüssel eines fPKG ist mit dem öffentlich bekannten Fake-Schlüssel verschlüsselt, ein offizielles PKG nicht. Die Erkennung braucht nur wenige KB am Dateianfang.
- Lässt sich ein Paket nicht bestimmen, steht wie bisher nur PKG.

## [2.6.31](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.31) – 2026-10-25

- PC-Suche: Der Fortschrittsbalken bleibt auch bei einer schlafenden Platte nicht stehen – er rückt stetig vor und geht nie zurück.
- Spiele an festen Orten samt Backport-Ordnern an der echten PS5 geprüft (Kopieren mit Prüfsumme).

## [2.6.30](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.30) – 2026-10-25

- Backport-Ordner werden jetzt wie bei ShadowMountPlus gefunden: `backports/<Titel-ID>` (auch mit Zusatz im Namen, z. B. `PPSA12345 Spielname`) neben dem Spiel, im Laufwerk, in `homebrew` und `etaHEN/games` sowie im internen Speicher. Andere Ordner bleiben unberührt.
- Beim Kopieren und Verschieben landet der Backport-Ordner am Ziel wieder in `backports` neben dem Spiel; beim Löschen kommt die Frage, ob er mitgelöscht werden soll.

## [2.6.29](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.29) – 2026-10-25

- Spiele am internen PS5-Speicher (/data/homebrew, /data/etaHEN/games) und an /mnt/extN lassen sich kopieren, verschieben und löschen – in der Weboberfläche und im Windows-Programm.
- Zugehörige Dateien (Backport-Ordner mit der Titel-ID des Spiels, Prüfsummen, Dump-Infos) werden auf Wunsch mitkopiert; beim Löschen kommt die Frage, ob sie ebenfalls gelöscht werden sollen.
- Der Formatierer selbst lässt sich nicht löschen.

## [2.6.28](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.28) – 2026-10-25

- PKG-Dateien lassen sich im PKG-Reiter verschieben (auch auf einen anderen Datenträger) und löschen – in der Weboberfläche und im Windows-Programm.
- Suche und Einlesen: Der Fortschrittsbalken bleibt nicht mehr bei 0 %, auch wenn eine schlafende Platte erst anlaufen muss; die Spielesuche der PS5 zeigt von Anfang an einen Balken.
- Windows-Programm: Spielstände-Liste übersichtlicher.

## [2.6.27](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.27) – 2026-10-24

### Neu in 2.6.27

- **Windows-Programm mit Reitern wie die Weboberfläche:** An der PS5 gibt es jetzt Spiele · PKG · Spielstände · Werkzeuge · Formatieren (mit Anzahl an Spiele und PKG). Die Seitenleiste enthält nur noch die Betriebsarten.
- **PKG-Seite:** PKG oder fPKG (PS5, an der Signatur im Paketkopf), Plattform, Update, Titel-ID und Größe.
- **Formatieren in rotem Rahmen** mit der Warnung „Formatieren löscht alle Daten auf diesem Laufwerk“ und dem gewählten Laufwerk.
- **Werkzeuge an der PS5:** Tempo messen, Prüfen & reparieren und Umbenennen (bisher nur am PC), darunter die Image-Sicherung mit den unvollständigen Sicherungen.
- Alle Seiten sind gleich hoch (das Fenster springt beim Wechseln nicht mehr); Strg+1 bis 5 wechseln die Reiter, Strg+6/7 die Betriebsart.
- Der Formatierer auf der PS5 ist unverändert (2.6.26).

## [2.6.26](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.26) – 2026-10-24

### Neu in 2.6.26

- **PKG-Reiter liest den Paketkopf (wie die LibProsperoPkg):**
  - **PS5-Pakete:** Das Signatur-Byte im Kopf unterscheidet **PKG** (offiziell signiert) von **fPKG** (Debug-Signatur).
  - **PS4-Pakete:** Titel-ID aus der Content-ID und **Update** (Patch) an den Inhalts-Flags. fPKG und offizielles PKG unterscheiden sich im PS4-Kopf nicht; dort steht nur „PKG“.
  - Jede Zeile zeigt Kennzeichen (PKG grün, fPKG orange), Plattform, Update, Titel-ID und Pfad.
- `/api/library` liefert je Paket `type`, `title_id` und `update`.

## [2.6.25](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.25) – 2026-10-24

### Neu in 2.6.25

- **Neue Oberfläche in Reitern:** Laufwerke oben, darunter Spiele · PKG · Spielstände · Werkzeuge · Formatieren. Es ist immer nur ein Bereich sichtbar. **Formatieren** liegt in einem roten Rahmen mit der Warnung „löscht alle Daten auf diesem Laufwerk“ und nennt das gewählte Laufwerk.
- **Typ-Kennzeichen der Spiele:** Dump-Ordner, exFAT, ffpfs, ffpfsc, ffpkg (früher nur „Ordner“, „Abbild PFS/UFS2“). Auch in der Windows-App.
- **PKG-Reiter:** listet die `.pkg`-Dateien des Laufwerks (auch in den Unterordnern `ps4` und `PS5-Updates`) mit Plattform PS4/PS5 aus dem Dateikopf. Ob ein Paket ein fPKG oder ein offizielles PKG ist, lässt sich aus der Datei nicht sicher erkennen.
- **Überarbeitete Spieleliste:** zweizeilige Zeilen, ausgerichtete Größenspalte, kompakte Sortierleiste; die Größen der Ordner-Spiele werden automatisch im Hintergrund ermittelt.
- **Image-Sicherung fortsetzen:** Beim Abbrechen einer Sicherung auf die PS5 kann die Teilsicherung behalten werden. Unvollständige Sicherungen (auch nach Fehler oder Neustart der PS5) erscheinen unter Werkzeuge → „Sichern & wiederherstellen“ mit **Fortsetzen** und **Verwerfen**. Wahlweise wird der bisherige Stand des Laufwerks noch einmal gelesen und verglichen (empfohlen). Die fertige Datei ist byte-identisch zu einer Sicherung ohne Unterbrechung. Daten werden jetzt in Rahmen von höchstens 1 GiB geschrieben (mit dem bisherigen Lesen und Wiederherstellen kompatibel).
- Neue Aufrufe: `POST /api/backup/resume`, `POST /api/images/discard`; `/api/cancel` nimmt `{"keep":true}`; `/api/images` liefert `partials`; `/api/library` liefert `pkgfiles` und je Titel `ext`.

## [2.6.24](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.24) – 2026-10-24

### Neu in 2.6.24

- **Spiele verschieben: Hinweis zum Löschen der Originale.** Kennt ShadowMountPlus ein Spiel nicht (z. B. manche PS4-Spiele), lehnt der Löschdialog das Löschen des Originals nach dem Kopieren ab; das Original bleibt dann stehen, die Kopie ist trotzdem vollständig. Der Hinweis steht jetzt im Verschieben-Dialog und in der Hilfe (fünf Sprachen).
- Der Verschieben-Dialog erscheint in neutraler Farbe statt im Rosa des Löschdialogs.

## [2.6.23](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.23) – 2026-10-24

### Neu in 2.6.23

- **Spiele zwischen Laufwerken verschieben:** „Spiele verschieben …“ (Web-Oberfläche und Windows-Programm) kopiert angehakte Spiele auf ein anderes eingebundenes Laufwerk, an denselben Pfad (z. B. `homebrew/Name`). Zuerst wird in den versteckten Ordner `.usbfmt-move` kopiert (ShadowMountPlus sucht dort nicht), je Datei eine SHA-256-Prüfsumme gerechnet und auf Wunsch die Kopie zurückgelesen und verglichen; erst danach wird das Spiel an seine Stelle gesetzt.
- **Die Originale bleiben erhalten.** Danach fragt der Formatierer, ob sie gelöscht werden sollen (über den bekannten Löschdialog).
- Ein am Ziel schon vorhandenes Spiel wird nie überschrieben; Platzprüfung vorher; Abbrechen räumt auf; mit laufendem Spiel wird abgelehnt.
- Neuer Aufruf `POST /api/move` (`keys`, `device`, `verify`).

## [2.6.22](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.22) – 2026-10-24

### Neu in 2.6.22

- **Spieleliste sortieren (Web-Oberfläche):** nach Name, Größe oder Titel-ID; die Wahl wird gemerkt. Das Windows-Programm hatte die Sortierung schon.
- **Größe von Ordner-Spielen:** Der Formatierer ermittelt sie beim Sortieren nach Größe im Hintergrund (nur lesend, `GET`/`POST /api/library/sizes`); bei großen Spielen mit vielen Dateien kann das dauern. Die Größe steht hinter jedem Spiel und bei jedem Fundort doppelter Spiele.
- Prüfbericht nach dem Formatieren: kein überflüssiger Rollbalken mehr.

## [2.6.21](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.21) – 2026-10-24

### Neu in 2.6.21

- **Prüfbericht nach dem Formatieren in neutraler Darstellung:** Die Liste lag zuvor in der roten Warnbox und wirkte wie eine Warnung; jetzt erscheint sie ohne Warnfarbe, mit ✓ / ! / ✗ / • je Zeile.

## [2.6.20](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.20) – 2026-10-24

### Neu in 2.6.20

- **Prüfbericht nach dem Formatieren:** Der Abschlussdialog zeigt jetzt einzelne Zeilen mit Ergebnis (✓ / ! / ✗ / •): Prüfung vorher, Tempo, Dateisystem, Partitionstabelle, Ordner, Größe und Einbinden.
- **Nachkontrolle:** Bei exFAT prüft der Formatierer das frisch geschriebene Dateisystem noch einmal, solange er den Datenträger hat; ein gefundener Fehler steht im Bericht und in der Meldung. Web-Oberfläche und Windows-Programm.

## [2.6.19](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.19) – 2026-10-24

### Neu in 2.6.19

- **Spielstände: Vorversionen der Sicherung.** Wird derselbe Spielstand erneut an dasselbe Ziel gesichert, bleibt die bisherige Sicherung als Vorversion (`<Titel>_<Name>.<Datum-Uhrzeit>.zip`) erhalten. Es bleiben die neueste und die zwei davor; **ältere Vorversionen werden automatisch gelöscht.**
- **Liste zeigt den Stand der Sicherung:** „gesichert am …“, „noch nie gesichert“ oder „seit der Sicherung vom … geändert“; bei Spielen mit mehreren Containern eine Zusammenfassung in der Kopfzeile. Web-Oberfläche und Windows-Programm.
- **Wiederherstellen vergleicht mit dem aktuellen Stand:** jede Sicherung ist als „entspricht dem aktuellen Stand“, „älter“ oder „neuer“ gekennzeichnet; von mehreren Versionen desselben Spielstands lässt sich nur eine auswählen.
- Die Spielstände-Liste der PS5 und die Liste der Sicherungen melden die Änderungszeit des Spielstands.

## [2.6.18](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.18) – 2026-10-24

### Neu in 2.6.18

- **Restzeit-Zeile unter dem Fortschrittsbalken wird am Ende des Auftrags geleert:** Zuvor blieb nach dem Fertigwerden z. B. „3,4 GB von 3,4 GB · noch etwa 5 Sek.“ stehen.

## [2.6.17](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.17) – 2026-10-23

### Neu in 2.6.17

- **Ein durchgehender Fortschrittsbalken bei Echtheitsprüfung, Tempomessung und vollständigem Formatieren:** Jeder Schritt hat einen festen Abschnitt des Balkens (z. B. Beschreiben 0–50 %, Prüfen 50–100 %). Der Balken läuft nicht mehr rückwärts und steht nach dem Start nicht auf 0 %. Die Restzeit steht jetzt in der Zeile unter dem Balken statt im Schrittnamen.
- **Spiele löschen nennt das Spiel:** „Spiele löschen: Name (2 von 5)“.

## [2.6.16](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.16) – 2026-10-23

### Neu in 2.6.16

- **Fortschrittsanzeige überarbeitet:** Die Image-Sicherung rechnet nach den belegten Bytes statt nach der Position auf dem Datenträger. Der Balken blieb vorher bei großen Datenträgern lange auf 0 % (Lauf-Balken ohne Wert), jetzt wächst er von Beginn an gleichmäßig. Beim Wiederherstellen eines Images laufen Schreiben und Nachlesen in einem durchgehenden Balken.
- **Zeile unter dem Balken:** zeigt bei der Image-Sicherung die bearbeitete Menge („12,3 GB von 58,6 GB“) und bei allen Aufträgen mit Prozentwert die geschätzte Restzeit („noch etwa 4 Min.“). Weboberfläche und Windows-Programm (PS5-Modus).

## [2.6.15](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.15) – 2026-10-23

### Neu in 2.6.15

- **Spielstände: Container eines Spiels erst beim Aufklappen sichtbar.** Hat ein Spiel mehrere Container (z. B. Spielstand und PS2-Speicherkarte), zeigt die Liste nur die Kopfzeile mit Pfeil; ein Klick klappt die einzelnen Einträge auf. Web-Oberfläche (Liste und Wiederherstellen-Auswahl) und Windows-Programm.

## [2.6.14](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.14) – 2026-10-23

### Neu in 2.6.14

- **Zeitstempel von Spielständen:** Das Setzen der Änderungszeit (Wiederherstellen, `POST /api/saves/times`) probiert nacheinander `futimens`, `futimes`, `utimes` und `utimensat`, weil die PS5 bei einzelnen Aufrufen „No such file or directory“ meldet.

## [2.6.13](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.13) – 2026-10-23

### Neu in 2.6.13

- **Zeitstempel-Reparatur für Spielstände funktioniert auf der PS5:** `POST /api/saves/times` setzt die Änderungszeit jetzt über eine geöffnete Datei (der Aufruf mit Pfad lieferte auf der PS5 „No such file or directory“). Damit lassen sich die Zeiten bereits wiederhergestellter Spielstände mit `tools/ps5_fix_save_times.py` an die Datenbank der PS5 angleichen.

## [2.6.12](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.12) – 2026-10-23

### Neu in 2.6.12

- **Wiederherstellen von Spielständen behält die Änderungszeit der Dateien:** Die Datenbank der PS5 führt zu jedem Spielstand die Änderungszeit mit. Frühere Versionen setzten sie beim Wiederherstellen auf die aktuelle Zeit, danach passten Datei und Datenbank nicht mehr zusammen (ein Spiel konnte den Spielstand nicht mehr laden). Jetzt bleibt die Zeit der vorhandenen Datei erhalten; neu angelegte Spielstände bekommen die Zeit aus der Sicherung.
- Sicherungen enthalten die Änderungszeit des Spielstands.
- Neu: `POST /api/saves/times` und `tools/ps5_fix_save_times.py` gleichen die Zeiten bereits wiederhergestellter Spielstände mit der Datenbank der PS5 ab.

## [2.6.11](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.11) – 2026-10-23

### Neu in 2.6.11

- **Wiederherstellen zeigt, wo eine Sicherung liegt:** Die Auswahl nennt bei jeder Sicherung den Ort (intern auf der PS5 oder den Namen des USB-Laufwerks). Dieselbe Sicherung kann an mehreren Orten liegen und war bisher nicht zu unterscheiden. Web-Oberfläche und Windows-Programm.

## [2.6.10](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.10) – 2026-10-23

### Neu in 2.6.10

- **Spielstände nach Spiel gruppiert:** Ein Spiel mit mehreren Containern (z. B. Indiana Jones: AppSmall, PS2.VMC, PS2HDSNAP) erscheint einmal als Kopfzeile mit Sammel-Kästchen; darunter die einzelnen Container, einzeln abwählbar. Gilt in der Web-Oberfläche, im Windows-Programm und im Wiederherstellen-Dialog.

## [2.6.9](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.9) – 2026-10-23

### Neu in 2.6.9

- **PS4-Spielstände sichern und wiederherstellen:** Spielstände aus `savedata` (PS4) werden samt Schlüsseldatei (`.bin`) gesichert und zurückgeschrieben, nichts wird entschlüsselt. Hinter dem Spielnamen steht jetzt das Kennzeichen PS5 bzw. PS4 (Web-Oberfläche und Windows-Programm).
- **Weniger Doppelte:** Die zweite Systemkopie, die die PS5 zu jedem Spielstand selbst anlegt (`sce_bu`), wird nicht mehr aufgelistet.
- Sammel-ZIPs können PS5- und PS4-Spielstände mischen; beim Wiederherstellen werden Konsole, Container und Schlüsseldatei geprüft.

## [2.6.8](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.8) – 2026-10-22

### Neu in 2.6.8

- **Sichern und Wiederherstellen von Spielständen lassen sich abbrechen** ("Abbrechen" in Web-Oberfläche und Windows-Programm). Halb geschriebene Dateien werden entfernt, vorhandene Spielstände bleiben unverändert; die Meldung nennt, was schon fertig ist.
- **Konsole sichtbar:** hinter dem Spielnamen steht PS5 (oder PS4 bei einem Container im PS4-Format); im Windows-Programm eigene Spalte "Konsole". Auch in `metadata.json` jeder Sicherung.

## [2.6.7](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.7) – 2026-10-22

### Neu in 2.6.7

- **Fortschritt beim Sichern und Wiederherstellen von Spielständen:** Der Balken zeigt jetzt den echten Fortschritt (byte-genau über alle gewählten Spielstände) statt eines endlos laufenden Streifens.
- **Man sieht, was gerade passiert:** Der Schritt nennt "Spielstand 2 von 5 – Spielname (Benutzer)" bzw. beim Wiederherstellen "Sicherung 2 von 5 – Dateiname". Gilt in Web-Oberfläche und Windows-Programm.

## [2.6.6](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.6) – 2026-10-22

### Neu in 2.6.6

- **Spielstände mit Namen:** Die Spielnamen stehen jetzt in der Liste (auf neuerer Firmware aus `param.json`), die Titel-ID klein darunter.
- **Mehrfachauswahl:** Spielstände per Kästchen markieren (auch "Alle auswählen") und gemeinsam sichern - als ZIP-Download oder direkt auf der PS5: intern in `/data/game_saves` oder auf einem USB-Laufwerk in `game_saves`, je Benutzer ein Ordner.
- **Sichern an der PS5:** Ein Klick auf "Sichern ..." fragt jetzt, wohin gespeichert werden soll (vorher passierte im PS5-Browser nichts).
- **Wiederherstellen:** zeigt die auf der PS5 gefundenen Sicherungen nach Benutzer zum Markieren; alternativ eine ZIP (auch Sammel-ZIP) vom Gerät hochladen.
- **Download im Browser:** keine zusätzliche Fehlerseite mehr (parallele Zugriffe auf denselben Spielstand werden erlaubt), Fehler erscheinen als Meldung.
- Windows-Programm: dieselbe Auswahl und dieselben Dialoge; Anleitung und READMEs in fünf Sprachen angepasst.

## [2.6.5](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.5) – 2026-10-22

### Neu in 2.6.5

- **PS5-Spielstände sichern und wiederherstellen:** neue Seite "Spielstände" (Web-Oberfläche und Windows-Programm) mit Spielname, Benutzer und Größe, Benutzerfilter, Sichern als ZIP (der verschlüsselte Container bleibt unverändert) und Wiederherstellen mit Prüfsumme. Ein laufendes Spiel verhindert Änderungen. Nur PS5-Spiele.
- **Schutz beim Wiederherstellen:** Namen aus der ZIP werden geprüft (kein Pfad-Ausbruch), der alte Spielstand bleibt bei jedem Fehler erhalten; Hochladen ohne Größenbegrenzung des Arbeitsspeichers.
- **Windows-Programm:** Fenster baut sich jetzt in unter einer Sekunde statt 8-12 s auf (erstes Zeichnen und Sprach-/Designwechsel).
- Anleitung und READMEs in fünf Sprachen ergänzt; neue Tests (Spielstand-API, Fuzzing des ZIP-Lesers).

## [2.6.4](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.4) – 2026-10-21

### USB-Formatierer 2.6.4

Gründliche Fehlerprüfung der 2.6-Reihe – und jedes Laufwerk zeigt, was zuletzt mit ihm passiert ist.

#### Neu
- **„Zuletzt: …“ am Laufwerk**: In der Weboberfläche und den PC-Programmen steht unter jedem Laufwerk der letzte Auftrag, z. B. „Zuletzt: Formatieren (exFAT), 20.10.2026“ – wiedererkannt an der Seriennummer, auch wenn der Stick an einem anderen Anschluss steckt.
- **Anleitung**: neuer Abschnitt „Verlauf, Updates und Tastenkürzel“ (alle 5 Sprachen).

#### Behoben
- **Rückfragen sicherer**: Bei unumkehrbaren Fragen (Spiele löschen, Sicherung löschen, Fenster schließen während eines Auftrags) ist jetzt „Abbrechen“ vorgewählt, und Enter löst den Knopf aus, der den Fokus hat. Vorher bestätigte Enter immer.
- **Tastenkürzel** (Strg+1 … 5, F5, Strg+F) wirken während eines Auftrags oder bei offenem Dialog nicht mehr – sie konnten die gesperrte Umschaltung umgehen. Strg+H löscht in Eingabefeldern kein Zeichen mehr.
- Design „Automatisch“ baut die Oberfläche nicht mehr unter einem offenen Dialog um; nach Design- oder Sprachwechsel bleiben gewähltes Laufwerk, angehakte Spiele und Suche erhalten.
- Weboberfläche: Der Verlauf drängt das Ergebnis eines eben fertigen Auftrags nicht mehr weg; Fokus für den Controller nach Dialogen.
- „Image prüfen“: Eine `.sha256` aus einem anderen Werkzeug (anderes Format) gilt nicht mehr als „beschädigt“; Abbrechen wirkt bis zum Schluss.
- Dunkle Titelleiste gleich beim Start; der USB-Formatierer passt auch bei Full HD mit 150 % auf den Bildschirm.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-x86_64.AppImage`<br>`PS5-USB-Formatierer-aarch64.AppImage` | dasselbe für Linux (Intel/AMD bzw. ARM64): ausführbar machen und starten – mit eingebauter ELF |
| `USB-Formatierer-x86_64.AppImage`<br>`USB-Formatierer-aarch64.AppImage` | USB-Formatierer für Linux (Intel/AMD bzw. ARM64) |
| `…-linux-x86_64.tar.gz`<br>`…-linux-aarch64.tar.gz` | dieselben Linux-Programme als Archiv: entpacken und starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |
| `SHA256SUMS` | Prüfsummen aller Dateien |

## [2.6.3](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.3) – 2026-10-21

### USB-Formatierer 2.6.3

Nachsehen, was mit welchem Laufwerk passiert ist – und Downloads selbst prüfen.

#### Auftragsverlauf (neu)
- **Formatierer auf der PS5**: Knopf **„Verlauf“** oben in der Weboberfläche – die letzten 100 Aufträge mit Ergebnis (erfolgreich, abgebrochen, fehlgeschlagen), Laufwerk, Dateisystem, Bezeichnung, Dauer und Meldung. Bleibt nach einem Neustart erhalten, lässt sich leeren.
- **PC-Programme**: Knopf **„Verlauf“** in der Kopfzeile (Strg+H) mit den Aufträgen am PC; der PS5-USB-Formatierer zeigt dazu die der verbundenen PS5.

#### Prüfsummen
- Jedes Release hat jetzt eine Datei **`SHA256SUMS`** mit den Prüfsummen aller Dateien. Prüfen unter Linux: `sha256sum -c --ignore-missing SHA256SUMS`, unter Windows: `certutil -hashfile <Datei> SHA256`.
- Das Fenster **„Was ist neu“** zeigt die Prüfsumme der angebotenen Datei gleich mit an.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-x86_64.AppImage`<br>`PS5-USB-Formatierer-aarch64.AppImage` | dasselbe für Linux (Intel/AMD bzw. ARM64): ausführbar machen und starten – mit eingebauter ELF |
| `USB-Formatierer-x86_64.AppImage`<br>`USB-Formatierer-aarch64.AppImage` | USB-Formatierer für Linux (Intel/AMD bzw. ARM64) |
| `…-linux-x86_64.tar.gz`<br>`…-linux-aarch64.tar.gz` | dieselben Linux-Programme als Archiv: entpacken und starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |
| `SHA256SUMS` | Prüfsummen aller Dateien |

## [2.6.2](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.2) – 2026-10-21

### USB-Formatierer 2.6.2

Neue Versionen leichter finden – und Linux jetzt als AppImage und für ARM64.

#### Beide PC-Programme
- **„Was ist neu“**: Ein Klick auf „Neue Version … verfügbar“ unten im Fenster zeigt die Hinweise zur neuen Version und einen Knopf, der gleich die passende Datei lädt (Windows: `.exe`, Linux: AppImage bzw. Archiv für die eigene Architektur).

#### Formatierer auf der PS5 (Weboberfläche)
- **Update-Hinweis**: Unten neben der Version erscheint „Neue Version … verfügbar“, sobald es auf GitHub eine neuere Fassung gibt. Ohne Internet bleibt die Seite still – sonst ändert sich nichts.

#### Linux
- **AppImage**: eine Datei – ausführbar machen (`chmod +x`) und starten, keine Installation.
- **ARM64 (aarch64)**: beide Programme gibt es jetzt auch für ARM-Rechner, z. B. Raspberry Pi 4/5 – als AppImage und als Archiv.
- Die Archive (`.tar.gz`) gibt es weiterhin.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-x86_64.AppImage`<br>`PS5-USB-Formatierer-aarch64.AppImage` | dasselbe für Linux (Intel/AMD bzw. ARM64): ausführbar machen und starten – mit eingebauter ELF |
| `USB-Formatierer-x86_64.AppImage`<br>`USB-Formatierer-aarch64.AppImage` | USB-Formatierer für Linux (Intel/AMD bzw. ARM64) |
| `…-linux-x86_64.tar.gz`<br>`…-linux-aarch64.tar.gz` | dieselben Linux-Programme als Archiv: entpacken und starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.6.1](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.1) – 2026-10-20

### USB-Formatierer 2.6.1

Mehr Komfort in den PC-Programmen.

#### PS5-USB-Formatierer
- **Sicherungen auf der PS5 verwalten**: Die Seite „Image-Sicherung“ listet alle Sicherungen auf der PS5 (intern und auf USB-Laufwerken) – mit **Prüfen** und **Löschen**.
- **Spieleliste**: Suche nach Name oder Titel-ID, Sortierung nach Name, Größe oder Titel-ID und die Gesamtgröße der angehakten Spiele.

#### Beide Programme
- **Design Automatisch → Hell → Dunkel**: „Automatisch“ folgt dem System, auch wenn es im laufenden Betrieb umschaltet.
- **Kleine Bildschirme und viele Laufwerke**: Die Laufwerksliste scrollt; ist das Fenster höher als der Bildschirm (z. B. Full HD mit 150 %), passt es sich an.
- **Tastenkürzel**: F1 Anleitung, F5 neu einlesen, Strg+1 … 5 Seiten, Strg+F Spiele suchen.
- **Kurzinfos** an den Knöpfen – bei gesperrten Knöpfen mit dem Grund („Erst ein Laufwerk wählen“ …).

Der Formatierer auf der PS5 ist inhaltlich unverändert (nur die Versionsnummer).

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.6.0](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.0) – 2026-10-20

### USB-Formatierer 2.6.0

Neue, moderne Oberfläche der PC-Programme – hell und dunkel – und mehr rund um die Image-Sicherung.

#### Neue Oberfläche (Windows und Linux)
- **Hell und dunkel** in den Farben der PS5-Weboberfläche; beim ersten Start wie das System, oben umschaltbar.
- **PS5-USB-Formatierer mit Seitenleiste**: *An der PS5* – Formatieren, Spiele, Image-Sicherung; *Am PC* – für die PS5 oder frei formatieren. Unten der Verbindungsstatus.
- **Laufwerke als Karten** mit Marken (Dateisystem, eingebunden/frei, „gefälscht · echt …“, Anzahl Spiele) und Belegung.
- **„Wofür soll der Datenträger dienen?“** im freien Formatieren: Dateien tauschen, Ältere Geräte, Windows, Linux, Mac oder eigene Aufteilung.
- Schalter statt Häkchen, eigene Meldungsfenster im selben Design.

#### Image-Sicherung
- **Image prüfen**, ohne es zurückzuschreiben – am PC (ohne Administrator-Abfrage) und in der Weboberfläche: ganze Datei entpacken, Prüfsumme im Image und die `.sha256` daneben.
- **Sicherungen auf der PS5 löschen** in der Weboberfläche (samt `.sha256`, mit Rückfrage).

#### Außerdem
- **Hinweis auf neue Versionen** in den PC-Programmen (einmal je Start bei GitHub nachgesehen, nichts wird geladen).
- Nach dem Formatieren an der PS5 erscheint der Stick ohne „Aktualisieren“ wieder als eingebunden.
- Behoben: Wechsel von „Am PC – für PS5“ mit gewähltem Stick nach „Am PC – frei“ zeigte einen Fehler.

Vor dem Release an echter PS5 und echten Sticks durchgespielt (Formatieren, Echtheitsprüfung mit gefälschtem Stick, Tempo, Umbenennen, Sichern, Prüfen, Wiederherstellen, Löschen von Sicherungen).

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.11](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.11) – 2026-10-19

### USB-Formatierer 2.5.11

Sichern auch von schreibgeschützten Sticks und SD-Karten.

#### Image-Sicherung am PC (Windows und Linux)
- **Schreibschutzschalter an** – Sticks und SD-Karten mit eingeschaltetem Schreibschutz lassen sich jetzt als `.img.zst` sichern. Der Datenträger wird dabei nur gelesen und wie bisher gesperrt, damit nichts halb ins Image kommt.
- Formatieren, Umbenennen und Wiederherstellen lehnen schreibgeschützte Datenträger weiter mit einem klaren Hinweis ab.

#### PS5
- App-Kachel meldet Version 2.5.11 (mit „Kachel neu anmelden“ in der Weboberfläche übernehmen).

Enthält alles aus 2.5.10 (Spieleliste mit Kästchen, größeres Fenster).

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.10](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.10) – 2026-10-19

### USB-Formatierer 2.5.10

Bequemer Spiele löschen und ein größeres Fenster.

#### PS5-USB-Formatierer
- **Spieleliste mit Kästchen** – wie in der Weboberfläche: Titel links, rechts Titel-ID · Format · Größe. Ein Klick auf die Zeile hakt das Spiel an oder ab, kein Markieren mit Strg/Umschalt mehr. Dann „Angehakte Spiele löschen …“.
- **Größeres Fenster** – in allen drei Modi gleich breit wie die anderen Programme; es springt beim Umschalten nicht mehr.

Enthält alles aus 2.5.9 (Prüfung des ganzen Projekts: stabiler, sicherer, flüssiger).

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.9](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.9) – 2026-10-19

### USB-Formatierer 2.5.9

Gründliche Prüfung des ganzen Projekts: stabiler, sicherer und flüssiger.

#### Sicherheit beim Formatieren am PC
- Windows prüft unmittelbar vor dem Schreiben noch einmal, dass unter der Nummer genau der gewählte Datenträger steckt (Größe, Modell, Seriennummer) – auch vor ext2/3/4.
- Schreibfehler beim Zurückschreiben (gefälschte oder defekte Sticks) werden gemeldet statt „erfolgreich“.
- Zu kleine Partitionen werden abgelehnt, bevor der Datenträger neu aufgeteilt wird.

#### Image-Sicherung
- Neue Sicherungen merken sich die Sektorgröße – ein Image von 512-Byte-Sektoren wird auf einem 4K-Datenträger abgelehnt (er wäre danach unlesbar).
- Eine Image-Datei auf dem Datenträger selbst wird abgelehnt; scheitert ein Wiederherstellen, steht dabei, ob schon geschrieben wurde.

#### Flüssigere Programme
- Kein Einfrieren mehr beim Öffnen von „Sichern …“/„Wiederherstellen …“ im Modus „An der PS5“ und am Ende eines Downloads.
- Warnung beim Schließen während einer Übertragung zwischen PC und PS5; die Spieleliste behält Auswahl und Scrollstand.

#### Formatierer und Weboberfläche
- „Kachel neu anmelden“ bleibt nie hängen und lässt sich beim Warten abbrechen.
- Robuster bei WLAN-Aussetzern; im PS5-Browser bricht der Kopf sauber um.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.8](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.8) – 2026-10-19

### USB-Formatierer 2.5.8

Die App-Kachel zeigt nach einem Update jetzt ganz einfach die richtige Version.

#### Kachel neu anmelden
- Die PS5 liest die Version einer Kachel nur beim Anmelden. Nach einem Update des App-Ordners stand unter „Informationen“ deshalb weiter die alte Version.
- Der Formatierer erkennt das jetzt selbst: Oben in der Weboberfläche erscheint dann ein Hinweis mit dem Knopf **„Kachel neu anmelden“**. ShadowMountPlus meldet die App damit kurz ab und gleich wieder an – App-Ordner und Formatierer bleiben unverändert. Die Kachel steht danach womöglich an anderer Stelle auf dem Startbildschirm.
- Ist der App-Ordner älter als der laufende Formatierer, weist der Hinweis ebenfalls darauf hin.
- Läuft ein Spiel oder ist die App noch geöffnet, wird nichts abgemeldet.

Enthält alles aus 2.5.7 (schnellere Image-Sicherung, Versionsabgleich mit der PS5, Design und Beenden oben).

#### Nach dem Update

Den Ordner `PPSA86920` aus dem ZIP nach `/data/homebrew/` kopieren, dann den neuen Formatierer starten (z. B. „ELF senden“ im PS5-USB-Formatierer) und in der Weboberfläche auf **„Kachel neu anmelden“** drücken.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.7](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.7) – 2026-10-18

### USB-Formatierer 2.5.7

Schnellere Image-Sicherung, Versionsabgleich mit der PS5 und eine aufgeräumte Weboberfläche.

#### Image-Sicherung deutlich schneller
- Die Prüfsumme (SHA-256) rechnet jetzt der Prozessor selbst, wenn er es kann (SHA-NI – auch die PS5): gut sechsmal so schnell. Gepackt wird auf bis zu vier Kernen.
- Am PC: 2 GiB Daten sichern in **2,6 s statt 15,6 s**. Auf der PS5 bremst meist der Stick selbst – mit schnellen USB-SSDs wirkt es am stärksten.
- Die Dateien bleiben normale `.img.zst` – ältere Sicherungen lassen sich weiter wiederherstellen.

#### PS5-USB-Formatierer: Versionsabgleich
- Nach dem Verbinden fragt das Programm die Version des Formatierers auf der PS5 ab. Läuft dort eine ältere Fassung, bietet es an, die eingebaute neue zu senden. Ist die PS5 neuer, erscheint ein Hinweis, das Programm zu aktualisieren.
- Fortschritt mit Prozent auch beim Warten nach „ELF senden“ und beim Neueinlesen der Datenträger nach einem Auftrag.

#### Weboberfläche
- **Design** (hell/dunkel) und **Beenden** (Formatierer auf der PS5 beenden) sitzen jetzt oben neben Sprache, Hilfe und Aktualisieren.
- Die Spielesuche zeigt einen Balken mit Prozent.

#### Geprüft
- An der echten PS5: Sichern, Wiederherstellen (alle Testdateien gleich), exFAT-Prüfung, Download, Hochladen und Abbrechen – Kernel-Log ohne Auffälligkeiten.
- Am PC: alle Testreihen, auch mit AddressSanitizer; Windows-Helfer an Abbildern (exFAT, FAT32, ext4, Sichern und Wiederherstellen).
- Die Programme für Windows und Linux baut und prüft ab dieser Fassung die CI direkt aus dem Quelltext und hängt sie an das Release.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.6](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.6) – 2026-10-18

### USB-Formatierer 2.5.6

Man sieht jetzt, dass etwas passiert – und die Modi heißen klarer.

#### Fortschrittsbalken mit Prozent (PS5-USB-Formatierer und USB-Formatierer, Windows und Linux)
- **Einlesen der USB-Datenträger am PC:** „Lese Datenträger: WD Elements, Seagate … 40 %“. Schlafende USB-Festplatten müssen dabei erst anlaufen – das konnte bis zu 20 Sekunden ohne jede Anzeige dauern. Unter Windows werden jetzt alle Platten gleichzeitig geweckt statt nacheinander.
- **PS5-Suche im Netzwerk:** „Suche PS5 im Netzwerk … 36 %“ – auch für die erweiterte Suche in weiteren Heimnetzen.
- **Nach „ELF senden“:** Warten, bis der Formatierer auf der PS5 antwortet, mit Prozent.
- **Spielesuche auf der PS5:** „Spiele auf der PS5 werden gesucht: /mnt/usb0 … 50 %“.

#### Klarere Modus-Knöpfe
- **An der PS5** · **Am PC – für PS5** · **Am PC – frei** (vorher „USB an der PS5“, „USB am PC (für PS5)“, „Am PC Standard“) – in allen fünf Sprachen, in der Anleitung und den READMEs.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: „An der PS5“ (übers Netz), „Am PC – für PS5“, „Am PC – frei“ – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.5](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.5) – 2026-10-18

### USB-Formatierer 2.5.5

Kleiner Schliff an der Weboberfläche.

#### Weboberfläche
- Wird es im Kopf der Seite eng – schmales Browserfenster oder stark vergrößerte Ansicht –, rutschen Sprachauswahl, „Hilfe“ und „Aktualisieren“ in die nächste Zeile. Die Überschrift „USB-Formatierer“ bricht nicht mehr mitten im Wort um.

Enthält alles aus 2.5.3 und 2.5.4: Versionsanzeige in allen Programmen, Sprachauswahl oben, mkntfs 2026.9.28 und mke2fs 1.47.4.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.4](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.4) – 2026-10-18

### USB-Formatierer 2.5.4

Die Sprachauswahl der Weboberfläche steht jetzt oben.

#### Weboberfläche
- Die **Sprachauswahl** sitzt jetzt im Kopf der Seite neben „Hilfe“ und „Aktualisieren“ – im PS5-Browser und im PC-Browser. Vorher stand sie über der Seite neben dem Größenregler.
- Am Handy rutscht sie zusammen mit den Knöpfen in die nächste Zeile.

Alle Neuerungen aus 2.5.3 (Versionsanzeige in allen Programmen, mkntfs 2026.9.28, mke2fs 1.47.4) sind enthalten.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.3](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.3) – 2026-10-18

### USB-Formatierer 2.5.3

Versionsanzeige in allen Programmen – und aktuelle Werkzeuge für NTFS und ext4.

#### Versionsanzeige
- **Weboberfläche** (PS5-Browser und PC-Browser): unten neben dem Entwickler steht jetzt die Version des laufenden Formatierers, z. B. „v2.5.3“.
- **PS5-USB-Formatierer und USB-Formatierer** (Windows und Linux): Version in der Fußzeile.
- **App-Kachel auf der PS5**: Die Version erscheint unter „Informationen“ (02.005.003).

#### Aktualisierte Werkzeuge
- **NTFS auf der PS5:** mkntfs aus ntfs-3g **2026.9.28** (vorher 2022.10.3) – mit den Fehlerbehebungen der letzten Jahre, u. a. mehrere use-after-free.
- **ext2/ext3/ext4 auf der PS5 und am PC:** mke2fs aus e2fsprogs **1.47.4** (vorher 1.47.1).
- Die Windows-Programme sind mit den aktuellen Bauwerkzeugen gebaut (Pillow 13).

#### Geprüft
- An der echten PS5: einen Stick nacheinander mit NTFS, ext4 und wieder exFAT formatiert, jeweils mit Echtheits- und Tempoprüfung; im Kernel-Log keine Auffälligkeiten.
- Am PC: ext2, ext3 und ext4 mit dem neuen mke2fs.exe, von e2fsck als fehlerfrei bestätigt.
- Alle Testreihen und die CI unter Linux.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.2](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.2) – 2026-10-17

### USB-Formatierer 2.5.2

Image-Sicherung jetzt auch im PC-Programm für Sticks an der PS5 – und deutlichere Meldungen.

#### PS5-USB-Formatierer (Windows und Linux): Image-Sicherung im Modus „USB an der PS5“
- Neue Zeile „Image-Sicherung: Sichern … / Wiederherstellen …“ auch für Sticks, die an der PS5 stecken.
- **Sichern nach:** intern auf der PS5, auf ein anderes USB-Laufwerk an der PS5 – oder **auf diesen PC**: Die Datei wird heruntergeladen, die `.sha256` daneben gelegt und die Prüfsumme nachgerechnet.
- **Wiederherstellen aus:** einer Sicherung auf der PS5 – oder **einer Datei vom PC**, die dabei hochgeladen wird. Mit derselben Sicherheitsabfrage wie beim Formatieren.

#### Weboberfläche
- Wird ein Auftrag abgelehnt (z. B. weil ein Spiel läuft, ein anderer Stick steckt oder die Größe nicht passt), erscheint jetzt ein Dialog „Auftrag nicht gestartet“ mit dem Grund – am Fernseher war die Meldung unten auf der Seite leicht zu übersehen.

#### Geprüft an der echten PS5
- Die Sperre aus 2.5.1 mit einem laufenden Spiel: Prüfen, Sichern und Download wurden abgelehnt („bitte zuerst beenden“), der Stick blieb eingebunden, im Kernel-Log kein Abmelden.
- Das PC-Programm hat einen Stick an der PS5 auf den PC gesichert und per Hochladen zurückgeschrieben.
- Alle Testreihen, auch mit AddressSanitizer, und die CI unter Linux.

#### Aktualisieren

Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.1](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.1) – 2026-10-17

### USB-Formatierer 2.5.1

Schneller bei großen Laufwerken, sicherer bei laufenden Spielen.

#### Image-Sicherung: leere Bereiche kosten keine Zeit mehr
- Lange freie Strecken (ab 32 MB) werden nicht mehr als Nullen gepackt, sondern direkt als „hier ist alles leer“ vermerkt – und beim Wiederherstellen übersprungen. Ein 62-GB-Stick mit 266 MB Daten ist an der PS5 jetzt in **8 statt 53 Sekunden** gesichert und in 16 statt 35 Sekunden zurückgeschrieben; eine fast leere 16-GB-Platte braucht nur noch Millisekunden. Bei großen Festplatten spart das bis zu einer Viertelstunde.
- Die Datei bleibt eine gewöhnliche zstd-Datei: `zstd -d` entpackt sie zum vollständigen Image.
- Sicherungen aus 2.5.0 lassen sich weiter wiederherstellen.

#### PS5: Sperre bei laufenden Spielen
- Läuft ein Spiel oder eine App, meldet der Formatierer eingebundene Laufwerke nicht mehr ab (Formatieren, Prüfen, Umbenennen, Sichern, Wiederherstellen) und sagt stattdessen „bitte zuerst beenden“ – geprüft beim Auftrag und noch einmal unmittelbar vor dem Abmelden. Die PS5 nutzt eingebundene Laufwerke in laufenden Spielen mit; sie dann abzumelden konnte das Spiel und die PS5 durcheinanderbringen. Nicht eingebundene Laufwerke sind davon nicht betroffen.
- Nach dem Abmelden behält der Formatierer das Laufwerk mindestens 5 Sekunden, bevor er es der PS5 zurückgibt. Bei sehr kurzen Aufträgen (z. B. exFAT prüfen, sofort abbrechen) kam der AutoMounter der PS5 sonst gelegentlich durcheinander.

#### Sonstiges
- README mit neuen Bildern (auch „Sichern & wiederherstellen“), CI auf aktuelle GitHub-Actions umgestellt.

Getestet mit allen Testreihen (auch mit AddressSanitizer), in der CI unter Linux (mit dem zstd-Programm) und an der echten PS5: Sichern, Wiederherstellen, Download, Hochladen, Abbrechen und wiederholte kurze Aufträge – Kernel-Log ohne AutoMounter-Fehler.

#### Aktualisieren

Payload und App haben sich geändert. Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.5.0](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.0) – 2026-10-17

### USB-Formatierer 2.5.0

Neu: **Image-Sicherung** – ganze Laufwerke sichern und genau so wiederherstellen, an der PS5 und am PC.

#### Image sichern und wiederherstellen
- **Klein und schnell:** Gesichert wird nur, was belegt ist – freier Platz von exFAT und FAT32 kostet nichts, gepackt wird mit zstd. Ein 62-GB-Stick mit 266 MB Daten wird zur 258-MB-Datei. Bei gefälschten Sticks wird der Bereich ohne echten Speicher gar nicht gelesen.
- **Offenes Format:** `Modell_Bezeichnung_Datum.img.zst` ist eine gewöhnliche zstd-Datei – `zstd -d` macht daraus ein rohes Image für jedes Werkzeug (balenaEtcher, dd …). Daneben liegt die Prüfsumme als `.sha256`.
- **Geprüft:** Beim Wiederherstellen werden die Prüfsummen des Images kontrolliert und danach alles Geschriebene noch einmal gelesen – ein defekter oder gefälschter Stick fällt auf.
- **An der PS5** (Knopf „Sichern & wiederherstellen“): sichern nach intern (`/data/usbfmt-backup`), auf ein anderes eingebundenes USB-Laufwerk oder – am PC/Handy – als Download; wiederherstellen aus diesen Ordnern oder per Hochladen. Eingebundene Laufwerke werden dafür kurz per Software abgemeldet, wie beim Formatieren.
- **Am PC** (USB-Formatierer und „Am PC Standard“, Windows und Linux): Zeile „Image-Sicherung“ mit „Sichern …“ und „Wiederherstellen …“ über den gewohnten Dateidialog.
- Schutzregeln: nie auf das Laufwerk sichern, das gesichert wird; ein Image, das größer ist als das Ziel (oder als der echte Teil eines gefälschten Sticks), wird abgelehnt.

Getestet mit allen Testreihen (auch mit AddressSanitizer), in der CI unter Linux, im Browser und an echten Geräten: an der PS5 mit Sichern, Wiederherstellen, Download, Hochladen und Abbrechen (Kernel-Log sauber), am PC an einem gefälschten 124-GB-Stick (15,7 GB echt) – die Sicherung ließ sich mit dem normalen zstd vollständig entpacken.

#### Aktualisieren

Payload und App haben sich geändert. Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus, Image-Sicherung |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

Neu eingebaut: zstd 1.5.7 (Meta Platforms, BSD-Lizenz).

## [2.4.9](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.9) – 2026-10-17

### USB-Formatierer 2.4.9

Aktualisierte Dokumentation – Programme, Payload und App sind unverändert gegenüber 2.4.8.

#### Neu in der README (alle fünf Sprachen)
- Funktionen ergänzt: **„ELF senden“** (das PC-Programm schickt den eingebauten Formatierer an die PS5 und ersetzt eine ältere Fassung), **PS5-Suche** auch aus WSL oder einer virtuellen Maschine, **Schutz vor Verwechslung** (Seriennummer-Abgleich, Aufräumen nach dem Befund „gefälscht/defekt“).
- Schnellstart: ohne App-Kachel einfach im PS5-USB-Formatierer auf „ELF senden“ klicken.
- Alle Bilder neu – mit dem Knopf „Spiele löschen …“, „ELF senden“ und der Entwickler-Fußzeile.

Alles in 2.4.8 Enthaltene ist an der echten PS5, unter Windows und unter Linux mit echten USB-Sticks getestet.

#### Aktualisieren

Wer 2.4.8 hat, muss nichts tun – die Dateien sind dieselben. Sonst: Am einfachsten im PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.8](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.8) – 2026-10-17

### USB-Formatierer 2.4.8

Korrekturen aus einer fünften Durchsicht – vor allem Partitions-Editor (Expertenmodus) und Echtheitsprüfung.

#### PC-Programme (Windows und Linux)
- **Expertenmodus:** Es lässt sich nur noch eine Partition startfähig machen (vorher konnten beim Hinzufügen zwei entstehen – eine ungültige MBR-Tabelle). Bei GPT entfällt „startfähig“.
- **Keine Löschung mehr vor einem sicheren Fehler:** FAT32/FAT16 hinter 2 TB und NTFS auf zu kleinen Partitionen bzw. mit zu großer Zuordnungseinheit werden jetzt vorher abgelehnt – bisher wurde der Datenträger erst neu aufgeteilt (alles gelöscht) und dann scheiterte das Formatieren; unter Linux konnte mkntfs dabei endlos laufen.
- Der Bearbeiten-Dialog ändert die Größe einer Partition nicht mehr still durch Rundung; „Standard“ als Zuordnungseinheit bleibt „Standard“.
- **Umbenennen** gibt es nur noch bei genau einem Laufwerk auf dem Datenträger – vorher konnte bei mehreren Partitionen ein anderes umbenannt werden als angezeigt.

#### Echtheitsprüfung (PS5 und PC)
- Ein toter Stick wird als **defekt** gemeldet statt als „gefälscht, echt 0 Bytes“; ein echter Stick mit defektem Anfang landet nicht mehr fälschlich in der Liste gefälschter Sticks.
- Gefälschte Sticks, die hinter der echten Grenze mit Fehlern antworten, erkennt auch die vollständige Prüfung als **gefälscht** – mit dem Angebot, nur den echten Bereich zu formatieren.
- Nach dem Befund „gefälscht“ oder „defekt“ wird das alte Dateisystem entfernt: Die Prüfung hat Teile davon überschrieben, die PS5 hätte es sonst mit kaputten Dateien wieder eingebunden.
- PS5-App: Die Rückfragen danach („Nur echten Bereich formatieren“, „Trotzdem formatieren“, „Reparieren“) nehmen den aktuellen Stand des Sticks.

Geprüft mit allen Testreihen (auch mit AddressSanitizer, mit nachgebildeten gefälschten und toten Sticks), in der CI unter Linux, im Browser und an der echten PS5 (per „ELF senden“ eingespielt, Kernel-Log sauber).

#### Aktualisieren

Payload und App haben sich geändert. Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.7](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.7) – 2026-10-17

### USB-Formatierer 2.4.7

Korrekturen aus einer vierten Durchsicht von PC-Programmen und PS5-App.

#### PC-Programme (Windows und Linux)
- **„ELF senden“ zuverlässiger:** Antwortet die PS5 bei der Vorprüfung nicht, wird nichts gesendet und das klar gemeldet. Vorher konnte die ELF geschickt werden, ohne den laufenden Formatierer zu beenden – die neue Fassung startete dann nicht, gemeldet wurde aber Erfolg.
- Lehnt die PS5 das Beenden ab (inzwischen läuft ein Auftrag), bleibt die Verbindung bestehen und der Auftrag wird weiter angezeigt. Wird das Fenster während des Sendens geschlossen, fragt das Programm nach.
- **PS5-Suche unter Linux wieder schnell** (etwa 2 s statt 5–9 s). Hinter dem NAT von WSL kommen jetzt auch die Antworten der Konsolen-Suche an.

#### PS5-App
- **Nach einer Prüfung** („Nur echten Bereich formatieren“, „Reparieren“) gilt die Folgeaktion immer dem geprüften Stick. Wurde inzwischen ein anderer Stick unter demselben Namen eingesteckt, wird er nicht angefasst.
- Fehlt die Seriennummer eines Sticks kurz (frisch eingesteckt), schützt die PS5 trotzdem vor einem getauschten baugleichen Stick; ein inzwischen eingebundener Stick wird nicht von selbst ausgewählt.
- Controller: Nach „Abbrechen“ im Lösch-Auswahlmodus während eines Auftrags bleibt der Fokus erhalten.
- **App-Kachel:** Geht der Browser nicht auf, erscheint jetzt eine Benachrichtigung statt gar nichts.

Geprüft mit allen Testreihen (auch mit AddressSanitizer), in der CI unter Linux, im Browser und an der echten PS5 (Payload per „ELF senden“ getauscht, Kernel-Log sauber).

#### Aktualisieren

Payload und App haben sich geändert. Am einfachsten im neuen PS5-USB-Formatierer auf „ELF senden“ klicken; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.6](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.6) – 2026-10-17

### USB-Formatierer 2.4.6

Der PS5-USB-Formatierer (Windows und Linux) kann den Formatierer jetzt auf Knopfdruck an die PS5 schicken.

#### Neu: „ELF senden“
- Im Modus „USB an der PS5“ steht neben „Suchen“ der Knopf **„ELF senden“**. Er schickt die in das Programm eingebaute `usb_format.elf` an den ELF-Loader der PS5 (Port 9021) – eine separate Datei ist nicht nötig.
- Läuft auf der PS5 schon ein Formatierer, fragt das Programm nach und **ersetzt** ihn durch die eingebaute Fassung (erst sauber beenden, dann senden). So lässt sich eine ältere Fassung auf der PS5 einfach aktualisieren. Während eines laufenden Auftrags wird nichts gesendet.
- **Klare Meldungen**, wenn es nicht klappt: Port 9021 geschlossen (auf der PS5 läuft kein ELF-Loader – Jailbreak starten), PS5 antwortet nicht, Übertragung abgebrochen. Das gilt auch für das automatische Senden beim Verbinden.

Getestet an der echten PS5 (laufenden Formatierer ersetzt, nach 4 Sekunden wieder verbunden, Kernel-Log sauber) und gegen einen nachgebauten ELF-Loader (auch Port zu, PS5 nicht erreichbar, Auftrag läuft).

#### Aktualisieren

Payload und App haben sich nur in der Anleitung geändert (Hinweis auf „ELF senden“). Wer das neue Programm nutzt, kann die PS5 direkt mit „ELF senden“ aktualisieren; für die App-Kachel den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard – mit eingebauter ELF |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten – mit eingebauter ELF |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload – nur nötig, um ihn ohne die PC-Programme von Hand an Port 9021 zu schicken |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.5](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.5) – 2026-10-17

### USB-Formatierer 2.4.5

Die PC-Programme finden die PS5 jetzt automatisch – unter Windows und Linux, auch in Umgebungen, in denen das bisher nicht klappte.

#### PS5-Suche
- **Hinter einem NAT** (Linux in WSL im Standardmodus, virtuelle Maschinen): Findet die normale Suche nichts, fragt das Programm die üblichen Heimnetze direkt (FritzBox 192.168.178.x, 192.168.0.x, 192.168.1.x, 10.0.0.x …). Das dauert nur dann etwa 5 Sekunden länger.
- **WSL:** Antworten auf Broadcasts kommen dort nicht an – die PS5 wird jetzt zusätzlich gezielt gefragt. So findet die Linux-Fassung die PS5 auch dann, wenn der Formatierer noch nicht läuft, und zeigt ihren Namen.
- Wird eine PS5 über die Konsolen-Suche gefunden, prüft das Programm direkt, ob der Formatierer schon läuft – der Payload wird nicht unnötig ein zweites Mal geschickt.

Getestet an der echten PS5 unter Windows, unter WSL („mirrored“ und NAT), mit und ohne laufenden Formatierer. Außerdem wurde die Linux-Fassung erstmals an einem echten USB-Stick geprüft (NTFS, FAT16, ext4, exFAT, HFS+, gemischte GPT/MBR-Partitionen, Umbenennen, Seriennummer-Schutz – alle mit fsck geprüft).

#### Aktualisieren

Nur die PC-Programme haben sich geändert. Payload (`usb_format.elf`) und App-Kachel (`PPSA86920.zip`) sind dieselben wie in 2.4.4 – auf der PS5 muss nichts erneuert werden.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) – unverändert seit 2.4.4 |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren – unverändert seit 2.4.4 |

## [2.4.4](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.4) – 2026-10-17

### USB-Formatierer 2.4.4

Neue Funktionen aus den offenen Punkten, ein deutlicher Knopf zum Löschen von Spielen und Korrekturen aus einer dritten Durchsicht.

#### Neu
- **Spiele löschen in der PS5-App:** „Spiele löschen …“ steht jetzt als deutlicher Knopf unter der Spieleliste (wie im Windows-Programm). Spiele anhaken, „Löschen“, auf Wunsch samt Kachel auf der Startseite. Mit dem Controller bedienbar.
- **Seriennummer:** Steckt unter derselben Nummer bzw. demselben Namen inzwischen ein baugleicher anderer Stick, lehnen PS5 und PC-Helfer den Auftrag ab („Liste neu laden“), statt den falschen Stick zu formatieren.
- **Umstecken an der PS5:** Fremde Sticks werden vor dem exklusiven Öffnen nur lesend auf ihre Größe geprüft.
- **PS5-Suche** nutzt die echte Netzmaske des PCs (bis /22 vollständig), Linux-Fenster stehen mittig auf dem richtigen Monitor.
- **Entwickler-Fußzeile** mit GitHub-Profil in der Weboberfläche und in beiden PC-Programmen.

#### Korrekturen
- **Linux:** Beide Programme starteten nicht, wenn `xrandr` installiert ist (Fehler aus der Mehrmonitor-Anpassung) – behoben und mit Test abgesichert.
- **Anderer Datenträger am selben Anschluss:** Die Spieleliste merkt das jetzt und sucht neu. Vorher konnte die Warnung „Darauf liegen Spiele“ beim Formatieren fehlen.
- **Löschen:** Liegt unter dem Pfad seit der Suche ein anderes Spiel (z. B. per FTP ersetzt), wird nichts gelöscht.
- **FAT32-Bezeichnung mit Umlauten** (alte Windows-Codepage): Das Windows-Programm konnte sich dann nicht mit der PS5 verbinden – behoben.
- Seriennummern mit Sonderzeichen sperren einen Stick nicht mehr dauerhaft.

Geprüft mit allen Testreihen (auch mit AddressSanitizer), in der CI als root an Loop-Geräten und unter Xvfb, mit dem Gerätetest am echten Stick unter Windows (Seriennummer, 8 Schritte, chkdsk, e2fsck, fsck.hfsplus) und mit dem Software-Abziehen an der PS5.

#### Aktualisieren

Payload und App haben sich geändert: Den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren. Läuft noch ein älterer Formatierer, vorher in der Weboberfläche „Formatierer auf der PS5 beenden“ wählen.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.3](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.3) – 2026-10-16

### USB-Formatierer 2.4.3

Fehlerkorrekturen aus einer zweiten Durchsicht (PS5-Formatierer, PC-Programme, Weboberfläche, Zusammenspiel der Teile).

#### An der PS5
- **Umstecken zuverlässiger:** Ein noch nicht abgeräumter Eintrag des alten Laufwerks führte manchmal zu „Stick von Hand abziehen“ oder „Die PS5 war schneller“ – jetzt wird es erneut versucht. Ein gleich großer fremder Stick ohne Seriennummer wird nicht mehr für den eigenen gehalten.
- **NTFS/ext4:** Schreibt mkntfs auf einem langsamen Stick minutenlang ohne Ausgabe, meldet der Formatierer nicht mehr vorschnell einen Fehler, während das Werkzeug noch weiterschreibt.
- **exFAT prüfen:** Ein Fehler beim Aufräumen konnte eine andere Verbindung des Formatierers stören – behoben.
- **Spiele löschen:** Die Antwort von ShadowMountPlus wird auch dann vollständig erkannt, wenn es die Verbindung offen lässt.
- Gefälschter Stick mit sehr wenig echtem Speicher: verständliche Meldung statt eines technischen Fehlers.

#### PC-Programme (Windows und Linux)
- **Mit eingetragenem Proxy** findet und erreicht das Programm die PS5 jetzt; die Suche hängt nicht mehr an Geräten mit endloser Antwort und folgt keinen Umleitungen.
- **Dateisystem-Auswahl** entspricht genau dem, was der Helfer kann (FAT32 über 2 TB, exFAT unter 8 MB, FAT16-Grenzen, Datenträger unter 16 MB) – keine Auswahl mehr, die erst beim Formatieren scheitert.
- Linux: Umbenennen von FAT16 funktioniert wieder mit älteren dosfstools (z. B. Ubuntu 20.04); die Hilfe-Kopie liegt in einem eigenen Ordner.
- Meldungen mit Sonderzeichen bringen das Programm nicht mehr aus dem Tritt; Rückfragen erscheinen auf dem Monitor des Programms.

#### Weboberfläche
- Eine Reparatur, die Fehler übrig ließ, zeigt jetzt „Fehler ließen sich nicht beheben“ statt „Prüfen nicht möglich“.
- Tempo-Bewertung und einige Meldungen („leer“, „unbekannt“ …) jetzt in allen Sprachen.

Geprüft mit allen Testreihen (auch mit AddressSanitizer), in der CI als root an Loop-Geräten, mit dem Gerätetest am echten Stick unter Windows (8 Schritte, chkdsk, e2fsck, fsck.hfsplus) und mit dem Software-Abziehen an der PS5.

#### Aktualisieren

Payload und App haben sich geändert: Den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren. Läuft noch ein älterer Formatierer, vorher in der Weboberfläche „Formatierer auf der PS5 beenden“ wählen.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.2](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.2) – 2026-10-16

### USB-Formatierer 2.4.2

Fehlerkorrekturen aus einer Durchsicht aller Teile (PS5-Formatierer, Weboberfläche, PC-Programme, Helfer, App).

#### Sicherheit und richtiges Laufwerk
- Eingelöteter eMMC-Speicher (z. B. in manchen Laptops) gilt nicht mehr als SD-Karte und wird nicht mehr angeboten – unter Windows und Linux.
- Windows: Laufwerke, die sich auch über einen anderen Datenträger erstrecken (übergreifende Volumes), werden abgelehnt.
- Nach dem Aktualisieren der Liste bleibt ein Laufwerk nur ausgewählt, wenn Name, Größe und Modell gleich sind – bekommt ein anderer Datenträger den Namen (etwa die Spiele-SSD), ist nichts mehr ausgewählt.
- Linux: Der Helfer legt seine Ergebnisdatei nur noch in einem Ordner des Benutzers an.

#### Abbrechen und Aufträge
- Wird abgebrochen, bevor die Prüfung etwas geschrieben hat, bleibt der Datenträger unverändert – vorher wurde trotzdem formatiert.
- Der Abbrechen-Knopf der PC-Programme bleibt nach einem Abbruch nicht mehr gesperrt.
- Weboberfläche: kein doppelter Start durch Doppelklick, „Jetzt reparieren“ trifft immer das geprüfte Laufwerk, die Hilfe am Fernseher wartet, bis ein Auftrag fertig ist.

#### Spiele
- Wird ein Laufwerk mit Spielen eingesteckt, während der Formatierer schon läuft, sucht er die Spiele jetzt selbst neu – vorher konnte die Warnung vor Spielen beim Formatieren fehlen.
- Spiele löschen: Unvollständige Antworten von ShadowMountPlus gelten als Fehler, Pfade werden über die Datei selbst verglichen – ein eingebundenes Spiel wird nie für eine zweite Kopie gehalten.

#### Dateisysteme und Expertenmodus
- MBR-Partitionen auf Datenträgern über 2 TB enden korrekt innerhalb der MBR-Grenze; HFS+ auf sehr großen Partitionen rechnet richtig.
- FAT32 umbenennen findet die Bezeichnung auch in späteren Teilen des Wurzelordners.
- Expertenmodus: Passt ein Dateisystem oder eine Zuordnungseinheit nicht zur Partitionsgröße, wird das gleich angezeigt statt erst beim Formatieren zu scheitern.

#### Sonstiges
- App-Kachel: hängt nicht mehr, wenn der ELF-Loader nicht antwortet.
- Unlesbare Herstellernamen billiger Sticks legen die Laufwerksliste der PC-Programme nicht mehr lahm.
- Geprüft mit allen Testreihen (auch mit AddressSanitizer), in der CI als root an Loop-Geräten und mit dem Gerätetest am echten Stick unter Windows.

#### Aktualisieren

Payload und App haben sich geändert: Den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren. Läuft noch ein älterer Formatierer, vorher in der Weboberfläche „Formatierer auf der PS5 beenden“ wählen.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.1](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.1) – 2026-10-16

### USB-Formatierer 2.4.1

- **README:** Der Hinweis auf eine geplante Mac-Fassung ist entfernt – es wird keine Mac-Fassung geben. Die Programme formatieren Datenträger für Macs (HFS+) weiterhin unter Windows und Linux.

Programme, Payload und App sind unverändert gegenüber 2.4.0 (dieselben Dateien). Wer 2.4.0 installiert hat, muss nichts aktualisieren.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.4.0](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.0) – 2026-10-16

### USB-Formatierer 2.4.0

- **Neu: Linux-Fassung** beider PC-Programme (`PS5-USB-Formatierer` und `USB-Formatierer`, x86_64) – gleiche Oberfläche, gleiche Funktionen wie unter Windows. Zum Formatieren fragt das System nach dem Administrator-Passwort (pkexec/polkit). NTFS, FAT16 und ext2/3/4 schreiben die Werkzeuge des Systems (Pakete `ntfs-3g`, `dosfstools`, `e2fsprogs`). Gebaut auf Ubuntu 22.04, läuft daher auch auf älteren Verteilungen; getestet in der CI auch als root an Loop-Geräten.
- **Fenster mittig:** Die PC-Programme öffnen sich mittig auf dem Bildschirm und bleiben ganz sichtbar, auch wenn sie größer werden.
- **Windows:** ext2/3/4 direkt nach einer neu angelegten FAT16- oder NTFS-Partition scheiterte manchmal, weil Windows das Laufwerk kurz offen hielt – mke2fs versucht es jetzt erneut. Gerätetest am echten Stick (8 Schritte, chkdsk, e2fsck, fsck.hfsplus) bestanden.
- **Anleitung** (Knopf „Hilfe“) und README in allen fünf Sprachen um Linux ergänzt.

#### Aktualisieren

Payload und App enthalten die erweiterte Anleitung: Den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren. Läuft noch ein älterer Formatierer, vorher in der Weboberfläche „Formatierer auf der PS5 beenden“ wählen.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./PS5-USB-Formatierer` starten |
| `USB-Formatierer-linux-x86_64.tar.gz` | dasselbe für Linux: entpacken, `./USB-Formatierer` starten |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.3.1](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.3.1) – 2026-10-15

### USB-Formatierer 2.3.1

- **Warnhinweis kürzer:** In der Weboberfläche und im Windows-Programm steht ganz oben nur noch die eine Zeile „Während der Nutzung keine USB-Datenträger abziehen oder neu anschließen“. Die Erklärung dazu steht in der Anleitung (Knopf „Hilfe“ › Wichtig vorab).
- **Windows-Programm:** In der Spieleliste ist die Art „Ordner“ jetzt in allen fünf Sprachen übersetzt.
- **Lizenz:** Das Projekt steht jetzt unter der GNU General Public License 3.0 (`LICENSE`); enthaltener Fremdcode behält seine eigene Lizenz.
- **Neue README** in fünf Sprachen mit Bildern aller Oberflächen.

Alle Neuerungen von 2.3.0 (fünf Sprachen, Anleitung, „Am PC Standard“, eigenes `USB-Formatierer.exe`, Expertenmodus) siehe dort.

#### Aktualisieren

Payload und App haben sich geändert: Den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren. Läuft noch ein älterer Formatierer, vorher in der Weboberfläche „Formatierer auf der PS5 beenden“ wählen.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.3.0](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.3.0) – 2026-10-15

### USB-Formatierer 2.3.0

#### Neu

- **Fünf Sprachen:** Deutsch, English, Italiano, Español und Français in allen Oberflächen – Weboberfläche (Auswahl oben, gilt für PS5- und PC-Browser gleich), Meldungen der PS5 und beide Windows-Programme (Auswahl oben rechts).
- **Anleitung mit Knopf „Hilfe“:** eine bebilderte Anleitung in allen fünf Sprachen für App, Payload und beide Windows-Programme – in der Weboberfläche und in den Windows-Programmen jeweils oben.
- **„Am PC Standard“ und eigenes Programm `USB-Formatierer.exe`:** USB-Datenträger am PC frei formatieren, ohne PS5-Vorgaben – exFAT, FAT32, NTFS, FAT (FAT16), ext4/ext3/ext2 (Linux) und HFS+ (Mac), Zuordnungseinheit frei wählbar, Partitionstabelle MBR oder GPT, Schnell- oder vollständige Formatierung, Echtheit und Tempo prüfen, Umbenennen.
- **Expertenmodus:** mehrere Partitionen anlegen (MBR bis 4, GPT bis 16) – je Partition Dateisystem, Größe, Zuordnungseinheit, Typ (Windows, Linux, Swap, EFI, Apple), Bezeichnung und bei MBR „startfähig“.
- **Warnhinweis** zum Umstecken an der PS5 steht jetzt ganz oben in der Kachel.

#### Getestet

- Am PC mit echtem Stick: NTFS, FAT16, Umbenennen, ext4, HFS+, GPT mit 5 Partitionen (exFAT, NTFS, FAT32, HFS+, ext4), MBR mit FAT16 (startfähig), ext3 und exFAT – Windows-chkdsk sowie e2fsck und fsck.hfsplus unter Linux ohne Fehler.
- An der PS5: neue Weboberfläche, Anleitung und übersetzte Meldungen; alle 13 Testreihen des Payloads.

#### Aktualisieren

Payload und App haben sich geändert: Den Ordner `PPSA86920` aus dem ZIP erneut nach `/data/homebrew/` kopieren (die App startet den mitgelieferten Payload). Läuft noch ein älterer Formatierer, vorher in der Weboberfläche „Formatierer auf der PS5 beenden“ wählen.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm: USB an der PS5 (übers Netz), USB am PC für die PS5, Am PC Standard |
| `USB-Formatierer.exe` | Windows-Programm ohne PS5-Funktionen: alle Dateisysteme, Partitionen, Expertenmodus |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

ext2/3/4 erzeugt `mke2fs.exe` aus e2fsprogs (GPL, in den Windows-Programmen enthalten, Lizenzhinweis liegt bei).

## [2.2.1](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.2.1) – 2026-10-15

### USB-Formatierer 2.2.1

**Korrektur für den PC-Modus des Windows-Programms.** In 2.2.0 meldete „USB an diesem PC“ nach der Windows-Abfrage (UAC) nur „Der Helfer wurde unerwartet beendet“: Das Programm durfte die Ergebnisdatei des Helfers nicht lesen (Rechte des Arbeitsordners). Behoben und am PC mit echtem Stick getestet – Tempo messen, Echtheitsprüfung (gefälschter Stick erkannt, echte Größe exakt wie auf der PS5), nur den echten Bereich formatieren mit Ordnervorlage, Umbenennen.

Außerdem: Kann der Helfer seine Ergebnisdatei nicht anlegen, zeigt das Programm jetzt einen eindeutigen Fehlercode.

PS5-Payload und App sind gegenüber 2.2.0 unverändert (zur Vollständigkeit erneut beigefügt). Alle Neuerungen von 2.2.0 siehe dort.

| Datei | Zweck |
|---|---|
| `PS5-USB-Formatierer.exe` | Windows-Programm (PS5 im Netz oder USB am PC) |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |

## [2.2.0](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.2.0) – 2026-10-15

> **Hinweis:** Im Windows-Programm funktioniert der PC-Modus in dieser Version nicht – bitte [2.2.1](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.2.1) verwenden.

### USB-Formatierer 2.2.0

#### Neu
- **USB an diesem PC** (Windows-Programm): Sticks direkt am PC formatieren – mit derselben Technik wie auf der PS5 (exFAT 64 KB / FAT32 auch über 32 GB, MBR/GPT, Ordnervorlage, Echtheitsprüfung, Tempo messen, Umbenennen). Administratorrechte nur für den Auftrag (UAC), nur USB-/SD-Datenträger, nie die Systemplatte.
- **Spiele löschen** (Web-UI und Windows-Programm): Spiele auswählen und löschen – über ShadowMountPlus, das eingebundene Spiele selbst freigibt; Kachel auf Wunsch mit entfernen. Zweite Kopien doppelter Spiele werden sicher erkannt.
- **Spieleliste im Windows-Programm** und vollständige Laufwerkstexte.
- **Gefälschte Sticks** werden gemerkt (Seriennummer) und gekennzeichnet; Formatieren nutzt dann automatisch nur den echten Bereich. Hinweis: für Spiele nicht empfohlen.
- **Warnhinweis**: während der Nutzung keine USB-Datenträger abziehen oder neu anschließen.

#### Verbessert
- Fortschritt ohne Stillstand: Formatieren, Löschen, Tempo messen, Echtheitsprüfung (mit Restzeit); Lauf-Balken, wo noch keine Prozente vorliegen.
- Echtheitsprüfung schätzt die echte Größe gefälschter Sticks nicht mehr zu hoch; ein abgezogener Stick gilt nicht mehr als „gefälscht“.
- Bedienung mit dem PS5-Controller: Fokus bleibt erhalten, Fortschritt wird eingeblendet, Abzieh-Aufforderung nennt das Laufwerk.
- Windows-Programm: Bestätigung mit Häkchen bei Spielen/Daten, Abbrechen auch im PS5-Modus, Netzaussetzer ohne Fehlalarm.

#### Stabilität und Sicherheit
- Server bearbeitet Verbindungen parallel; abgebrochene oder langsame Verbindungen legen ihn nicht mehr lahm.
- Zeitgrenzen gegenüber ShadowMountPlus, atomares Speichern, Content-Security-Policy, strenge Prüfung von Daten von USB-Sticks.
- Windows-Helfer wird vor dem Start gesperrt und per Prüfsumme kontrolliert; Ausgabedateien nur am verlangten Ort.
- Geprüft mit AddressSanitizer, UBSan und ThreadSanitizer; am Gerät getestet.

#### Dateien
| Datei | Zweck |
|---|---|
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PS5-USB-Formatierer.exe` | Windows-Programm (PS5 im Netz oder USB am PC) |

## [2.1.7](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.7) – 2026-10-13

Windows-Programm: Der Fortschrittsbalken wächst jetzt sichtbar mit. Aufträge, die im Browser oder auf der PS5 gestartet wurden, werden automatisch erkannt und angezeigt. Wartephasen ohne Prozentangabe zeigen einen laufenden Balken. PS5-Payload und App unverändert.

## [2.1.6](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.6) – 2026-10-13

Fortschritt wird jetzt in jedem offenen Browser angezeigt, auch wenn der Auftrag woanders (z. B. an der PS5) gestartet wurde. Sonst wie 2.1.5.

## [2.1.5](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.5) – 2026-10-13

Auswahlfelder mit eigenem, dezentem Pfeil statt des grellweißen Browser-Pfeils der PS5. Sonst wie 2.1.4.

## [2.1.4](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.4) – 2026-10-13

Hauptkarte ohne Bild, leicht durchsichtig und weichgezeichnet (Platinen-Hintergrund scheint ruhig durch). Sonst wie 2.1.1.

## [2.1.3](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.3) – 2026-10-13

Hauptkarte ohne Bild und leicht durchsichtig – der Platinen-Hintergrund scheint dezent durch. Sonst wie 2.1.1.

## [2.1.2](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.2) – 2026-10-13

Hauptkarte der Weboberfläche ohne Hintergrundbild (ruhiger). Sonst wie 2.1.1.

## [2.1.1](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.1) – 2026-10-13

Echtheitstest deutlich schneller auf billigen Sticks (höchstens 128 Stichproben) und mit Restzeit-Anzeige. Sonst wie 2.1.0.

## [2.1.0](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.0) – 2026-10-13

### Neu

- **Weboberfläche in zwei Spalten:** links Laufwerke, Inhalt und Werkzeuge (Tempo messen, Prüfen & reparieren, Umbenennen), rechts Formatieren. Bei schmalem Fenster bzw. großer Schrift stehen die Spalten untereinander.
- Spieleliste zeigt drei Spiele, der Rest ist aufklappbar.
- Größenregler oben über der Seite.
- `.ffpfs`/`.ffpfsc`-Spiele mit Titel-ID und Namen (von ShadowMountPlus).

Alle Funktionen aus 2.0 sind enthalten. Getestet: 10 Testsammlungen unter Linux.

## [2.0.0](https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.0.0) – 2026-10-13

### Neu

**Weboberfläche mit Reitern** (Formatieren · Inhalt · Prüfen), Laufwerkskarten mit Belegung, Spielezahl, Format und Bezeichnung.

- **Inhalt:** Spiele als Ordner und Abbilder (`.exfat`, `.ffpkg`, `.ffpfs`, `.ffpfsc`) mit Titel-ID und Namen, PKG-Dateien, freier Platz
- **Doppelte Spiele** über alle Laufwerke erkennen (gleiche Titel-ID)
- **Schutz:** Liegen Spiele auf dem Laufwerk, muss das Löschen ausdrücklich bestätigt werden
- **Echtheitstest** vor dem Formatieren: erkennt gefälschte Sticks, auf Wunsch nur den echten Bereich formatieren
- **Tempo** messen (lesen; schreiben beim Prüfen)
- **Vollständig formatieren:** jede Stelle beschreiben und prüfen
- **Ordner-Vorlage:** `homebrew`, `etaHEN/games`, `pkg`
- **Umbenennen** ohne Formatieren (exFAT, FAT32)
- **exFAT prüfen und reparieren** (fsck.exfat aus exfatprogs 1.4.3)
- **GPT** automatisch ab 2 TB oder auf Wunsch
- App-Starter: Sandbox wird jetzt sauber abgebaut (vorher blockierte ShadowMountPlus danach gelegentlich Spielstarts)

### Dateien

| Datei | Zweck |
| --- | --- |
| `PPSA86920.zip` | App-Kachel: entpackten Ordner nach `/data/homebrew/` kopieren |
| `usb_format.elf` | Formatierer als Payload (ELF-Loader, Port 9021) |
| `PS5-USB-Formatierer.exe` | Windows-Programm |

Getestet: 10 Testsammlungen unter Linux (u. a. gegen fsck.exfat, fsck.fat, sgdisk). Die neuen Funktionen sind auf der PS5 noch nicht im Echtbetrieb geprüft.
