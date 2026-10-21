/* PS5-Spielstand-Sicherung, Wiederherstellung und Neusignierung */
#ifndef SAVES_H
#define SAVES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char uid[16];           /* Ordnername unter /user/home, z. B. "1c3f8a0e" */
    char username[64];      /* Anzeigename aus <uid>/username.dat */
    char account_id[17];    /* 16-stellige Hex-Account-ID aus der Registry ("" = nicht gefunden) */
} save_user_t;

typedef struct {
    char     key[320];      /* "<uid>/<title_id>/<save_dir>" - eindeutige Kennung, auch nach Neuaufbau der Liste gleich */
    char     path[512];     /* vollständiger Pfad zum Container-Image */
    char     title_id[32];  /* z. B. PPSA01234 */
    char     save_dir[256]; /* Dateiname des Containers im Titel-Ordner */
    char     title_name[256]; /* Spielname (param.sfo des Spiels); sonst die Titel-ID */
    char     uid[16];
    char     username[64];
    uint64_t size;
} save_info_t;

/* Namen aus Ordnern und aus hochgeladenen Metadaten dürfen nur Buchstaben, Ziffern, "_", "-" und "." enthalten,
 * nicht mit "." beginnen und höchstens max-1 Zeichen lang sein (sonst Pfad-Ausbruch wie "../../") */
int  saves_name_ok(const char *s, size_t max);

/* Benutzer / Spielstände auflisten (alloziert *out, Aufrufer gibt frei; Spielstände sortiert nach Benutzer, Titel, Name).
 * -1 = Ordner nicht lesbar. */
int  saves_scan_users(save_user_t **out, int *count);
int  saves_scan(save_info_t **out, int *count);

/* Spielstand über die Kennung aus saves_scan finden. 0 = gefunden */
int  saves_find(const char *key, save_info_t *out);

/* Spielstand als ZIP (metadata.json + Container) über den Socket streamen (chunked HTTP).
 * 0 = fertig gesendet; -1 = Fehler, bevor etwas gesendet wurde (err gefüllt, Aufrufer antwortet);
 * -2 = Fehler mitten im Strom (Antwort bewusst ohne Abschluss, der Aufrufer schließt die Verbindung). */
int  saves_download(int sock, const save_info_t *s, char *err, size_t errsz);

/* Spielstand aus einer ZIP-Datei (Ergebnis von saves_download) zurückschreiben. 0 = ok, sonst Fehlertext in err */
int  saves_restore_file(const char *zip_path, char *err, size_t errsz);

/* Spielstand auf einen anderen lokalen Benutzer (Ordnername target_uid) neu signieren: Account-ID und Benutzer-ID
 * in sce_sys/param.sfo des Containers ändern. 0 = ok, sonst Fehlertext in err */
int  saves_resign(const save_info_t *s, const char *target_uid, char *err, size_t errsz);

/* param.sfo: ACCOUNT_ID (8 Byte) und die Benutzer-ID in PARAMS (+4, 4 Byte) ändern (Test: ohne PS5 prüfbar) */
int  saves_sfo_patch(const char *path, const uint8_t aid[8], uint32_t user_id, char *err, size_t errsz);

/* Arbeitsordner für temporäre Container-Kopien (PS5: /data/usb_format/save_tmp) */
const char *saves_workdir(void);

#endif
