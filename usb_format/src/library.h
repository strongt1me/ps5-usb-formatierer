#ifndef LIBRARY_H
#define LIBRARY_H

#include <stddef.h>
#include <stdint.h>

/* Spiele auf einem Laufwerk finden, wie ShadowMountPlus sucht: in der Wurzel, in homebrew/ und etaHEN/games/ je
 * Spiele-Ordner (mit sce_sys/param.json bzw. param.sfo) und Abbilder (.exfat, .ffpkg, .ffpfs, .ffpfsc).
 * Titel-ID und Name stehen in sce_sys/param.json (PS5) bzw. param.sfo (PS4), bei Abbildern im Abbild selbst. */
typedef struct {
    char     id[16];       /* z. B. "PPSA01234", leer wenn nicht lesbar */
    char     name[128];    /* Spielname (deutsch, sonst Standardsprache), sonst Dateiname */
    char     path[256];
    char     kind[8];      /* "Ordner", "exFAT", "UFS2", "PFS" */
    uint64_t size;         /* Abbild: Dateigröße; Ordner: 0 */
} lib_title_t;

typedef struct {
    char         root[96];
    lib_title_t *titles;
    int          ntitles, cap;
    int          pkgs;           /* .pkg-Dateien (Wurzel, pkg/, homebrew/) */
    uint64_t     pkg_bytes;
    char         folders[512];   /* Ordner in der Wurzel, durch '/' getrennt, ohne Systemordner */
} lib_root_t;

/* Titel-ID/Name zu einem Abbild-Pfad, wie ShadowMountPlus sie kennt. Für .ffpfs/.ffpfsc (verschlüsselt bzw.
 * komprimiert - liest nur der PS5-Kernel) die einzige Quelle; ergänzt auch fehlende Namen. */
typedef struct {
    char path[256];
    char id[16];
    char name[128];
} lib_hint_t;

/* Hinweise für folgende lib_scan-Aufrufe setzen (Kopie; NULL/0 löscht) */
void lib_set_hints(const lib_hint_t *h, int n);
/* Antwort von POST /api/v1/games (ShadowMountPlus) bzw. sein debug.log auswerten; hängt an *h an (realloc) */
int  lib_parse_smp_games(const char *json, size_t len, lib_hint_t **h, int *n);
int  lib_parse_smp_log(const char *log, size_t len, lib_hint_t **h, int *n);

/* usb_root: Wurzel eines Laufwerks (sucht auch homebrew/, etaHEN/games/, pkg/); sonst nur der Ordner selbst */
int  lib_scan(const char *root, int usb_root, lib_root_t *out);
void lib_free(lib_root_t *r);

/* Feld der obersten Ebene eines JSON-Objekts (Antworten von ShadowMountPlus); 0 = gefunden */
int lib_json_str(const char *json, size_t len, const char *key, char *out, size_t n);
int lib_json_num(const char *json, size_t len, const char *key, long long *v);

/* für Tests: Titel-ID und Name aus param.json bzw. param.sfo; 0 = Titel-ID gefunden */
int lib_parse_param_json(const char *json, size_t len, char *id, size_t idn, char *name, size_t namen);
int lib_parse_param_sfo(const uint8_t *b, size_t len, char *id, size_t idn, char *name, size_t namen);

#endif
