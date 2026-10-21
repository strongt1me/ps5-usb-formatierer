/* PS5-Spielstand-Sicherung, Wiederherstellung und Neusignierung (nur PS5-Payload) */
#ifndef SAVES_H
#define SAVES_H

#include <stddef.h>
#include <stdint.h>

#define SAVE_MAX 512

typedef struct {
    char uid[16];           /* z. B. "10000000" */
    char username[64];      /* Anzeigename aus /user/home/<uid>/username.dat */
    char account_id[17];    /* 16-stellige Hex-Account-ID aus der Registry */
} save_user_t;

typedef struct {
    char path[512];         /* vollständiger Pfad zum Container-Image */
    char title_id[32];      /* z. B. PPSA01234 */
    char save_dir[256];     /* Verzeichnisname (z. B. savedata0) */
    char title_name[256];   /* Spielname (aus param.sfo des Spiels); leer = nicht gefunden */
    char uid[16];           /* Benutzer-ID */
    char username[64];      /* Benutzername */
} save_info_t;

/* Benutzer auflisten (alloziert *out; Aufrufer gibt frei) */
int  saves_scan_users(save_user_t **out, int *count);

/* Spielstände auflisten (alloziert *out; Aufrufer gibt frei) */
int  saves_scan(save_info_t **out, int *count);

/* Spielstand als ZIP über den Socket streamen (chunked HTTP) */
void saves_download(int sock, const save_info_t *s, const char *filename);

/* Spielstand aus hochgeladenem ZIP-Body wiederherstellen */
int  saves_restore(const char *body, size_t bodylen, char *err, size_t errsz);

/* Spielstand neusignieren: Account-ID in sce_sys/param.sfo ändern */
int  saves_resign(const save_info_t *s, const char *new_aid_hex, char *err, size_t errsz);

#endif
