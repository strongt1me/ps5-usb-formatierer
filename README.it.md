<div align="center">

<img src="docs/images/logo.png" width="140" alt="Formattatore USB">

# Formattatore USB

**Formatta chiavette e dischi USB – direttamente sulla PS5 o sul PC.**<br>
Con test di autenticità e velocità, backup immagine, gestione dei giochi, tutti i file system più diffusi e partizioni personalizzate.

[![Versione](https://img.shields.io/badge/Versione-2.5.8-185fa5?style=for-the-badge)](../../releases/latest)
[![Licenza](https://img.shields.io/badge/Licenza-GPL--3.0-6e7781?style=for-the-badge)](LICENSE)
[![Piattaforma](https://img.shields.io/badge/Piattaforma-PS5%20%7C%20Windows%20%7C%20Linux-0070d1?style=for-the-badge)](#-download)
[![Lingue](https://img.shields.io/badge/Lingue-DE%20%7C%20EN%20%7C%20IT%20%7C%20ES%20%7C%20FR-3b6d11?style=for-the-badge)](#-funzioni)

[Funzioni](#-funzioni) · [Download](#-download) · [Avvio rapido](#-avvio-rapido) · [Schermate](#-schermate) · [Compilazione](#-compilazione) · [Licenze](#-licenze)

[Deutsch](README.de.md) · [English](README.md) · **Italiano** · [Español](README.es.md) · [Français](README.fr.md)

<img src="docs/images/it/web.webp" width="900" alt="Interfaccia web del Formattatore USB">

</div>

## ✨ Funzioni

<table>
<tr>
<td width="33%" valign="top">

### 🎮 Fatto per la PS5
exFAT con 64 KB – ideale per ShadowMountPlus. Crea subito le cartelle `homebrew`, `etaHEN/games` e `pkg`. Si avvia da un proprio riquadro app.

</td>
<td width="33%" valign="top">

### 🔌 Senza scollegare
Il formattatore scollega via software le unità montate e le acquisisce prima che la PS5 le rimonti.

</td>
<td width="33%" valign="top">

### 🕹️ Giochi sotto controllo
Giochi (cartelle e immagini con ID titolo), file PKG e occupazione. Riconosce i giochi doppi, rimuove i giochi insieme al riquadro con «Elimina giochi …».

</td>
</tr>
<tr>
<td valign="top">

### 🛡️ Test di autenticità
Riconosce le chiavette con capacità contraffatta e, se vuoi, formatta solo l'area reale. Misura la velocità di lettura e scrittura.

</td>
<td valign="top">

### 🧩 Tutti i file system
exFAT, FAT32 (anche oltre 32 GB), NTFS, FAT16, ext4/ext3/ext2 e HFS+ – unità di allocazione a scelta.

</td>
<td valign="top">

### 🗂️ Modalità esperto
Più partizioni con MBR (fino a 4) o GPT (fino a 16): file system, dimensione, tipo, etichetta, avviabile.

</td>
</tr>
<tr>
<td valign="top">

### 🩺 Controlla e ripara
Controlla e ripara exFAT, rinomina le unità senza perdere dati, formattazione completa che scrive e verifica ogni settore.

</td>
<td valign="top">

### 🌍 Cinque lingue
Deutsch, English, Italiano, Español e Français – con una guida dettagliata dietro il pulsante «Aiuto».

</td>
<td valign="top">

### 📱 Usalo ovunque
Browser della PS5, smartphone o PC (codice QR) e due programmi per PC, per Windows e Linux. Tema chiaro o scuro, dimensione regolabile.

</td>
</tr>
<tr>
<td valign="top">

### 📤 Invia ELF
Il programma per PC invia il formattatore integrato alla PS5 con un clic (porta 9021) – e sostituisce una versione precedente già attiva.

</td>
<td valign="top">

### 📡 Trova la PS5
I programmi per PC trovano da soli la PS5 nella rete domestica – anche da WSL o da una macchina virtuale.

</td>
<td valign="top">

### 🔒 Niente scambi
Controllo del numero di serie: se nel frattempo è inserita un'altra chiavetta dello stesso modello, non viene formattato nulla. Dopo l'esito «falsificata» o «difettosa» il vecchio file system mezzo sovrascritto viene rimosso.

</td>
</tr>
<tr>
<td colspan="3" valign="top">

### 💾 Salvare e ripristinare immagini
Salva un'intera unità come <code>.img.zst</code> – solo le aree occupate, compresse con zstd, con <code>.sha256</code>. Sulla PS5 nella memoria interna, su un'altra unità USB o come download; sul PC in qualsiasi cartella. Riscrittura con verifica finale – anche caricando il file da telefono o PC. Con <code>zstd -d</code> il file diventa una normale immagine.

</td>
</tr>
</table>

## 📦 Download

Tutti i file sono nell'[ultima release](../../releases/latest).

| File | A cosa serve |
|---|---|
| `PPSA86920.zip` | **Riquadro app per la PS5:** estrai e copia la cartella `PPSA86920` in `/data/homebrew/` (ad es. via FTP). ShadowMountPlus registra l'app. |
| `usb_format.elf` | **Il formattatore (payload).** L'app lo avvia da sola, il programma per PC lo invia con «Invia ELF»; altrimenti invialo a mano all'ELF loader (porta 9021). |
| `PS5-USB-Formatierer.exe` | **Programma Windows con funzioni PS5:** «Sulla PS5» (in rete), «Sul PC – per PS5», «Sul PC – libero» – con l'ELF integrato. |
| `USB-Formatierer.exe` | **Programma Windows senza funzioni PS5:** tutti i file system, partizioni e modalità esperto. |
| `PS5-USB-Formatierer-linux-x86_64.tar.gz`<br>`USB-Formatierer-linux-x86_64.tar.gz` | **Gli stessi due programmi per Linux** (x86_64): estrai e avvia. |

**Requisiti:** una PS5 con jailbreak attivo ed ELF loader (testato con il firmware 12.00); ShadowMountPlus per il riquadro app e per eliminare i giochi. Windows 10 o 11 (64 bit); per formattare sul PC, Windows chiede una volta i diritti di amministratore. Linux (x86_64): per formattare, il sistema chiede la password di amministratore (pkexec/polkit); NTFS, FAT16 ed ext li scrivono gli strumenti di sistema di `ntfs-3g`, `dosfstools` ed `e2fsprogs`.

## 🚀 Avvio rapido

1. **Installa:** copia la cartella `PPSA86920` dello ZIP in `/data/homebrew/` sulla PS5.
2. **Avvia:** carica il jailbreak e apri il riquadro **USB-Formatierer**. L'interfaccia compare nel browser della PS5 – con il codice QR o su `http://<IP-PS5>:8090` anche su smartphone o PC. Senza riquadro: nel programma per PC «PS5-USB-Formatierer» fai clic su **Invia ELF**.
3. **Formatta:** scegli l'unità, imposta file system ed etichetta, premi **Formatta** – fatto.

> [!WARNING]
> Durante l'uso non scollegare né ricollegare unità USB alla PS5. La PS5 le collega ai giochi e alle app in esecuzione – dopo spesso questi ripartono solo dopo un riavvio della console. Scollega solo quando il formattatore te lo chiede esplicitamente.

Il pulsante **Aiuto** apre la guida dettagliata in cinque lingue – nell'interfaccia web e nei programmi per PC.

## 🖼️ Schermate

<table>
<tr>
<td width="50%" align="center"><img src="docs/images/it/ps5.webp" alt="Browser della PS5"><br><sub><b>Nel browser della PS5 sul televisore</b></sub></td>
<td width="50%" align="center"><img src="docs/images/it/dialog.webp" alt="Conferma"><br><sub><b>Conferma – se l'unità contiene giochi, va confermato esplicitamente</b></sub></td>
</tr>
<tr>
<td align="center"><img src="docs/images/it/light.webp" alt="Tema chiaro"><br><sub><b>Tema chiaro – chiavetta contraffatta riconosciuta, si usa solo l'area reale</b></sub></td>
<td align="center"><img src="docs/images/it/help.webp" alt="Guida"><br><sub><b>Guida in cinque lingue dietro il pulsante «Aiuto»</b></sub></td>
</tr>
<tr>
<td colspan="2" align="center"><img src="docs/images/it/backup.webp" alt="Salvare e ripristinare immagini" width="640"><br><sub><b>Salvare e ripristinare immagini – nella memoria interna, su un'altra unità USB o come download</b></sub></td>
</tr>
<tr>
<td align="center"><img src="docs/images/it/win_ps5.webp" alt="Windows: Sulla PS5" width="420"><br><sub><b>Windows: Sulla PS5 – trova da solo la PS5, «Invia ELF» le invia il formattatore</b></sub></td>
<td align="center"><img src="docs/images/it/win_std.webp" alt="Windows: Formattatore USB" width="420"><br><sub><b>Windows: Formattatore USB – tutti i file system</b></sub></td>
</tr>
<tr>
<td colspan="2" align="center"><img src="docs/images/it/win_expert.webp" alt="Modalità esperto" width="520"><br><sub><b>Modalità esperto – partizioni personalizzate con exFAT, NTFS, HFS+ ed ext4</b></sub></td>
</tr>
</table>

## 🛠️ Compilazione

| Parte | Come fare |
|---|---|
| Payload e avvio dell'app | WSL/Linux con [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk): `make -C usb_format all app PS5_PAYLOAD_SDK=…` (`app` richiede `ps5-native-tool`, vedi `usb_format/app/build_title.sh`) |
| Cartella dell'app | `usb_format/dump/build.ps1` (.NET 10) |
| Programmi Windows | `windows/build_exe.bat` (Python con PyInstaller, Pillow e ziglang) – compila anche l'helper `usbfmt_win.exe` e `mke2fs.exe` |
| Programmi Linux | `windows/build_linux.sh [usb_format.elf]` (Python 3 con tkinter, PyInstaller e Pillow, gcc) – compila anche l'helper `usbfmt_linux`. La CI (`.github/workflows/linux.yml`) compila e verifica tutto su Ubuntu, anche su dispositivi loop. |
| Test | Linux/WSL: `usb_format/test/*.sh` |
| Traduzioni | `usb_format/i18n/*.json` (`tools/i18n_gen.py`, `tools/web_i18n.py`) e `windows/i18n_app.json` (`windows/i18n_check.py`) |

## 📄 Licenze

Il Formattatore USB è distribuito con licenza [GNU General Public License 3.0](LICENSE). Il codice di terze parti incluso mantiene la propria licenza:

- `usb_format/third_party/exfatprogs` – exfatprogs 1.4.3, **GPL-2.0-or-later** (fsck.exfat è integrato nel formattatore, quindi anche `usb_format.elf` è soggetto alla GPL)
- `usb_format/tools/bin` – mkntfs (ntfs-3g) e mke2fs (e2fsprogs), GPL, vedi `THIRD_PARTY_NOTICES.txt`
- `windows/mke2fs` – mke2fs da e2fsprogs 1.47.4 per ext2/3/4 sul PC, GPL, vedi `NOTICE.txt`
- `usb_format/app/sce_module/libc.prx` – GPL-3.0-or-later, vedi `NOTICE.txt`
- `usb_format/third_party/zstd` – zstd 1.5.7 (Meta Platforms) per il backup immagine, **licenza BSD** (o GPL-2.0), vedi `LICENSE` lì

Uso a proprio rischio – fai prima un backup dei dati importanti.

<div align="center"><sub>Sviluppato da <a href="https://github.com/strongt1me">strongt1me</a> · per la community PS5</sub></div>
