#ifndef GEOM_XML_H
#define GEOM_XML_H

#include <stddef.h>

/* Sucht in kern.geom.confxml (Klasse DISK) das <descr>-Feld von Laufwerk "name" (z.B. "da1").
 * 1 = gefunden (out gefüllt), 0 = nicht vorhanden/leer. */
int geom_disk_descr(const char *xml, const char *name, char *out, size_t n);

/* Wie geom_disk_descr, aber das <ident>-Feld (Seriennummer des Laufwerks) */
int geom_disk_ident(const char *xml, const char *name, char *out, size_t n);

#endif
