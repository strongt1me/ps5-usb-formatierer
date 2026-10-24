/* PS5-Spielstand-Sicherung und Wiederherstellung */
#ifndef SAVES_H
#define SAVES_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    char uid[16];           /* Ordnername unter /user/home, z. B. "1c3f8a0e" */
    char username[64];      /* Anzeigename aus <uid>/username.dat */
} save_user_t;

typedef struct {
    char     key[320];      /* "<uid>/<title_id>/<save_dir>" - eindeutige Kennung, auch nach Neuaufbau der Liste gleich */
    char     path[512];     /* vollständiger Pfad zum Container-Image */
    char     title_id[32];  /* z. B. PPSA01234 */
    char     save_dir[256]; /* Dateiname des Containers im Titel-Ordner */
    char     title_name[256]; /* Spielname (param.sfo des Spiels); sonst die Titel-ID */
    char     uid[16];
    char     username[64];
    char     platform[8];   /* "PS5" (savedata_prospero) oder "PS4" (savedata: Container sdimg_<Name> + <Name>.bin) */
    char     key_path[512]; /* PS4: die Schlüsseldatei <Name>.bin (96 Byte, verschlüsselter Schlüssel), sonst leer */
    uint64_t size;
    time_t   mtime;         /* Änderungszeit des Containers (= die der PS5-Datenbank) */
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

/* Fortschritt: done von total Byte des Spielstands (bei einer ZIP mit mehreren: der ganzen ZIP) sind geschrieben */
typedef int (*saves_prog_fn)(void *ctx, uint64_t done, uint64_t total);   /* Rückgabe != 0: abbrechen */

/* ZIP (chunked HTTP) mit den Spielständen list[0..n-1] an den Socket streamen. Ein Spielstand: metadata.json und der
 * Container im Wurzelordner der ZIP; mehrere: je Spielstand der Ordner "<uid>_<Titel>_<Name>/". Der verschlüsselte
 * Container bleibt unverändert.
 * 0 = fertig gesendet; -1 = Fehler, bevor etwas gesendet wurde (err gefüllt, Aufrufer antwortet);
 * -2 = Fehler mitten im Strom (Antwort bewusst ohne Abschluss, der Aufrufer schließt die Verbindung). */
int  saves_download(int sock, const save_info_t *list, int n, char *err, size_t errsz);

/* Spielstand als ZIP in <root>/<Benutzer>_<uid>/<Titel>_<Name>.zip schreiben (erst daneben, dann umbenannt). Eine vorhandene
 * Sicherung desselben Spielstands bleibt als Vorversion <Titel>_<Name>.<JJJJMMTT-HHMMSS>.zip erhalten; es bleiben die neueste und
 * die zwei davor (BACKUP_KEEP), ältere Vorversionen werden gelöscht. out = Pfad der Datei. 0 = ok, sonst Fehlertext in err */
int  saves_backup_file(const save_info_t *s, const char *root, char *out, size_t outsz, saves_prog_fn prog, void *pctx,
                       char *err, size_t errsz);

/* ZIP-Datei (Ergebnis von saves_download / saves_backup_file, einzeln oder mehrere Spielstände) zurückschreiben. Erst werden
 * alle Spielstände der ZIP geprüft, dann geschrieben. *count = Zahl der geschriebenen. 0 = ok, sonst Fehlertext in err */
int  saves_restore_file(const char *zip_path, int *count, saves_prog_fn prog, void *pctx, char *err, size_t errsz);

/* Zeitstempel (Änderungszeit, Sekunden seit 1970) von Container und - bei PS4 - Schlüsseldatei eines Spielstands setzen.
 * Die PS5 führt zu jedem Spielstand in ihrer Datenbank die Änderungszeit mit; sie muss zur Datei passen. mtime_key < 0:
 * Schlüsseldatei nicht ändern. 0 = ok, sonst Fehlertext in err */
int  saves_set_times(const char *key, int64_t mtime, int64_t mtime_key, char *err, size_t errsz);

/* Sicherungen unter <root>/<Ordner>/<Datei>.zip auflisten (nur gültige: metadata.json vorhanden) */
typedef struct {
    char     path[600];
    char     uid[16], username[64], title_id[32], title_name[256], save_dir[256], platform[8];
    uint64_t size;
    time_t   mtime;         /* Zeit der Sicherungsdatei */
    int64_t  save_mtime;    /* Änderungszeit des Spielstands zum Zeitpunkt der Sicherung (0 = unbekannt, ältere Sicherungen) */
} save_backup_t;
int  saves_list_backups(const char *root, save_backup_t **out, int *count);

/* Arbeitsordner für Zwischendateien (PS5: /data/usb_format/save_tmp) */
const char *saves_workdir(void);

#endif
