/*
 * Wird beim Übersetzen der unveränderten exfatprogs-Quellen (third_party/exfatprogs) vorangestellt (-include):
 * leitet deren Ein-/Ausgabe auf den Formatierer um (exfat_fsck.c).
 *  - pread/pwrite/fsync auf den Datenträger gehen an den exklusiv gehaltenen Stick, ab Partitionsbeginn und
 *    sektorgenau (FreeBSD-Rohgeräte verlangen das)
 *  - printf/fprintf/vprintf werden eingesammelt statt ausgegeben
 *  - exit() springt in den Formatierer zurück, statt ihn zu beenden
 *  - kein mmap von /dev/zero (_POSIX_MAPPED_FILES aus): Puffer per calloc
 */
#ifndef USBFMT_EXFAT_SHIM_H
#define USBFMT_EXFAT_SHIM_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include <limits.h>

#undef _POSIX_MAPPED_FILES

/* C23-Makro, das glibc schon kennt, die FreeBSD-libc der PS5 noch nicht */
#ifndef ULONG_WIDTH
#define ULONG_WIDTH (__SIZEOF_LONG__ * 8)
#endif

int     usbfmt_printf(const char *fmt, ...);
int     usbfmt_fprintf(FILE *f, const char *fmt, ...);
int     usbfmt_vprintf(const char *fmt, va_list ap);
ssize_t usbfmt_pread(int fd, void *buf, size_t n, off_t off);
ssize_t usbfmt_pwrite(int fd, const void *buf, size_t n, off_t off);
int     usbfmt_fsync(int fd);
__attribute__((noreturn)) void usbfmt_exit(int code);

#define printf  usbfmt_printf
#define fprintf usbfmt_fprintf
#define vprintf usbfmt_vprintf
#define pread   usbfmt_pread
#define pwrite  usbfmt_pwrite
#define fsync   usbfmt_fsync
#define exit    usbfmt_exit

#endif
