/* ZIP-Dateien für Spielstände (übernommen aus dem PS5 Cooling Center, savezip.h).
 *
 * Geschrieben: Einträge ungepackt (Größe und Prüfsumme stehen vorher fest - die lokalen Köpfe tragen sie, wie die
 * Werkzeuge es brauchen), UTF-8-Namen, Unix-Rechte in den Attributen. Gelesen: ungepackte und Deflate-Einträge, Größen aus
 * dem zentralen Verzeichnis, jede Prüfsumme geprüft, jeder Name geprüft (nicht absolut, kein "..", kein Backslash,
 * keine Steuerzeichen). Kein ZIP64: 4 GiB und mehr werden abgelehnt.
 *
 * Erweitert für den Formatierer: geschrieben wird in eine Datei oder in eine beliebige Senke (HTTP-Antwort). */
#ifndef SAVEZIP_H
#define SAVEZIP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

enum {
    SZ_OK = 0,
    SZ_E_IO = -1,        /* Lesen, Schreiben oder Öffnen fehlgeschlagen (errno gesetzt) */
    SZ_E_TOO_BIG = -2,   /* jenseits dessen, was das Format ohne ZIP64 kann, oder über der Grenze */
    SZ_E_BAD = -3,       /* keine, eine kaputte oder eine nicht lesbare ZIP (ZIP64, verschlüsselt, andere Methode) */
    SZ_E_NAME = -4,      /* unsicherer Eintragsname */
    SZ_E_CRC = -5,       /* Daten passen nicht zur Prüfsumme */
    SZ_E_CHANGED = -6,   /* Datei hat sich beim Lesen verändert */
    SZ_E_NOMEM = -7,
    SZ_E_STOP = -8       /* abgebrochen */
};

#define SZ_MAX_ENTRIES 60000u
#define SZ_MAX_NAME    500u
#define SZ_MAX_COMMENT 255u

int savezip_name_ok(const char *name);
uint32_t savezip_crc32(uint32_t crc, const void *buf, size_t len);

/* ── Schreiben ── */
typedef struct szw szw_t;
typedef int (*sz_tick_fn)(void *ctx, uint64_t bytes);              /* != 0: abbrechen (SZ_E_STOP) */
typedef int (*sz_sink_fn)(void *ctx, const void *buf, size_t n);   /* 0 = geschrieben */

szw_t *szw_create(const char *path);                    /* neue Datei (nie überschreiben); NULL bei Fehler */
szw_t *szw_open_sink(sz_sink_fn sink, void *ctx);       /* in eine Senke */
int szw_add_mem(szw_t *z, const char *name, const void *data, size_t len, time_t mtime, unsigned mode);
/* src zweimal lesen (Prüfsumme, dann Daten); hat sie sich dazwischen geändert: SZ_E_CHANGED */
int szw_add_file(szw_t *z, const char *name, const char *src, time_t mtime, unsigned mode, sz_tick_fn tick, void *ctx);
void szw_comment(szw_t *z, const char *text);           /* Kommentar der ZIP (höchstens SZ_MAX_COMMENT Zeichen) */
int szw_finish(szw_t *z);                               /* Verzeichnis, bei Dateien fsync; gibt frei; Fehler: Datei gelöscht */
void szw_abort(szw_t *z);                               /* schließt, gibt frei, löscht eine Datei */

/* ── Lesen ── */
typedef struct {
    char     name[SZ_MAX_NAME + 1];
    uint16_t method;     /* 0 ungepackt, 8 Deflate */
    uint16_t dtime, ddate;
    uint32_t crc;
    uint64_t csize, usize;
    uint32_t lho;        /* Lage des lokalen Kopfs */
    uint32_t attr;       /* externe Attribute (Unix-Rechte: attr >> 16) */
    int      is_dir;
} szr_entry_t;

typedef struct szr szr_t;

szr_t *szr_open(const char *path, int *err);
unsigned szr_count(const szr_t *z);
const szr_entry_t *szr_at(const szr_t *z, unsigned i);
/* ganzer Eintrag in einen neuen Puffer (free()), Prüfsumme geprüft; usize > max_len: SZ_E_TOO_BIG */
int szr_read(szr_t *z, unsigned i, uint8_t **out, size_t *len, size_t max_len);
/* Eintrag nach fd (ungepackt in Stücken; Deflate über den Speicher bis max_mem), Prüfsumme geprüft */
int szr_extract(szr_t *z, unsigned i, int fd, size_t max_mem, sz_tick_fn tick, void *ctx);
void szr_close(szr_t *z);
const char *szr_comment(const szr_t *z);                /* Kommentar der ZIP ("" ohne) */
/* DOS-Zeit eines Eintrags als Unix-Zeit (UTC), 0 = unbekannt */
time_t szr_mtime(const szr_entry_t *e);

#endif
