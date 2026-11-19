/* Der Eintrag eines Spielstands in der Spielstand-Datenbank der Konsole (übernommen aus dem PS5 Cooling Center, savedb.h).
 *
 * /system_data/<savedata|savedata_prospero>/<uid>/db/user/savedata.db, Tabelle savedata. Ein Abbild im Ordner des Benutzers
 * ist erst mit seiner Zeile dort vollständig. Eine vorhandene Datenbank wird nur angefasst, wenn sie alle Spalten hat; eine
 * neue nur in einem vorhandenen Ordner und mit dem Aufbau, den die Konsole selbst schreibt (Firmware 12.00, user_version 11). */
#ifndef SAVEDB_H
#define SAVEDB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     title_id[16];
    char     dir_name[100];
    char     main_title[256];
    char     sub_title[256];
    char     detail[1024];
    char     mtime[32];          /* "YYYY-MM-DDTHH:MM:SS.00Z" (UTC) */
    uint64_t account_id;
    uint32_t user_id;
    uint32_t user_param;
    uint64_t blocks, free_blocks, size_kib, system_blocks;
} savedb_row_t;

/* Zeile ändern (Titel + Name) oder anlegen. *inserted = 1 bei einer neuen Zeile. 0 oder -1 (err) */
int savedb_upsert(const char *db_path, const savedb_row_t *row, int *inserted, char *err, size_t err_len);
/* Kopie über SQLites eigene Sicherung (ohne Datenbank: nichts zu tun, 0). dest darf nicht vorhanden sein. 0 oder -1 */
int savedb_backup(const char *db_path, const char *dest_path, char *err, size_t err_len);
/* PRAGMA quick_check = "ok": 0, sonst -1 */
int savedb_check(const char *db_path, char *err, size_t err_len);
/* Zahl der Zeilen zu Titel + Name; -1 = nicht lesbar oder mit einem Aufbau, in den nicht geschrieben wird */
int savedb_has_slot(const char *db_path, const char *title_id, const char *dir_name, char *err, size_t err_len);

#endif
