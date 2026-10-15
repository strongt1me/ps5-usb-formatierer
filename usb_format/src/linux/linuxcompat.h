/* Wird beim Linux-Helfer vor jede Quelldatei gesetzt (-include): arc4random gibt es in glibc erst ab 2.36 - der
 * Helfer soll aber auch auf älteren Systemen gebaut werden (und als statisches Programm überall laufen). */
#ifndef LINUXCOMPAT_H
#define LINUXCOMPAT_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/random.h>

#if defined(__GLIBC__) && !__GLIBC_PREREQ(2, 36)
static inline void usbfmt_random_buf(void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) abort();   /* ohne Zufall keine eindeutigen Seriennummern/GUIDs - nie still weitermachen */
        p += n;
        len -= (size_t)n;
    }
}
static inline uint32_t usbfmt_random(void) {
    uint32_t v;
    usbfmt_random_buf(&v, sizeof(v));
    return v;
}
#define arc4random()         usbfmt_random()
#define arc4random_buf(b, n) usbfmt_random_buf(b, n)
#endif

#endif
