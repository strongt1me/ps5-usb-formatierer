/* param.sfo eines Spielstands lesen und an Ort und Stelle ändern (wie im PS5 Cooling Center, savesfo.h).
 *
 * Aufbau (little endian): Kopf 20 Byte - "\0PSF", Version, Schlüsseltabelle u32 @8, Datentabelle u32 @12, Einträge u32 @16;
 * je Eintrag 16 Byte - Schlüssel-Versatz u16, Format u16, benutzt u32 (+4), Platz u32 (+8), Daten-Versatz u32 (+12).
 * Werte werden nie in der Größe verändert, nur überschrieben.
 *
 * Besitzer eines Spielstands: ACCOUNT_ID (8 Byte) und die Benutzer-ID - im Schlüssel USER_ID, sonst im zweiten Wort von
 * PARAMS (1024 Byte: +4..+8 Benutzer-ID, +8..+40 ein Wert, der in allen Spielständen dieses Benutzers gleich ist). */
#ifndef SAVESFO_H
#define SAVESFO_H

#include <stddef.h>
#include <stdint.h>

/* 1, wenn buf ein in sich stimmiges param.sfo ist */
int savesfo_valid(const uint8_t *buf, size_t len);
/* Lage des Werts zu key: 0 und off/used/cap (jedes darf NULL sein), sonst -1 */
int savesfo_find(const uint8_t *buf, size_t len, const char *key, size_t *off, size_t *used, size_t *cap);
/* Text (abgeschnitten auf out_cap-1): Länge, oder -1 */
int savesfo_get_str(const uint8_t *buf, size_t len, const char *key, char *out, size_t out_cap);
/* Rohbytes (höchstens out_cap): Anzahl, oder -1 */
int savesfo_get_raw(const uint8_t *buf, size_t len, const char *key, uint8_t *out, size_t out_cap);
/* u32 (der Wert muss mindestens 4 Byte haben): 0 oder -1 */
int savesfo_get_u32(const uint8_t *buf, size_t len, const char *key, uint32_t *out);
/* n Byte ab off in den Wert key schreiben (muss in den benutzten Bereich passen): 0 oder -1 */
int savesfo_set_raw(uint8_t *buf, size_t len, const char *key, size_t off, const void *data, size_t n);
/* ACCOUNT_ID und (has_uid) die Benutzer-ID setzen. 0; -1 ohne passende ACCOUNT_ID, -2 ohne Platz für die Benutzer-ID */
int savesfo_patch_owner(uint8_t *buf, size_t len, uint64_t account_id, int has_uid, uint32_t uid);

#endif
