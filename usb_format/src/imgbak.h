#ifndef IMGBAK_H
#define IMGBAK_H

#include <stddef.h>
#include <stdint.h>

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
typedef int  (*ib_out_fn)(void *ctx, const void *p, size_t n);   /* 0 = geschrieben */
typedef long (*ib_in_fn)(void *ctx, void *p, size_t n);          /* gelesene Bytes, 0 = Ende, < 0 = Fehler */

/* Angaben im Kopf eines Images */
typedef struct {
    uint64_t size;          /* Bytes des Images (ausgepackt) */
    uint64_t used;          /* davon belegt */
    char     model[128], label[48], fs[16], ident[64], created[24];
    int      has_map;       /* mit Belegungskarte (eigenes Image) - sonst wird alles geschrieben */
} ib_info_t;

typedef struct {
    uint64_t size, used;    /* Image-Bytes, davon gelesen bzw. geschrieben */
    uint64_t file_bytes;    /* Größe der .img.zst */
    char     sha_img[65];   /* SHA-256 der belegten Blöcke (fremde Images ohne Karte: des ganzen Images) */
    char     sha_file[65];  /* SHA-256 der .img.zst (für die .sha256 daneben) */
    int      cancelled;
} ib_result_t;

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

/* Dateiname "<Modell>_<Bezeichnung>_<Zeit>.img.zst" aus Angaben (nur A-Z a-z 0-9 - _) */
void ib_file_name(const ib_info_t *info, char *out, size_t n);

#endif
