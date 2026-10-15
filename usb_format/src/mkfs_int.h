#ifndef MKFS_INT_H
#define MKFS_INT_H

/* Innereien von mkfs.c für weitere Formatierer (hfsplus.c) - nicht für Aufrufer von außen */

#include "mkfs.h"

int      mk_fail(const char *fmt, ...);                 /* Fehlertext für mkfs_error(), liefert -1 */
void     mk_summary(const char *fmt, ...);              /* Zusammenfassung für mkfs_summary() */
void     mk_report(blkdev_t *d, const char *step, int pct);
int      mk_write(blkdev_t *d, uint64_t sec, const void *buf, uint64_t nsec);   /* ab d->base */
int      mk_zero(blkdev_t *d, uint64_t sec, uint64_t nsec, const char *step);
uint8_t *mk_alloc(const blkdev_t *d, uint64_t nsec);    /* genullt, mit free() freigeben */
int      mk_sync(blkdev_t *d);
const char *mk_size_text(uint32_t bytes, char *buf, size_t n);

#endif
