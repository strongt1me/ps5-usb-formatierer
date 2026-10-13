# PS5 USB-Formatierer

Formatiert USB-Sticks und -Festplatten direkt an der PS5 (Jailbreak, FW 12.00) – bedient über den PS5-Browser,
jeden Browser im Heimnetz oder ein Windows-Programm.

## Funktionen

- **Formatieren** als exFAT (64 KB, ideal für ShadowMountPlus) oder FAT32; im Bereich „nicht für die PS5“ auch
  NTFS, ext4 und freie Zuordnungseinheiten
- **Ohne Abziehen:** eingebundene Laufwerke meldet der Formatierer per Software ab und übernimmt sie, bevor die PS5
  sie wieder einbindet
- **Inhalt:** Spiele (Ordner und Abbilder `.exfat`/`.ffpkg`/`.ffpfs`, mit Titel-ID und Name), PKG-Dateien, Belegung
- **Doppelte Spiele** über alle Laufwerke erkennen (gleiche Titel-ID – ShadowMountPlus wechselt sonst ständig)
- **Schutz vor Datenverlust:** liegen Spiele auf dem Laufwerk, muss das Löschen ausdrücklich bestätigt werden
- **Echtheitstest:** erkennt Sticks mit gefälschter Größe und formatiert auf Wunsch nur den echten Bereich
- **Tempo** messen (lesen; schreiben beim Prüfen vor dem Formatieren)
- **Vollständig formatieren:** jede Stelle beschreiben und prüfen
- **Ordner-Vorlage:** `homebrew`, `etaHEN/games`, `pkg` gleich beim Formatieren anlegen
- **Umbenennen** ohne Formatieren (exFAT, FAT32)
- **exFAT prüfen und reparieren** (fsck.exfat aus exfatprogs, läuft im Formatierer)
- **GPT** für Laufwerke über 2 TB (automatisch) oder auf Wunsch

## Installation

Vom [Release](../../releases) laden:

| Datei | Zweck |
| --- | --- |
| `PPSA86920.zip` | App-Kachel: Ordner nach `/data/homebrew/` auf die PS5 kopieren (ShadowMountPlus meldet ihn an) |
| `usb_format.elf` | der Formatierer als Payload (ELF-Loader, Port 9021) |
| `PS5-USB-Formatierer.exe` | Windows-Programm: findet die PS5 im Netz und formatiert über sie – oder (Schalter „USB an diesem PC“) formatiert Sticks direkt am PC mit demselben PS5-Layout, inkl. FAT32 über 32 GB, Fake-Test, Tempo messen und Umbenennen |

Die Weboberfläche ist danach unter `http://<PS5-IP>:8090` erreichbar.

## Bauen

- Payload und App (WSL/Linux, [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)):
  `make -C usb_format all app PS5_PAYLOAD_SDK=…` (`app` braucht `ps5-native-tool`, siehe `usb_format/app/build_title.sh`)
- App-Ordner: `usb_format/dump/build.ps1` (.NET 10)
- Windows-Programm: `windows/build_exe.bat` (Python, PyInstaller; der PC-Helfer `usbfmt_win.exe` aus
  `usb_format/src/win` wird mit `pip install ziglang` gebaut)
- Tests (Linux/WSL): `usb_format/test/*.sh`

## Fremdcode und Lizenzen

- `usb_format/third_party/exfatprogs` – exfatprogs 1.4.3, **GPL-2.0-or-later** (unverändert; fsck.exfat ist in den
  Formatierer eingebunden, damit unterliegt auch `usb_format.elf` der GPL)
- `usb_format/tools/bin` – mkntfs (ntfs-3g) und mke2fs (e2fsprogs), GPL, siehe `THIRD_PARTY_NOTICES.txt`
- `usb_format/app/sce_module/libc.prx` – GPL-3.0-or-later, siehe `NOTICE.txt`
