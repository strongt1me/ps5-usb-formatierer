# Changelog

Alle nennenswerten Änderungen am PS5 USB-Formatierer. Das Format folgt [Keep a Changelog](https://keepachangelog.com/de/1.1.0/),
die Versionsnummern folgen [Semantic Versioning](https://semver.org/lang/de/). Die Dateien zu jeder Version liegen unter
[Releases](https://github.com/strongt1me/ps5-usb-formatierer/releases).

## [2.6.64] – 2026-10-31

### Entfernt

- Weboberfläche: Reiterwechsel mit L1/R1 – er funktionierte auf der PS5 nicht. L2/R2 scrollen weiterhin.

## [2.6.63] – 2026-10-31

### Geändert

- Weboberfläche: „Formatierer stoppen“ heißt jetzt „Formatierer ausschalten“ (Knopf, Dialog und Meldung, alle Sprachen).

### Entfernt

- Hilfe: Hinweis auf den Reiterwechsel mit L1/R1.

## [2.6.62] – 2026-10-31

### Hinzugefügt

- Kennzeichen „eingehängt“ in der Spieleliste, Filter „Nur eingehängte“ (Weboberfläche) und Sortierung „Eingehängt“ (Programm).
- „Aushängen und fortfahren“, wenn „Kachel neu anmelden“ an einem eingehängten Spiel scheitert.
- Weboberfläche an der PS5: L1/R1 wechseln den Reiter.
- PS5-Benachrichtigung „beendet“ nennt die Quelle des Befehls; jeder Stopp wird in `/data/usb_format/quit.log` festgehalten.
- Diagnose-Bericht mit den letzten Stopp-Befehlen und auffälligen Zeilen aus dem Kernel-Log.
- Linux-AppImage: „Jetzt aktualisieren“ lädt nur die geänderten Teile (zsync, auch mit AppImageUpdate nutzbar).

### Geändert

- Weboberfläche: „Beenden“ heißt „Formatierer stoppen“; der Dialog erklärt, dass zum Schließen der App die PS-Taste reicht.
- ShadowMountPlus-Meldung „ein Spiel ist noch eingehängt“ wird verständlich statt als englischer Fehler angezeigt.

## [2.6.61] – 2026-10-30

### Hinzugefügt

- Weboberfläche an der PS5: L2/R2 scrollen die Seite, je fester gedrückt, desto schneller.

### Geändert

- Spiele und PKG: das erste Laufwerk mit Spielen ist vorgewählt; leere Listen sagen, was zu tun ist; Kopieren, Verschieben und Löschen erscheinen erst beim Anhaken.
- Formatieren wird erst rot, wenn ein Laufwerk gewählt ist; eine für Spiele vorgewählte SSD wird nie übernommen.
- Spielstände: kürzere Angaben und Datum in der Schreibweise der Sprache.
- Werkzeuge: alle Knöpfe in einer Reihe, darunter der Grund, wenn sie gesperrt sind.
- Das Fenster bleibt in allen Betriebsarten und Reitern gleich groß.

### Behoben

- Die Erklärung zu fPKG/PKG verschwand, sobald Pakete geladen waren.

## [2.6.60] – 2026-10-30

### Hinzugefügt

- Spiel-Einzelheiten zeigen, ob ShadowMountPlus das Spiel eingehängt hat, dazu Patch und DLCs – mit Aus-/Einhängen und Entfernen von Patch bzw. DLCs.
- Doppelte Spiele: „Ältere Version deaktivieren …“ bzw. „Überzählige Kopie deaktivieren …“ (Endung `.off` statt Löschen).
- Nach dem Auswerfen steht „kann abgezogen werden“; ein wieder eingesteckter Stick erscheint von selbst.

### Geändert

- Weboberfläche: mehr Abstand der Auswahlkästchen zum Rand, ruhigere Liste beim Aktivieren/Deaktivieren.

## [2.6.59] – 2026-10-29

### Hinzugefügt

- „Aktivieren / Deaktivieren …“: Abbilder (`.ffpfs`, `.ffpfsc`, `.ffpkg`, `.exfat`) bekommen bzw. verlieren die Endung `.off`, ShadowMountPlus bindet sie dann nicht mehr ein. Deaktivierte Spiele werden als solche angezeigt.

### Geändert

- Deaktivierte Kopien zählen nicht als doppelte Spiele; Prüfen, Prüfsummen, Kopieren, Löschen und Aufräumen berücksichtigen sie.

## [2.6.58] – 2026-10-29

### Behoben

- PS5-USB-Formatierer: Absturz beim Laden der Spielbilder ohne Verbindung zur PS5 (deshalb fehlten in 2.6.57 die Linux-Fassungen).

## [2.6.57] – 2026-10-29

### Geändert

- Spielbilder: auch große Bilder in UFS2-Abbildern werden angezeigt; Spiele ohne Bild werden nicht bei jedem Neuaufbau erneut angefragt.
- Auswerfen: Hinweis „braucht ShadowMountPlus 1.7“ nur noch, wenn das feststeht; ein später gestartetes ShadowMountPlus wird erkannt.

### Behoben

- PS5-USB-Formatierer: Einstellungen gingen bei kaputter oder gesperrter Einstellungsdatei verloren; nach dem Wechsel der PS5 galten deren Angaben nicht.

### Sicherheit

- „Inhalt vergleichen“ ist robust gegen manipulierte Abbilder: erkennt Ordner- und FAT-Schleifen, lässt sich abbrechen und begrenzt Speicher und Umfang.

## [2.6.56] – 2026-10-29

### Hinzugefügt

- „Auswerfen“ meldet ein Laufwerk über ShadowMountPlus 1.7 sicher ab.
- Spielbilder in der Spieleliste (Vorschau von ShadowMountPlus oder `icon0.png`, auch aus Abbildern).
- ShadowMountPlus-Version in der Fußzeile; Funktionen, die 1.7 brauchen, sind ohne es gesperrt.
- Tempo-Verlauf je Datenträger als kleine Kurve, deutliche Einbrüche rot markiert.
- Diagnose- und Fehlerbericht mit Firmware, ShadowMountPlus-Version und Kernel-Erweiterungen.

### Geändert

- Das ShadowMountPlus-Protokoll (`debug.log`) wird nur noch gelesen, wenn dessen API nicht antwortet.

## [2.6.55] – 2026-10-29

### Hinzugefügt

- Hinweis auf möglichen Verschleiß, wenn ein Stick deutlich langsamer liest als früher.
- „Inhalt vergleichen“ vergleicht auch ein Abbild (exFAT, UFS2) mit einem Spielordner.

### Geändert

- Gebaut mit dem PS5-Payload-SDK 0.45 (Firmware 14.00/14.10).
- Linux-Fassungen laufen weiterhin ab glibc 2.35.

### Behoben

- Spielordner mit `eboot2.bin` (ShadowMountPlus 1.7) galten als „unvollständig“.

## [2.6.54] – 2026-10-29

### Geändert

- Die Projektlizenz liegt den Windows-Programmen und AppImages bei.

### Behoben

- Einstellungen von „Am PC – frei formatieren“ gingen seit 2.6.52 verloren.
- Hilfe: Tastenkürzel und Namen der Betriebsarten richtiggestellt.
- „Inhalt vergleichen“ lehnt zweimal dasselbe Spiel ab; die Windows-Warteschlange findet ihr Ziel nach einem Betriebsartwechsel; CSV-Verlauf in Ortszeit.

## [2.6.53] – 2026-10-29

### Behoben

- Weboberfläche: Suchfeld, Häkchen, Größenangaben und Dialogknöpfe sehen im PS5-Browser richtig aus.
- PC-Programme: Bildlaufleisten nur bei Bedarf; „Suche …“ statt „kein USB-Datenträger gefunden“ während der Suche.

## [2.6.52] – 2026-10-29

### Behoben

- Zwei Fenster desselben Programms überschreiben sich nicht mehr gegenseitig die Einstellungen.
- Baugleiche Sticks ohne Seriennummer werden nach dem Neuladen nicht still ausgewählt.
- Linux: btrfs-Partitionen werden erkannt und ausgehängt.
- Windows: Ein Image, das über `subst` oder eine lokale Freigabe auf dem Ziel-Stick liegt, wird erkannt.

## [2.6.51] – 2026-10-29

### Behoben

- Aufräumen meldete während des Einlesens bzw. in sehr vollen Ordnern Dateien fälschlich als verwaist.
- Kopieren, Prüfen und Aufräumen scheiterten mit mehr als etwa 56 Einträgen.
- Ein abgebrochener Spielstand-Upload ließ eine Datei geöffnet.
- Eine präparierte Image-Datei mit unsinniger Größe brachte den Formatierer zum Absturz.
- Warteschlange: Ziel wird über die Seriennummer gefunden; die Web-Warteschlange blieb auf „läuft“ hängen.
- Windows-App: Kopierdialog fror bei langsamer PS5 ein; Fehler beim Schließen während einer Abfrage.
- Linux: Der Helfer prüft direkt vor dem Schreiben erneut, ob derselbe Datenträger steckt.
- PS5-Suche unter Linux durchsucht alle Netzwerkkarten.

## [2.6.50] – 2026-10-29

### Hinzugefügt

- „Inhalt vergleichen“ prüft zwei Kopien desselben Spiels Byte für Byte.
- Ordner-Spiele ohne `eboot.bin` oder Titelbild werden als „unvollständig“ gekennzeichnet.
- Ziel-Auswahl zeigt das zuletzt gemessene Lesetempo.
- Verlauf als CSV speichern.

### Geändert

- Windows-App: Die Kopier-Warteschlange übersteht einen Neustart.

## [2.6.49] – 2026-10-28

### Hinzugefügt

- Weboberfläche: Wahrscheinliche Kopien (gleiche Version und Größe) werden erkannt.

### Geändert

- Weboberfläche: Die Kopier-Warteschlange übersteht ein Neuladen der Seite.
- Ziel-Auswahl zeigt „zu wenig Platz“ und die FAT32-Grenze direkt beim Laufwerk.

## [2.6.48] – 2026-10-28

### Hinzugefügt

- „Aufräumen“ findet verwaiste Prüfsummen, Dump-Infos, Backport-Ordner und leere Spielordner.
- Prüfsummenlisten für Ordner-Spiele (`<Name>.manifest.sha256`).
- Warteschlange für Kopieraufträge.
- Weboberfläche: Diagnose-Bericht in der Fußzeile.

### Geändert

- Tempo und Restzeit beginnen bei jedem Schritt neu.

## [2.6.47] – 2026-10-28

### Hinzugefügt

- Teilkopien abgebrochener Kopiervorgänge werden angezeigt und lassen sich verwerfen.
- „Prüfsumme erstellen“ legt neben Abbildern eine `.sha256` an.
- Spiel-Einzelheiten per Klick bzw. Doppelklick.
- „Ältere Version auswählen“ bei doppelten Spielen.

### Behoben

- Weboberfläche: Knopfleisten ragten bei schmaler Anzeige über den Rand.

## [2.6.46] – 2026-10-27

### Hinzugefügt

- Abgebrochenes Kopieren lässt sich fortsetzen.
- „Spiele prüfen“ prüft Abbilder gegen ihre `.sha256`.
- Durchsuchbare Spieleliste in der Weboberfläche; Dialoge zeigen den freien Platz danach.
- Windows-App: `fehler.log` und „Bericht kopieren“ (Strg+Umschalt+B).

## [2.6.45] – 2026-10-27

### Behoben

- Die Windows-Programme fehlten am Release 2.6.44 (CI-Test); Inhalt sonst wie 2.6.44.

## [2.6.44] – 2026-10-27

### Behoben

- Windows: weitere Absicherung gegen Abstürze bei aufgeklappter Auswahlliste in Dialogen.

## [2.6.43] – 2026-10-27

### Geändert

- Weboberfläche: Reiter „Spiele“ in den Fremdsprachen großgeschrieben.

## [2.6.42] – 2026-10-27

### Behoben

- Windows: Absturz (`KeyError: 'popdown'`) bei aufgeklappter Laufwerksliste im Kopierdialog.

## [2.6.41] – 2026-10-26

### Hinzugefügt

- Vorabprüfung vor dem Kopieren: zu kleines Ziel oder FAT32-Grenze werden sofort gemeldet.
- PKG-Dateien im Vergleich mit der PS5: neuer, schon installiert oder älter; nötige Firmware.
- Kopiertempo unter dem Fortschrittsbalken; Löschen von PKG-Dateien lässt sich abbrechen.

## [2.6.40] – 2026-10-26

### Behoben

- Spielstände wiederherstellen: eine Anfrage mit Pfad ohne Schrägstrich konnte den Formatierer zum Absturz bringen.
- Das Kopf-Kästchen wählt nur die neueste Version je Spielstand; der alte PS4-Schlüssel wird bei Fehlern wiederhergestellt.
- Ausgeblendete Spielstände anderer Benutzer werden nicht mitgesichert.
- Formatieren schreibt den Bootsektor zuletzt.
- Speicherleck, Thread-Sicherheit und weitere kleinere Fehler.

## [2.6.39] – 2026-10-26

### Geändert

- Windows-Programm: breitere Spielstände-Liste.

## [2.6.38] – 2026-10-26

### Hinzugefügt

- PKG-Reiter: Spielname, Version, Art und nötige Firmware aus den Paketen.
- „Kopieren nach …“ für PKG-Dateien.

### Behoben

- Teilweise gescheitertes Löschen wird als solches gemeldet.
- Zugehörige Dateien bleiben stehen, solange eine andere Kopie sie nutzt.
- Windows-Programm: Listen nach Theme-/Sprachwechsel, Größen der Ordner-Spiele, Einfrieren bei Netzabfragen.

## [2.6.37] – 2026-10-25

### Geändert

- Reiter je nach Auswahl: interner Speicher zeigt Spiele, PKG und Spielstände; Laufwerke zusätzlich Werkzeuge und Formatieren.

## [2.6.36] – 2026-10-25

### Geändert

- Bei gewähltem internem Speicher gibt es keinen Reiter „Formatieren“.
- Zugehörige Dateien werden einmal je Suche ermittelt (schneller).

### Behoben

- Absturz beim Fortsetzen einer Image-Sicherung (Zugriff auf freigegebenen Speicher).

## [2.6.35] – 2026-10-25

### Hinzugefügt

- „Spiele kopieren …“ – wie Verschieben, die Originale bleiben.

## [2.6.34] – 2026-10-25

### Geändert

- Reiter „Spielstände“ heißt „Spielstände auf der PS5“.

## [2.6.33] – 2026-10-25

### Behoben

- PS4-Pakete wurden in 2.6.32 fälschlich als fPKG angezeigt.

## [2.6.32] – 2026-10-25

### Hinzugefügt

- Unterscheidung von fPKG und PKG bei PS4-Paketen (in 2.6.33 zurückgenommen).

## [2.6.31] – 2026-10-25

### Behoben

- Fortschrittsbalken der PC-Suche blieb bei schlafenden Platten stehen.

## [2.6.30] – 2026-10-25

### Geändert

- Backport-Ordner werden wie bei ShadowMountPlus gefunden und beim Kopieren und Verschieben mitgenommen.

## [2.6.29] – 2026-10-25

### Hinzugefügt

- Spiele im internen Speicher und auf `/mnt/extN` kopieren, verschieben und löschen – samt zugehöriger Dateien.

## [2.6.28] – 2026-10-25

### Hinzugefügt

- PKG-Dateien verschieben und löschen.

### Behoben

- Fortschrittsbalken blieb bei 0 %, während eine schlafende Platte anlief.

## [2.6.27] – 2026-10-24

### Hinzugefügt

- Windows-Programm mit Reitern wie die Weboberfläche (Spiele, PKG, Spielstände, Werkzeuge, Formatieren).
- Werkzeuge an der PS5 auch im PC-Programm: Tempo messen, Prüfen & reparieren, Umbenennen, Image-Sicherung.

## [2.6.26] – 2026-10-24

### Hinzugefügt

- PKG-Reiter liest den Paketkopf: PKG/fPKG bei PS5-Paketen, Titel-ID und Update bei PS4-Paketen.

## [2.6.25] – 2026-10-24

### Hinzugefügt

- Neue Oberfläche in Reitern mit Formatieren im roten Warnrahmen.
- Typ-Kennzeichen der Spiele (Dump-Ordner, exFAT, ffpfs, ffpfsc, ffpkg).
- PKG-Reiter mit allen `.pkg`-Dateien des Laufwerks.
- Image-Sicherung fortsetzen und unvollständige Sicherungen verwerfen.

### Geändert

- Überarbeitete Spieleliste; Größen der Ordner-Spiele werden im Hintergrund ermittelt.

## [2.6.24] – 2026-10-24

### Geändert

- Verschieben-Dialog: Hinweis, wenn ShadowMountPlus das Löschen eines Originals ablehnt; neutrale Farbe.

## [2.6.23] – 2026-10-24

### Hinzugefügt

- „Spiele verschieben …“ zwischen Laufwerken, mit SHA-256-Prüfung jeder Datei.

## [2.6.22] – 2026-10-24

### Hinzugefügt

- Weboberfläche: Spieleliste nach Name, Größe oder Titel-ID sortieren; Größe von Ordner-Spielen.

## [2.6.21] – 2026-10-24

### Geändert

- Prüfbericht nach dem Formatieren ohne Warnfarbe.

## [2.6.20] – 2026-10-24

### Hinzugefügt

- Prüfbericht nach dem Formatieren mit einzelnen Ergebnissen.
- Nachkontrolle des frisch geschriebenen exFAT-Dateisystems.

## [2.6.19] – 2026-10-24

### Hinzugefügt

- Spielstände: Vorversionen der Sicherung (die neueste und zwei davor bleiben).
- Liste zeigt den Stand der Sicherung; Wiederherstellen vergleicht mit dem aktuellen Stand.

## [2.6.18] – 2026-10-24

### Behoben

- Die Restzeit-Zeile blieb nach dem Auftrag stehen.

## [2.6.17] – 2026-10-23

### Geändert

- Durchgehender Fortschrittsbalken bei Echtheitsprüfung, Tempomessung und vollständigem Formatieren.
- Spiele löschen nennt das Spiel.

## [2.6.16] – 2026-10-23

### Geändert

- Fortschritt der Image-Sicherung nach belegten Bytes; Menge und Restzeit unter dem Balken.

## [2.6.15] – 2026-10-23

### Geändert

- Spielstände: Container eines Spiels werden erst beim Aufklappen gezeigt.

## [2.6.14] – 2026-10-23

### Behoben

- Setzen der Änderungszeit von Spielständen probiert mehrere Systemaufrufe.

## [2.6.13] – 2026-10-23

### Behoben

- Zeitstempel-Reparatur für Spielstände funktioniert auf der PS5.

## [2.6.12] – 2026-10-23

### Hinzugefügt

- `POST /api/saves/times` und `tools/ps5_fix_save_times.py` zum Angleichen bereits wiederhergestellter Spielstände.

### Behoben

- Wiederherstellen von Spielständen setzte die Änderungszeit neu, sodass Spiele den Spielstand nicht mehr laden konnten.

## [2.6.11] – 2026-10-23

### Hinzugefügt

- Wiederherstellen zeigt, wo eine Sicherung liegt.

## [2.6.10] – 2026-10-23

### Geändert

- Spielstände nach Spiel gruppiert.

## [2.6.9] – 2026-10-23

### Hinzugefügt

- PS4-Spielstände sichern und wiederherstellen.

### Geändert

- Die Systemkopie `sce_bu` wird nicht mehr aufgelistet.

## [2.6.8] – 2026-10-22

### Hinzugefügt

- Sichern und Wiederherstellen von Spielständen lassen sich abbrechen.
- Kennzeichen PS5/PS4 je Spielstand.

## [2.6.7] – 2026-10-22

### Geändert

- Echter Fortschritt beim Sichern und Wiederherstellen von Spielständen.

## [2.6.6] – 2026-10-22

### Hinzugefügt

- Spielnamen in der Spielstände-Liste, Mehrfachauswahl und Sichern auf die PS5 oder ein USB-Laufwerk.
- Wiederherstellen aus gefundenen Sicherungen oder einer hochgeladenen ZIP.

### Behoben

- Sichern im PS5-Browser tat nichts; zusätzliche Fehlerseite beim Download.

## [2.6.5] – 2026-10-22

### Hinzugefügt

- PS5-Spielstände sichern und wiederherstellen (Seite „Spielstände“).

### Geändert

- Windows-Programm baut sich in unter einer Sekunde auf.

## [2.6.4] – 2026-10-21

### Hinzugefügt

- „Zuletzt: …“ am Laufwerk zeigt den letzten Auftrag.

### Behoben

- Bei unumkehrbaren Fragen ist „Abbrechen“ vorgewählt.
- Tastenkürzel wirken nicht mehr während eines Auftrags oder bei offenem Dialog.
- „Image prüfen“ hielt fremde `.sha256` für beschädigt.

## [2.6.3] – 2026-10-21

### Hinzugefügt

- Auftragsverlauf auf der PS5 und in den PC-Programmen.
- `SHA256SUMS` an jedem Release.

## [2.6.2] – 2026-10-21

### Hinzugefügt

- „Was ist neu“ mit Download der passenden Datei.
- Update-Hinweis in der Weboberfläche.
- Linux als AppImage und für ARM64.

## [2.6.1] – 2026-10-20

### Hinzugefügt

- Sicherungen auf der PS5 verwalten (prüfen, löschen).
- Suche und Sortierung in der Spieleliste; Tastenkürzel; Kurzinfos an Knöpfen.

### Geändert

- Design „Automatisch“ folgt dem System auch im laufenden Betrieb.

## [2.6.0] – 2026-10-20

### Hinzugefügt

- Neue Oberfläche der PC-Programme in Hell und Dunkel, mit Seitenleiste und Laufwerkskarten.
- „Image prüfen“ ohne Zurückschreiben; Sicherungen auf der PS5 löschen.
- Hinweis auf neue Versionen.

### Behoben

- Fehler beim Wechsel von „Am PC – für PS5“ nach „Am PC – frei“.

## [2.5.11] – 2026-10-19

### Hinzugefügt

- Image-Sicherung von schreibgeschützten Sticks und SD-Karten.

## [2.5.10] – 2026-10-19

### Geändert

- PS5-USB-Formatierer: Spieleliste mit Kästchen; größeres Fenster.

## [2.5.9] – 2026-10-19

### Behoben

- Schreibfehler beim Zurückschreiben wurden als Erfolg gemeldet.
- Einfrieren beim Öffnen von „Sichern …“/„Wiederherstellen …“.
- „Kachel neu anmelden“ konnte hängen bleiben.

### Sicherheit

- Windows prüft direkt vor dem Schreiben erneut, dass der gewählte Datenträger steckt.
- Images mit anderer Sektorgröße und Images auf dem Datenträger selbst werden abgelehnt.

## [2.5.8] – 2026-10-19

### Hinzugefügt

- „Kachel neu anmelden“, damit die PS5 nach einem Update die richtige Version zeigt.

## [2.5.7] – 2026-10-18

### Hinzugefügt

- Versionsabgleich mit der PS5 und Angebot, die neue Fassung zu senden.

### Geändert

- Image-Sicherung etwa sechsmal schneller (SHA-NI, zstd auf bis zu vier Kernen).
- Weboberfläche: Design und Beenden oben.

## [2.5.6] – 2026-10-18

### Hinzugefügt

- Fortschrittsbalken beim Einlesen der Datenträger, bei der PS5-Suche und nach „ELF senden“.

### Geändert

- Modi heißen „An der PS5“, „Am PC – für PS5“ und „Am PC – frei“.

## [2.5.5] – 2026-10-18

### Behoben

- Weboberfläche: Kopfzeile bricht bei wenig Platz sauber um.

## [2.5.4] – 2026-10-18

### Geändert

- Weboberfläche: Sprachauswahl oben im Kopf.

## [2.5.3] – 2026-10-18

### Hinzugefügt

- Versionsanzeige in allen Programmen und an der App-Kachel.

### Geändert

- mkntfs 2026.9.28 und mke2fs 1.47.4.

## [2.5.2] – 2026-10-17

### Hinzugefügt

- Image-Sicherung im PC-Programm für Sticks an der PS5, auch auf den PC und vom PC.
- Dialog „Auftrag nicht gestartet“ mit Grund.

## [2.5.1] – 2026-10-17

### Geändert

- Image-Sicherung überspringt leere Bereiche (z. B. 8 statt 53 Sekunden für einen fast leeren Stick).

### Sicherheit

- Bei laufendem Spiel werden eingebundene Laufwerke nicht mehr abgemeldet.

## [2.5.0] – 2026-10-17

### Hinzugefügt

- Image-Sicherung: ganze Laufwerke als `.img.zst` sichern und wiederherstellen, an der PS5 und am PC.

## [2.4.9] – 2026-10-17

### Geändert

- Nur Dokumentation: README um „ELF senden“, PS5-Suche und Verwechslungsschutz ergänzt.

## [2.4.8] – 2026-10-17

### Behoben

- Partitions-Editor: nur noch eine startfähige Partition; unmögliche Dateisysteme werden vor dem Löschen abgelehnt.
- Umbenennen nur bei genau einem Laufwerk auf dem Datenträger.
- Echtheitsprüfung: tote Sticks als „defekt“, gefälschte Sticks mit Fehlern hinter der Grenze erkannt.

## [2.4.7] – 2026-10-17

### Behoben

- „ELF senden“ meldete Erfolg, obwohl die neue Fassung nicht startete.
- PS5-Suche unter Linux wieder schnell.
- Folgeaktionen nach einer Prüfung gelten immer dem geprüften Stick.

## [2.4.6] – 2026-10-17

### Hinzugefügt

- „ELF senden“ schickt den eingebauten Formatierer an die PS5 und ersetzt eine ältere Fassung.

## [2.4.5] – 2026-10-17

### Geändert

- PS5-Suche findet die PS5 auch hinter NAT (WSL, virtuelle Maschinen).

## [2.4.4] – 2026-10-17

### Hinzugefügt

- „Spiele löschen …“ als Knopf in der PS5-App.
- Seriennummer-Abgleich gegen vertauschte baugleiche Sticks.
- Entwickler-Fußzeile.

### Behoben

- Linux: Programme starteten nicht, wenn `xrandr` installiert ist.
- FAT32-Bezeichnung mit Umlauten verhinderte die Verbindung zur PS5.

## [2.4.3] – 2026-10-16

### Behoben

- Umstecken an der PS5 zuverlässiger; NTFS/ext4 auf langsamen Sticks melden keinen vorschnellen Fehler.
- PC-Programme erreichen die PS5 auch mit eingetragenem Proxy.
- Dateisystem-Auswahl entspricht genau dem, was der Helfer kann.

## [2.4.2] – 2026-10-16

### Behoben

- Eingelöteter eMMC-Speicher wird nicht mehr als SD-Karte angeboten.
- Abbrechen vor dem ersten Schreiben ließ trotzdem formatieren.
- MBR-Partitionen über 2 TB und HFS+ auf großen Partitionen.

## [2.4.1] – 2026-10-16

### Entfernt

- README: Hinweis auf eine geplante Mac-Fassung.

## [2.4.0] – 2026-10-16

### Hinzugefügt

- Linux-Fassung beider PC-Programme.

### Behoben

- Windows: ext2/3/4 nach neu angelegter FAT16-/NTFS-Partition scheiterte manchmal.

## [2.3.1] – 2026-10-15

### Geändert

- Kürzerer Warnhinweis oben; Lizenz GPL-3.0; neue README in fünf Sprachen.

## [2.3.0] – 2026-10-15

### Hinzugefügt

- Fünf Sprachen (Deutsch, Englisch, Italienisch, Spanisch, Französisch).
- Anleitung mit Knopf „Hilfe“.
- „Am PC Standard“ und eigenes Programm `USB-Formatierer.exe` mit allen Dateisystemen.
- Expertenmodus für mehrere Partitionen.

## [2.2.1] – 2026-10-15

### Behoben

- Windows-Programm: der PC-Modus aus 2.2.0 funktionierte nicht.

## [2.2.0] – 2026-10-15

### Hinzugefügt

- „USB an diesem PC“ im Windows-Programm.
- Spiele löschen über ShadowMountPlus.
- Gefälschte Sticks werden gemerkt und nur im echten Bereich formatiert.

### Sicherheit

- Server parallel und robust gegen langsame Verbindungen; Content-Security-Policy; Prüfsumme des Windows-Helfers.

## [2.1.7] – 2026-10-13

### Geändert

- Windows-Programm: Fortschrittsbalken wächst sichtbar mit; anderswo gestartete Aufträge werden angezeigt.

## [2.1.6] – 2026-10-13

### Geändert

- Fortschritt wird in jedem offenen Browser angezeigt.

## [2.1.5] – 2026-10-13

### Geändert

- Auswahlfelder mit eigenem, dezentem Pfeil.

## [2.1.4] – 2026-10-13

### Geändert

- Hauptkarte leicht durchsichtig und weichgezeichnet.

## [2.1.3] – 2026-10-13

### Geändert

- Hauptkarte ohne Bild und leicht durchsichtig.

## [2.1.2] – 2026-10-13

### Geändert

- Hauptkarte ohne Hintergrundbild.

## [2.1.1] – 2026-10-13

### Geändert

- Echtheitstest schneller (höchstens 128 Stichproben), mit Restzeit.

## [2.1.0] – 2026-10-13

### Hinzugefügt

- Weboberfläche in zwei Spalten; Größenregler.
- `.ffpfs`/`.ffpfsc`-Spiele mit Titel-ID und Namen.

## [2.0.0] – 2026-10-13

### Hinzugefügt

- Weboberfläche mit Reitern und Laufwerkskarten.
- Spiele und PKG-Dateien anzeigen, doppelte Spiele erkennen.
- Echtheitstest, Tempo messen, vollständig formatieren, Ordner-Vorlage, Umbenennen.
- exFAT prüfen und reparieren; GPT ab 2 TB.

### Behoben

- App-Starter baut die Sandbox sauber ab.

[2.6.64]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.64
[2.6.63]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.63
[2.6.62]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.62
[2.6.61]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.61
[2.6.60]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.60
[2.6.59]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.59
[2.6.58]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.58
[2.6.57]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.57
[2.6.56]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.56
[2.6.55]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.55
[2.6.54]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.54
[2.6.53]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.53
[2.6.52]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.52
[2.6.51]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.51
[2.6.50]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.50
[2.6.49]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.49
[2.6.48]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.48
[2.6.47]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.47
[2.6.46]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.46
[2.6.45]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.45
[2.6.44]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.44
[2.6.43]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.43
[2.6.42]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.42
[2.6.41]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.41
[2.6.40]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.40
[2.6.39]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.39
[2.6.38]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.38
[2.6.37]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.37
[2.6.36]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.36
[2.6.35]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.35
[2.6.34]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.34
[2.6.33]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.33
[2.6.32]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.32
[2.6.31]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.31
[2.6.30]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.30
[2.6.29]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.29
[2.6.28]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.28
[2.6.27]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.27
[2.6.26]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.26
[2.6.25]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.25
[2.6.24]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.24
[2.6.23]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.23
[2.6.22]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.22
[2.6.21]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.21
[2.6.20]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.20
[2.6.19]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.19
[2.6.18]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.18
[2.6.17]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.17
[2.6.16]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.16
[2.6.15]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.15
[2.6.14]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.14
[2.6.13]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.13
[2.6.12]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.12
[2.6.11]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.11
[2.6.10]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.10
[2.6.9]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.9
[2.6.8]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.8
[2.6.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.7
[2.6.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.6
[2.6.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.5
[2.6.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.4
[2.6.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.3
[2.6.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.2
[2.6.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.1
[2.6.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.6.0
[2.5.11]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.11
[2.5.10]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.10
[2.5.9]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.9
[2.5.8]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.8
[2.5.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.7
[2.5.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.6
[2.5.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.5
[2.5.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.4
[2.5.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.3
[2.5.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.2
[2.5.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.1
[2.5.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.5.0
[2.4.9]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.9
[2.4.8]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.8
[2.4.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.7
[2.4.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.6
[2.4.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.5
[2.4.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.4
[2.4.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.3
[2.4.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.2
[2.4.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.1
[2.4.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.4.0
[2.3.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.3.1
[2.3.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.3.0
[2.2.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.2.1
[2.2.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.2.0
[2.1.7]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.7
[2.1.6]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.6
[2.1.5]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.5
[2.1.4]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.4
[2.1.3]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.3
[2.1.2]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.2
[2.1.1]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.1
[2.1.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.1.0
[2.0.0]: https://github.com/strongt1me/ps5-usb-formatierer/releases/tag/v2.0.0
