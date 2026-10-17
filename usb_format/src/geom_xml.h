#ifndef GEOM_XML_H
#define GEOM_XML_H

#include <stddef.h>

/* Sucht in kern.geom.confxml (Klasse DISK) das <descr>-Feld von Laufwerk "name" (z.B. "da1").
 * 1 = gefunden (out gefüllt), 0 = nicht vorhanden/leer. */
int geom_disk_descr(const char *xml, const char *name, char *out, size_t n);

/* Wie geom_disk_descr, aber das <ident>-Feld (Seriennummer des Laufwerks), mit ident_clean bereinigt */
int geom_disk_ident(const char *xml, const char *name, char *out, size_t n);

/* Seriennummer auf druckbares ASCII bringen (sonst "_"): kommt sie über JSON und den Browser zurück, ist sie dieselbe
 * Zeichenkette - ungültiges UTF-8 käme sonst als U+FFFD zurück und passte nie mehr */
void ident_clean(char *s);

#endif
