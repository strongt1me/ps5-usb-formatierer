/* chives.json - die Beschreibung des Abbilds neben den Dateien eines ausgelesenen PS5-Spielstands
 * (übernommen aus dem PS5 Cooling Center, chives.h; dasselbe Format wie Garlic SaveMgr):
 *
 *   { "schema_version": 1, "platform": "PS5", "title_id": "...", "save_directory": "...", "encrypted_name": "sdimg_...",
 *     "original_image_size_bytes": N,
 *     "pfs": { "version": 2, "block_size_bytes": 32768|65536, "block_count": N, "mode_raw": N, "backup_header_count": N, "fs_ext": N },
 *     "ownership": [ { "path": "...", "uid": N, "gid": N, "mode": N }, ... ] }
 *
 * Alles aus einer ZIP-Datei gilt als feindlich: Größen, Zahlen und Namen werden geprüft. */
#ifndef CHIVES_H
#define CHIVES_H

#include <stddef.h>
#include <stdint.h>

#define CHIVES_MAX_JSON       262144u                    /* größere Beschreibungen werden abgelehnt */
#define CHIVES_MAX_OWN_READ   2048u                      /* Besitzer-Einträge beim Lesen */
#define CHIVES_MAX_OWN_WRITE  256u                       /* ... und beim Schreiben (Garlic lehnt mehr ab: dann keine) */
#define CHIVES_MAX_IMAGE      (UINT64_C(64) << 30)
#define CHIVES_PATH_MAX       256u

typedef struct {
    char     path[CHIVES_PATH_MAX];
    uint32_t uid, gid;
    uint16_t mode;                                       /* nur Rechte (07777) */
} chives_own_t;

typedef struct {
    uint64_t version;                                    /* 2 = PS5-Spielstand */
    uint64_t block_count;
    uint64_t image_size;                                 /* Größe der Abbild-Datei */
    uint32_t block_size, backup_header_count, fs_ext;
    uint16_t mode_raw;
    int      has_fs_ext;
    char     title_id[16];
    char     save_dir[96];
    char     image_name[128];                            /* Dateiname des Abbilds auf der Konsole (sdimg_...) */
    chives_own_t *own;                                   /* gehört der Struktur: chives_free */
    unsigned nown;
    unsigned own_cap;
} chives_t;

void chives_init(chives_t *g);
void chives_free(chives_t *g);
int  chives_own_add(chives_t *g, const char *path, uint32_t uid, uint32_t gid, unsigned mode);
/* Aufbau eines PS5-Abbilds aus seinen ersten Bytes (mind. 0x40, 0x36c für fs_ext) und seiner Größe. 0 = gültig, sonst -1 */
int  chives_from_header(const uint8_t *hdr, size_t len, uint64_t file_size, chives_t *g);
int  chives_valid(const chives_t *g);
/* 1 bei einem sicheren relativen Pfad (nicht absolut, kein "..", kein Backslash/Doppelpunkt, kein leerer Teil) */
int  chives_path_safe(const char *name);
/* 1, wenn der ZIP-Eintrag die Beschreibung selbst ist ("chives.json", beliebige Schreibweise, optional "./" davor) */
int  chives_is_sidecar(const char *name);
/* JSON-Text: Länge, oder -1, wenn er nicht in cap passt */
int  chives_write(const chives_t *g, char *out, size_t cap);
/* Beschreibung lesen: 0 (g gefüllt, mit chives_free freigeben), -1 = keine gültige Beschreibung eines PS5-Spielstands */
int  chives_parse(const char *json, size_t len, chives_t *g);

#endif
