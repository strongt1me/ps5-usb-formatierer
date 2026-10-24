#ifndef IMGBAK_H
#define IMGBAK_H

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/*
 * Image-Sicherung eines Datenträgers als .img.zst - eine gewöhnliche zstd-Datei (mit "zstd -d" zu einem rohen
 * Image entpackbar). Vorn steht ein überspringbarer zstd-Rahmen mit Angaben und der Belegungskarte, hinten einer
 * mit der SHA-256 der belegten Blöcke; zstd selbst ignoriert beide (den ganzen Inhalt prüft seine XXH64-Prüfsumme).
 *
 * Belegungskarte: 64-KiB-Blöcke. Freie Cluster von exFAT (Zuordnungs-Bitmap) und FAT32 (FAT) werden nicht gelesen,
 * sondern als Nullen gesichert (komprimieren auf fast nichts) und beim Wiederherstellen nicht geschrieben. Alles
 * andere - Partitionstabellen, Verwaltungsbereiche, andere Dateisysteme - wird vollständig gesichert.
 */

#define IB_BLOCK (64u * 1024u)

typedef void (*ib_progress_fn)(const char *step, int pct);

/* Kompressions-Threads beim Sichern: -1 = nach Zahl der Kerne (höchstens 4), 0 = im aufrufenden Thread */
extern int ib_threads;
typedef int  (*ib_out_fn)(void *ctx, const void *p, size_t n);   /* 0 = geschrieben */
typedef long (*ib_in_fn)(void *ctx, void *p, size_t n);          /* gelesene Bytes, 0 = Ende, < 0 = Fehler */

/* Angaben im Kopf eines Images */
typedef struct {
    uint64_t size;          /* Bytes des Images (ausgepackt) */
    uint64_t used;          /* davon belegt */
    char     model[128], label[48], fs[16], ident[64], created[24];
    int      has_map;       /* mit Belegungskarte (eigenes Image) - sonst wird alles geschrieben */
    uint32_t ss;            /* Sektorgröße der Quelle (ab 2.5.9 im Kopf; 0 = unbekannt) */
} ib_info_t;

typedef struct {
    uint64_t size, used;    /* Image-Bytes, davon gelesen bzw. geschrieben */
    uint64_t file_bytes;    /* Größe der .img.zst */
    char     sha_img[65];   /* SHA-256 der belegten Blöcke (fremde Images ohne Karte: des ganzen Images) */
    char     sha_file[65];  /* SHA-256 der .img.zst (für die .sha256 daneben; Wiederherstellen/Prüfen: der gelesenen) */
    int      cancelled;
    int      checked;       /* Prüfsumme am Ende des Images verglichen (eigene Images) */
} ib_result_t;

/* ---- Fortsetzbare Sicherung ----
 * Daten stehen in Rahmen von höchstens IB_FRAME_MAX Quell-Bytes. Nach jedem fertigen Rahmen (und jeder freien Strecke) kann der
 * Aufrufer einen Merkpunkt speichern; mit ihm macht ib_backup_ex dort weiter, wo eine Sicherung aufhörte. Die fertige Datei ist
 * dieselbe wie bei einem Lauf ohne Unterbrechung (außer dem Zeitstempel im Kopf, der aus dem ersten Lauf stammt). */
#define IB_FRAME_MAX (1ull << 30)
extern uint64_t ib_ckpt_bytes;  /* Merkpunkt frühestens nach so vielen Ausgabe-Bytes (Standard 64 MiB) ... */
extern int      ib_ckpt_secs;   /* ... oder so vielen Sekunden (Standard 5); Tests setzen beides auf 0 */
extern uint64_t ib_frame_max;   /* Rahmengröße (Tests verkleinern sie); Standard IB_FRAME_MAX, ein Vielfaches von IB_BLOCK */

typedef struct {
    uint64_t start, len;    /* Quell-Bytes des Rahmens */
    uint8_t  sha[32];       /* SHA-256 der belegten Blöcke darin (Quelle) */
} ib_frame_t;

typedef struct {
    uint64_t   seg;         /* Position im Image, ab der weitergemacht wird (Rahmengrenze) */
    uint64_t   file_bytes;  /* so viele Bytes der .img.zst sind bis dahin geschrieben */
    uint64_t   used_done;   /* belegte Bytes bis dahin gelesen */
    sha256_t   sha_file;    /* Zwischenstand der SHA-256 über die Datei */
    sha256_t   sha_used;    /* ... und über die belegten Blöcke */
    uint8_t    map_fp[32];  /* SHA-256 der Belegungskarte: dasselbe Laufwerk-Layout? */
    uint64_t   size, used;  /* Größe des Images und belegte Bytes laut Karte */
    ib_frame_t *frames;     /* fertige Datenrahmen (für die gründliche Prüfung beim Fortsetzen) */
    int        nframes;
} ib_ckpt_t;

/* Aufrufer speichert den Merkpunkt (Ausgabedatei vorher auf den Datenträger schreiben). 0 = gespeichert */
typedef int (*ib_ckpt_fn)(void *ctx, const ib_ckpt_t *c);

typedef struct {
    ib_ckpt_fn       save;
    void            *save_ctx;
    const ib_ckpt_t *resume;    /* fortsetzen (NULL: neue Sicherung) - die Ausgabe muss bei file_bytes stehen */
    int              thorough;  /* beim Fortsetzen die gesicherten Rahmen der Quelle noch einmal lesen und vergleichen */
} ib_resume_t;

int ib_backup_ex(int fd, uint32_t ss, uint64_t size, ib_info_t *info, ib_out_fn out, void *octx,
                 ib_progress_fn progress, volatile int *cancel, ib_result_t *r, char *err, size_t errlen,
                 const ib_resume_t *rs);

/* Belegte Bytes ermitteln (nur lesen) - für die Anzeige vorab und die Platzprüfung am Ziel */
uint64_t ib_used_bytes(int fd, uint32_t ss, uint64_t size);

/* Datenträger (bzw. die ersten size Bytes) sichern. info: model/label/fs/ident/created vom Aufrufer (size/used
 * füllt ib_backup). 0 = fertig, -1 = Fehler (err). Abgebrochen: 0 mit r->cancelled. */
int ib_backup(int fd, uint32_t ss, uint64_t size, ib_info_t *info, ib_out_fn out, void *octx,
              ib_progress_fn progress, volatile int *cancel, ib_result_t *r, char *err, size_t errlen);

/* Wiederherstellen in zwei Schritten: ib_open liest den Kopf (Größe prüfen, bevor der Datenträger übernommen
 * wird), ib_restore schreibt und prüft danach durch Nachlesen. */
typedef struct ib_reader ib_reader_t;
ib_reader_t *ib_open(ib_in_fn in, void *ctx, ib_info_t *info, char *err, size_t errlen);
int  ib_restore(ib_reader_t *rd, int fd, int vfd, uint32_t ss, uint64_t devsize, ib_progress_fn progress,
                volatile int *cancel, ib_result_t *r, char *err, size_t errlen);   /* vfd: zum Nachlesen */
void ib_close(ib_reader_t *rd);
/* Image nur prüfen, ohne Datenträger: alles entpacken, Aufbau und die Prüfsumme am Ende vergleichen (eigene Images
 * müssen eine haben, sonst gilt die Datei als unvollständig), die Datei bis zum Ende lesen. r->used = geprüfte
 * belegte Bytes, r->sha_file/file_bytes = SHA-256 und Größe der ganzen Datei (zum Vergleich mit der .sha256). */
int  ib_verify(ib_reader_t *rd, ib_progress_fn progress, volatile int *cancel, ib_result_t *r, char *err, size_t errlen);

/* Dateiname "<Modell>_<Bezeichnung>_<Zeit>.img.zst" aus Angaben (nur A-Z a-z 0-9 - _) */
void ib_file_name(const ib_info_t *info, char *out, size_t n);

#endif
