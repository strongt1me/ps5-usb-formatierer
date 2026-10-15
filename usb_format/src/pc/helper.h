/* Gemeinsamer Teil der PC-Helfer (Windows: usbfmt_win.exe, Linux: usbfmt_linux): Aufträge zerlegen, prüfen,
 * formatieren, Ergebnis melden. Was je Betriebssystem anders ist (Datenträger finden und sperren, NTFS/FAT16 und
 * ext schreiben, umbenennen über das System), steht in den plat_*-Funktionen der jeweiligen Datei.
 *
 * Ausgabe (UTF-8, zeilenweise, nach --out oder stdout): "P<TAB>prozent<TAB>schritt" und zum Schluss
 * "R<TAB>0|1<TAB>meldung<TAB>json". */
#ifndef USBFMT_PC_HELPER_H
#define USBFMT_PC_HELPER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../mkfs.h"

#define MIB (1024ull * 1024ull)

/* Dateisysteme, die nur die PC-Helfer kennen (zusätzlich zu FS_* aus mkfs.h) */
enum { WFS_FAT16 = 100, WFS_EXT2 = 101, WFS_EXT3 = 102, WFS_NONE = 103 };

typedef struct {
    char          id[32];   /* wie "list" ihn nennt: Windows die Nummer ("4"), Linux der Name ("sdb") */
    int           num;      /* nur Windows: Datenträgernummer */
    uint64_t      size;
    uint32_t      ss;
    int           removable;
    int           bus;      /* nur Windows: STORAGE_BUS_TYPE */
    unsigned long media;    /* nur Windows: MEDIA_TYPE für FormatEx */
    char          model[128];
} diskinfo_t;

typedef struct plat_session plat_session_t;   /* je Plattform: gesperrte Volumes, Handles ... */

typedef struct {
    diskinfo_t      info;
    int             fd;     /* zum Lesen/Schreiben des ganzen Datenträgers */
    int             tfd;    /* für Echtheitsprüfung und Tempo: am Zwischenspeicher vorbei (Linux O_DIRECT, sonst = fd) */
    plat_session_t *p;
} session_t;

/* ------------------------------------------------------------------ gemeinsam (helper.c) */
extern FILE *out;
extern volatile int cancel_requested;
void        on_progress(const char *step, int pct);
void        finish(int ok, const char *msg, const char *json);
void        json_str(FILE *f, const char *s);
const char *size_text(uint64_t b, char *buf, size_t n);
/* Auftrag ausführen: argv[0] = list|format|layout|speed|label, danach dessen Angaben; Rückgabe = Exit-Code */
int         helper_command(int argc, char **argv);

/* ------------------------------------------------------------------ je Plattform */
int  plat_image_mode(void);   /* nur Test-Build: Abbild-Datei statt Datenträger */
int  plat_list(void);         /* USB-Datenträger als JSON nach out */
/* Datenträger öffnen (write: vorher alle seine Laufwerke aushängen/sperren). Nur USB/SD, nie die Systemplatte,
 * Größe muss zu "list" passen. */
int  plat_session_open(session_t *s, const char *disk, uint64_t size, int write, char *err, size_t n);
/* freigeben; changed: das System soll die (neue) Partitionstabelle einlesen */
void plat_session_close(session_t *s, int changed);
/* NTFS oder FAT16 in Partition index (0-basiert, Lage start/count in Sektoren) - mit den Mitteln des Systems.
 * serial: Seriennummer des vorläufigen exFAT (nur Windows). summary erhält z. B. "NTFS, 4 KB Cluster". */
int  plat_os_format(const diskinfo_t *di, int fs, int index, uint64_t start, uint64_t count, const char *label,
                    uint32_t cluster, uint32_t serial, int lo, int hi, char *summary, size_t sn, char *err, size_t n);
/* ext2/3/4 (type) mit mke2fs in Partition index */
int  plat_mke2fs(const diskinfo_t *di, int index, uint64_t start, uint64_t count, const char *type, uint32_t block,
                 const char *label, int lo, int hi, char *summary, size_t sn, char *err, size_t n);
/* Bezeichnung von NTFS/FAT16 über das System ändern. *handled = 0: kein solches Laufwerk, dann schreibt der
 * gemeinsame Teil selbst (exFAT/FAT32). di erhält die Angaben des Datenträgers. */
int  plat_os_label(const char *disk, uint64_t size, const char *label, diskinfo_t *di, int *handled, char *err,
                   size_t n);
/* Partitionstyp im MBR (für die Ergebnis-Daten), -1 = GPT/unbekannt */
int  plat_read_mbr_type(const diskinfo_t *di);

#endif
