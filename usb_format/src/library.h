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
    char     ext[8];       /* Abbild: Dateiendung ohne Punkt ("exfat", "ffpkg", "ffpfs", "ffpfsc"), sonst leer */
    uint64_t size;         /* Abbild: Dateigröße; Ordner: 0 */
    char     version[16];  /* installierte Version laut param.json/param.sfo, leer wenn unbekannt */
} lib_title_t;

/* .pkg-Datei (Installationspaket) */
typedef struct {
    char     path[256];
    char     platform[4];  /* "PS4" (Kopf 7F 43 4E 54), "PS5" (7F 4C 49 48 bzw. 7F 46 49 48) oder "" */
    char     type[8];      /* PS5: "PKG" (offiziell signiert, Byte 0x05 = 0x80) oder "fPKG" (Debug-Signatur); PS4: "" (aus dem Kopf nicht zu erkennen) */
    char     title_id[12]; /* aus der Content-ID im Kopf (PS4) bzw. dem Dateinamen, sonst leer */
    int      update;       /* PS4: Patch laut Inhalts-Flags (0x40000000 = Folgepatch, 0x00100000 = erster Patch) */
    uint64_t size;
    /* aus den Metadaten im Paket (PS4: param.sfo, PS5: param.json); leer, wenn sie nicht lesbar sind */
    char     title[96];    /* Spielname */
    char     version[16];  /* Inhaltsversion, z. B. "01.02" bzw. "01.010.000" */
    char     cat[8];       /* "game", "update" oder "dlc" */
    char     minfw[12];    /* mindestens nötige Firmware, z. B. "9.00" */
} lib_pkg_t;

typedef struct {
    char         root[96];
    lib_title_t *titles;
    int          ntitles, cap;
    int          pkgs;           /* .pkg-Dateien (Wurzel, pkg/, homebrew/, Unterordner von ps4 und PS5-Updates) */
    uint64_t     pkg_bytes;
    lib_pkg_t   *pkglist;        /* davon die ersten LIB_PKG_MAX */
    int          npkglist;
    char         folders[512];   /* Ordner in der Wurzel, durch '/' getrennt, ohne Systemordner */
} lib_root_t;

#define LIB_PKG_MAX 512

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
/* Eintrag einer Titel-ID in der Spieleliste von ShadowMountPlus (/api/v1/games): angemeldete Version, Pfad der
 * Quelle, ob angemeldet. 1 = gefunden, 0 = nicht in der Liste, -1 = keine gültige Liste */
int  lib_smp_title(const char *json, size_t len, const char *id, char *version, size_t vn, char *path, size_t pn,
                   int *installed);
int  lib_parse_smp_log(const char *log, size_t len, lib_hint_t **h, int *n);

/* usb_root: Wurzel eines Laufwerks (sucht auch homebrew/, etaHEN/games/, pkg/); sonst nur der Ordner selbst */
int  lib_scan(const char *root, int usb_root, lib_root_t *out);
void lib_free(lib_root_t *r);

/* Liegt unter path noch das Spiel, das die Suche dort fand (Titel-ID; bei Abbildern auch die Größe)? Schützt vor dem
 * Löschen eines inzwischen dort abgelegten anderen Spiels. 1 = ja. Liest keine Hinweise (thread-sicher). */
int  lib_same_game(const char *path, const char *id, uint64_t size);

/* Feld der obersten Ebene eines JSON-Objekts (Antworten von ShadowMountPlus); 0 = gefunden */
int lib_json_str(const char *json, size_t len, const char *key, char *out, size_t n);
int lib_json_num(const char *json, size_t len, const char *key, long long *v);

/* für Tests: Titel-ID und Name aus param.json bzw. param.sfo; 0 = Titel-ID gefunden */
int lib_parse_param_json(const char *json, size_t len, char *id, size_t idn, char *name, size_t namen);
int lib_parse_param_sfo(const uint8_t *b, size_t len, char *id, size_t idn, char *name, size_t namen);

#endif
