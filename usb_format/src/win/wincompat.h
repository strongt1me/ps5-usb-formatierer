/* Wird beim Windows-Helfer vor jede Quelldatei gesetzt (-include): bildet die POSIX-Aufrufe des gemeinsamen
 * Codes (mkfs, devtest, vollabel) auf Windows ab. Bewusst ohne windows.h, damit dessen Makros den Code nicht
 * stören - die Umsetzung steht in wincompat.c. */
#ifndef WINCOMPAT_H
#define WINCOMPAT_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) == 8, "off_t muss 64 Bit haben (_FILE_OFFSET_BITS=64)");

ssize_t usbfmt_pread(int fd, void *buf, size_t len, uint64_t off);
ssize_t usbfmt_pwrite(int fd, const void *buf, size_t len, uint64_t off);
int usbfmt_fsync(int fd);
int usbfmt_memalign(void **p, size_t align, size_t len);
uint32_t usbfmt_random(void);
void usbfmt_random_buf(void *buf, size_t len);
int usbfmt_clock_gettime(int clk, struct timespec *ts);
const char *usbfmt_strerror(int e);

#define pread(fd, b, n, o)        usbfmt_pread(fd, b, n, (uint64_t)(o))
#define pwrite(fd, b, n, o)       usbfmt_pwrite(fd, b, n, (uint64_t)(o))
#define fsync(fd)                 usbfmt_fsync(fd)
#define posix_memalign(p, a, n)   usbfmt_memalign((void **)(p), a, n)
#define arc4random()              usbfmt_random()
#define arc4random_buf(b, n)      usbfmt_random_buf(b, n)
#define clock_gettime(c, ts)      usbfmt_clock_gettime(c, ts)
#define strerror(e)               usbfmt_strerror(e)   /* Windows-Fehlertext statt "Input/output error" */
#define localtime_r(t, tm)        (localtime_s(tm, t) ? NULL : (tm))
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef ENOTSUP
#define ENOTSUP 129
#endif

#endif
