#ifndef MKFS_H
#define MKFS_H

#include <stdint.h>

/* Für die PS5: FS_EXFAT (64 KB) und FS_FAT32. NTFS/ext4 schreiben externe Werkzeuge (mkntfs, mke2fs).
 * FS_HFSPLUS (Mac OS Extended) schreibt hfsplus.c. */
enum { FS_EXFAT = 1, FS_FAT32 = 2, FS_NTFS = 3, FS_EXT4 = 4, FS_HFSPLUS = 5 };

/* Partitionstyp im Expertenmodus: PT_AUTO = passend zum Dateisystem */
enum { PT_AUTO = 0, PT_WINDOWS, PT_LINUX, PT_SWAP, PT_EFI, PT_APPLE };

#define MAX_PARTS 16   /* Expertenmodus: höchstens so viele Partitionen (MBR: 4) */

/* Partitionstabelle: automatisch MBR, GPT erst, wenn MBR den Datenträger nicht mehr fassen kann (> 2 TiB) */
enum { LAYOUT_AUTO = 0, LAYOUT_MBR = 1, LAYOUT_GPT = 2 };

/* Schritt-Text und Prozent (0-100) innerhalb des Schritts */
typedef void (*progress_fn)(const char *step, int percent);

typedef struct {
    int         fd;
    uint32_t    ss;       /* Sektorgroesse in Bytes */
    uint64_t    sectors;  /* Sektoren des gesamten Geraets */
    uint64_t    base;     /* Byte-Offset, ab dem mkfs_* schreibt (Partitionsbeginn) */
    progress_fn progress; /* optional */
} blkdev_t;

/* Datei, die gleich beim Formatieren angelegt wird (nur exFAT; genutzt von den Tests) */
typedef struct {
    const char *path;     /* z. B. "PPSA01234/sce_sys/param.json" */
    const void *data;
    uint32_t    len;
} mkfs_file_t;

/* Zusätzliche Wünsche, alle optional (Nullen = Standard) */
typedef struct {
    int                scheme;  /* LAYOUT_* */
    uint64_t           limit;   /* nur die ersten limit Bytes nutzen (Stick mit gefälschter Größe), 0 = alles */
    const char *const *dirs;    /* Ordner anlegen (exFAT/FAT32), z. B. "homebrew", "etaHEN/games"; NULL am Ende */
    const mkfs_file_t *files;   /* Dateien anlegen (nur exFAT) */
    int                nfiles;
    uint8_t            mbr_type; /* Partitionstyp im MBR statt des passenden, 0 = passend (Windows-Helfer: FAT16) */
    int                is_gpt;  /* Ausgabe: es wurde GPT geschrieben */
} mkfs_opts_t;

#define EXFAT_CLUSTER_SIZE (64u * 1024u)

const char *mkfs_error(void);
const char *mkfs_summary(void);
int label_max(int fs);
int label_valid(const char *label, int fs);

/* Partitionstabelle (MBR oder GPT) mit einer Partition ab 1 MiB + FAT32/exFAT. cluster = Zuordnungseinheit in
 * Bytes, 0 = Standard (exFAT 64 KB, FAT32 wie Windows). o darf NULL sein. Loescht alle Daten. */
int format_disk(blkdev_t *d, int fs, const char *label, uint32_t serial, uint32_t cluster, mkfs_opts_t *o);

/* Fuer NTFS/ext4: Partitionstabelle mit passendem Partitionstyp schreiben und alte Signaturen loeschen; das
 * Dateisystem selbst schreibt danach das externe Werkzeug in die Partition (start und count in Sektoren). */
int prepare_partition(blkdev_t *d, int fs, const char *label, uint32_t serial, mkfs_opts_t *o,
                      uint64_t *start, uint64_t *count);

/* Nur Dateisystem ab d->base; hidden = Partitionsbeginn in Sektoren (fuer den Bootsektor). o darf NULL sein. */
int mkfs_fat32(blkdev_t *d, uint64_t hidden, uint64_t nsect, const char *label, uint32_t serial, uint32_t cluster,
               const mkfs_opts_t *o);
int mkfs_exfat(blkdev_t *d, uint64_t hidden, uint64_t nsect, const char *label, uint32_t serial, uint32_t cluster,
               const mkfs_opts_t *o);
/* Mac OS Extended (HFS+, ohne Journal); block = Zuordnungsblock in Bytes, 0 = 4 KB (hfsplus.c) */
int mkfs_hfsplus(blkdev_t *d, uint64_t nsect, const char *label, uint32_t serial, uint32_t block);

/* Expertenmodus: eine Partition */
typedef struct {
    int         fs;         /* FS_* (bestimmt den passenden Partitionstyp), 0 = ohne Dateisystem */
    int         ptype;      /* PT_*, PT_AUTO = passend zu fs */
    uint8_t     mbr_type;   /* MBR-Typ ausdrücklich (z. B. 0x0E für FAT16), 0 = aus fs/ptype */
    int         active;     /* MBR: startfähig */
    uint64_t    bytes;      /* gewünschte Größe; 0 = der ganze Rest (nur die letzte Partition) */
    const char *name;       /* GPT-Partitionsname (ASCII), NULL/leer = "Daten" */
    uint64_t    start, count;  /* Ergebnis: Lage in Sektoren */
} part_t;

/* Partitionstabelle mit n Partitionen (MBR höchstens 4, GPT höchstens MAX_PARTS), jede ab einer MiB-Grenze; löscht
 * alte Tabellen sowie die erste und letzte MiB jeder Partition (alte Signaturen). limit wie mkfs_opts_t.limit.
 * Die Dateisysteme schreibt der Aufrufer danach (mkfs_* mit d->base = start * ss). Löscht alle Daten. */
int layout_disk(blkdev_t *d, int scheme, uint64_t limit, part_t *p, int n, uint32_t serial, int *is_gpt);
/* nur die Lage berechnen (start/count, GPT ja/nein) - schreibt nichts; so lässt sich vorher prüfen, ob jedes
 * Dateisystem an seine Stelle passt */
int layout_plan(const blkdev_t *d, int scheme, uint64_t limit, part_t *p, int n, int *is_gpt);

/* Fortschritt für den nächsten Einzelaufruf (mkfs_* direkt) wieder bei 0 beginnen lassen */
void mkfs_progress_reset(void);

/* CRC-32 (IEEE 802.3) wie für GPT */
uint32_t mkfs_crc32(const void *p, uint64_t len);

#endif
