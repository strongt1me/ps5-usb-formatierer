/* PS4/PS5-Spielstände sichern und einspielen - nach dem Verfahren des PS5 Cooling Center (savexport/saveimport).
 *
 * Sichern: das Abbild wird nach /data kopiert, die Kopie über die Konsole eingehängt und ihre Dateien im Klartext gelesen;
 *          die ZIP-Datei enthält diese Dateien (sce_sys/param.sfo das echte aus dem Kopf des Abbilds) und bei PS5 die
 *          Beschreibung chives.json - dasselbe Format wie das Cooling Center und Garlic SaveMgr.
 * Einspielen: ein NEUES Abbild wird von der Konsole angelegt (PS4: mit frischem Schlüssel in <Name>.bin), eingehängt und
 *          gefüllt; im param.sfo werden Konto, Benutzer und der Besitzerwert aus PARAMS auf den Zielbenutzer gesetzt (der
 *          Besitzerwert aus einem anderen Spielstand desselben Benutzers); geprüft durch zweites Einhängen; dann ersetzt es das
 *          bisherige Abbild, mit Bild und Zeile in der Spielstand-Datenbank der Konsole (vorher gesichert, bei Fehler zurück). */
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
    char     key[320];      /* "<uid>/<title_id>/<save_dir>" (PS4: "#PS4" dahinter) - eindeutige Kennung */
    char     path[512];     /* vollständiger Pfad zum Abbild */
    char     title_id[32];  /* z. B. PPSA01234 */
    char     save_dir[256]; /* Dateiname des Abbilds im Titel-Ordner (sdimg_<Name>) */
    char     title_name[256];
    char     uid[16];
    char     username[64];
    char     platform[8];   /* "PS5" (savedata_prospero) oder "PS4" (savedata: sdimg_<Name> + <Name>.bin) */
    char     key_path[512]; /* PS4: die Schlüsseldatei <Name>.bin (96 Byte), sonst leer */
    uint64_t size;
    time_t   mtime;
} save_info_t;

/* Namen dürfen nur Buchstaben, Ziffern, "_", "-" und "." enthalten, nicht mit "." beginnen und höchstens max-1 Zeichen haben */
int  saves_name_ok(const char *s, size_t max);

int  saves_scan_users(save_user_t **out, int *count);
int  saves_scan(save_info_t **out, int *count);
int  saves_find(const char *key, save_info_t *out);

/* Fortschritt: done von total; Rückgabe != 0: abbrechen (geht nur bis zum Ersetzen auf der Konsole) */
typedef int (*saves_prog_fn)(void *ctx, uint64_t done, uint64_t total);

/* ZIP (chunked HTTP) mit den Spielständen an den Socket streamen. Ein Spielstand: seine Dateien im Wurzelordner, mehrere: je
 * Spielstand der Ordner "<uid>_<Titel>_<Name>/". 0 = gesendet; -1 = Fehler vor dem ersten Byte (err, Aufrufer antwortet);
 * -2 = Fehler mitten im Strom (Verbindung schließen). */
int  saves_download(int sock, const save_info_t *list, int n, char *err, size_t errsz);

/* Spielstand als ZIP nach <root>/<Benutzer>_<uid>/<Titel>_<Name>.zip (erst daneben, dann umbenannt; die zwei vorigen bleiben
 * als <Titel>_<Name>.<JJJJMMTT-HHMMSS>.zip). Fortschritt: done von s->size. 0 = ok, sonst Fehlertext in err */
int  saves_backup_file(const save_info_t *s, const char *root, char *out, size_t outsz, char *rotated, size_t rotsz, saves_prog_fn prog,
                       void *pctx, char *err, size_t errsz);
/* eine eben geschriebene Sicherung zurücknehmen (wie das Cooling Center bei einem gescheiterten Lauf): path löschen, die dabei
 * zur Vorversion gewordene Sicherung rotated (aus saves_backup_file, "" = keine) wieder unter ihren Namen */
/* Hinweis der zuletzt geschriebenen Sicherung ("" = keiner); gilt bis zur nächsten */
const char *saves_last_note(void);
int  saves_backup_revert(const char *path, const char *rotated);   /* -1: ließ sich nicht löschen */

/* Läuft ein Spiel? (der Server meldet es: id = Titel). Das Einspielen fragt direkt vor dem Ersetzen noch einmal, nach der
 * Sicherung des Stands davor - wie das Cooling Center */
void saves_set_game_check(int (*fn)(char *id, size_t n));

/* Apps (YouTube, Browser, Systemanwendungen) bleiben unberührt: nicht gelistet, nicht gesichert, nie eingespielt (wie das
 * Cooling Center: param.json applicationCategoryType 0 bzw. param.sfo CATEGORY "g..." = Spiel, NPXS = App, unbekannt = Spiel) */
int  saves_title_is_app(const char *title_id);

/* Alle Spielstände einer ZIP-Datei (dieser App, des Cooling Center oder von Garlic SaveMgr) für den Benutzer uid einspielen.
 * uid NULL/leer: fallback_uid, sonst der einzige Benutzer der Konsole. Vorher wird der jetzige Stand jedes betroffenen Spiels
 * unter undo_root gesichert (<Titel>_<Name>.vorher-<Zeit>.zip; einmal je Spiel und Auftrag: undo_done merkt sich die Spiele,
 * NULL = keine solche Sicherung); scheitert das, wird nichts eingespielt. *count = eingespielte; note = Hinweise trotz Erfolg.
 * Fortschritt: done von total (Promille). 0 = ok, sonst Fehlertext in err */
int  saves_restore_file(const char *zip_path, const char *uid, const char *fallback_uid, const char *undo_root, char *undo_done,
                        size_t undo_donesz, int *count, saves_prog_fn prog, void *pctx, char *note, size_t notesz, char *err, size_t errsz);

/* Benutzer, für den eingespielt wird: uid, sonst fallback_uid (wenn es ihn gibt), sonst der einzige Benutzer der PS5. 0 = out gefüllt */
int  saves_target_uid(const char *uid, const char *fallback_uid, char *out, size_t n, char *err, size_t errsz);

/* gewählte Spielstände (Schlüssel) um alle Spielstände derselben Spiele erweitern (ein Spiel legt mal SaveData, mal
 * sce_sdmemory, mal beides an). Zahl in out, -1 = ein Schlüssel unbekannt, -2 = mehr als max (nie still abgeschnitten) */
int  saves_expand(const save_info_t *all, int na, char *const *keys, int nk, const save_info_t **out, int max);

/* die Spielstände einer ZIP-Datei als "<Titel>/<Name>#<Konsole>" (höchstens max): ihre Zahl, -1 = keine lesbare Spielstand-ZIP */
int  saves_zip_slots(const char *zip, char (*out)[160], int max);

/* Ein Tausch, den ein abgebrochener Lauf nicht beendet hat (Strom, Absturz), steht in <data>/save_import.journal: beim Start und vor
 * jedem Einspielen zu Ende bringen oder zurücknehmen. 0 = nichts offen, 1 = erledigt (report sagt was), -1 = noch nicht möglich (etwa die
 * Datenbank gesperrt; die Aufzeichnung bleibt, report sagt warum) */
int  saves_import_recover(char *report, size_t report_len);
/* Aufräumen beim Start und zu Beginn jedes Spielstand-Auftrags: je Benutzer bleiben die zehn neuesten Kopien der Spielstand-Datenbank */
void saves_housekeeping(void);

/* Benutzer aus dem Ordner einer Sicherung (<Benutzer>_<uid>/<Datei>.zip): 0 und uid, sonst -1 */
int  saves_backup_uid(const char *path, char *uid, size_t n);

typedef struct {
    char     path[600];
    char     uid[16], username[64], title_id[32], title_name[256], save_dir[256], platform[8];
    uint64_t size;
    time_t   mtime;         /* Zeit der Sicherungsdatei */
    int64_t  save_mtime;    /* jüngste Dateizeit im Spielstand (0 = unbekannt) */
} save_backup_t;
/* Sicherungen unter <root>/<Ordner>/<Datei>.zip auflisten (nur solche mit einem Spielstand im Wurzelordner) */
int  saves_list_backups(const char *root, save_backup_t **out, int *count);

/* Arbeitsordner für Zwischendateien (PS5: /data/usb_format/save_tmp) */
const char *saves_workdir(void);

#endif
