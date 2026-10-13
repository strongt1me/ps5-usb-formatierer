#ifndef DEVTEST_H
#define DEVTEST_H

#include <stddef.h>
#include <stdint.h>

#include "mkfs.h"

/* Laufwerksprüfungen. fd: Gerät bzw. Image, ss: Sektorgröße, size: Bytes. *cancel wird regelmäßig geprüft. */
typedef struct {
    double   read_mbps, write_mbps;  /* MB/s (10^6 Bytes), 0 = nicht gemessen */
    int      fake;                   /* Stick meldet mehr Speicher, als er hat */
    uint64_t real_bytes;             /* bei fake: geschätzte echte Größe (auf 1 MiB abgerundet) */
    int      bad;                    /* Lese-/Schreibfehler oder falsche Daten gefunden */
    uint64_t bad_at;                 /* erste fehlerhafte Byte-Position */
    int      cancelled;
} devtest_t;

/* Lesetempo messen, zerstört nichts (bis 256 MiB bzw. 8 Sekunden) */
int dt_read_speed(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
                  char *err, size_t errlen);

/* Schreib-/Lesetempo und Echtheit (Stichproben über die ganze Größe). Zerstört Daten. */
int dt_verify(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
              char *err, size_t errlen);

/* Alles mit Nullen überschreiben und lesend prüfen. Zerstört Daten. Abbruch: r->cancelled, Rückgabe 0. */
int dt_full(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
            char *err, size_t errlen);

/* Bewertung für Spiele: "schnell", "ausreichend", "langsam" */
const char *dt_rating(double read_mbps);

#endif
