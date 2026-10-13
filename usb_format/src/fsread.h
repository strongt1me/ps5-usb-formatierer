#ifndef FSREAD_H
#define FSREAD_H

#include <stddef.h>
#include <stdint.h>

/* Nur lesender Zugriff auf ein Dateisystem in einem Abbild (Spiel als .exfat/.ffpkg) oder auf einem Datenträger:
 * exFAT und UFS2. Das Dateisystem beginnt bei Byte base. Gelesen wird in 4-KiB-Blöcken ab 4-KiB-Grenzen, wie es
 * Rohgeräte unter FreeBSD (PS5) verlangen. */
typedef struct fsr fsr_t;

fsr_t      *fsr_open(int fd, uint64_t base);   /* NULL: kein exFAT/UFS2 oder Lesefehler */
void        fsr_close(fsr_t *f);
const char *fsr_type(const fsr_t *f);          /* "exFAT" oder "UFS2" */

/* Datei unter path lesen (Teile durch '/', Groß-/Kleinschreibung egal), höchstens max Bytes.
 * Liefert die gelesene Länge oder -1, wenn es sie nicht gibt. */
long fsr_read_file(fsr_t *f, const char *path, void *buf, size_t max);

/* Gibt es path als Ordner? 1 ja, 0 nein */
int fsr_is_dir(fsr_t *f, const char *path);

/* exFAT: Bezeichnung als UTF-8 (leer, wenn keine); UFS2: immer leer. 0 = ok */
int fsr_label(fsr_t *f, char *out, size_t n);

/* Für Tests und die Umbenennung: Byte-Position des Bezeichnungs-Eintrags im exFAT-Wurzelordner (Typ 0x83),
 * sonst die des ersten freien Eintrags; 0 = ok */
int fsr_exfat_label_slot(fsr_t *f, uint64_t *off, int *found);

/* UTF-16LE (n Zeichen) -> UTF-8 */
void fsr_utf16_to_utf8(const uint8_t *u16, int n, char *out, size_t outlen);

#endif
