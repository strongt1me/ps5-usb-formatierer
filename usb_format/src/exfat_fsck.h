#ifndef EXFAT_FSCK_H
#define EXFAT_FSCK_H

#include <stdint.h>

/* exFAT prüfen bzw. reparieren mit fsck.exfat aus exfatprogs (third_party/exfatprogs, im Formatierer übersetzt).
 * Läuft auf dem exklusiv geöffneten Datenträger: Die PS5 kann ihn währenddessen nicht einbinden. */
typedef struct {
    int  code;          /* 0 fehlerfrei, 1 Fehler behoben, 4 Fehler übrig, 8 Prüfen nicht möglich */
    long dirs, files;   /* laut Zusammenfassung von fsck.exfat, -1 = unbekannt */
    long corrupted, fixed;
    char log[2048];     /* letzte Meldungen von fsck.exfat (englisch) */
} fsck_result_t;

/* Dateisystem ab Byte start (size Bytes) auf fd prüfen; repair: Fehler ohne Rückfrage beheben. 0 = gelaufen */
int exfat_fsck_run(int fd, uint64_t start, uint64_t size, uint32_t ss, int repair, fsck_result_t *r);

/* deutsche Zusammenfassung des Ergebnisses */
void exfat_fsck_summary(const fsck_result_t *r, int repair, char *out, size_t n);

#endif
