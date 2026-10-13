#ifndef VOLLABEL_H
#define VOLLABEL_H

#include <stddef.h>
#include <stdint.h>

/* Bezeichnung eines exFAT- oder FAT32-Datenträgers ändern, ohne zu formatieren. fd: exklusiv zum Schreiben
 * geöffnet, ss: Sektorgröße, devsize: Bytes. Leere Bezeichnung entfernt sie. 0 = ok, sonst Fehlertext in err. */
int vol_set_label(int fd, uint32_t ss, uint64_t devsize, const char *label, char *err, size_t errlen);

#endif
