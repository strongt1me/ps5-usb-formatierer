/*
 * fsck.exfat (exfatprogs 1.4.3, GPL-2.0-or-later) im Formatierer: Die unveränderten Quellen werden mit
 * exfat_shim.h übersetzt; hier liegen die Gegenstücke (Geräte-Ein-/Ausgabe, eingesammelte Ausgabe, exit).
 * fsck.exfat läuft nicht als eigener Prozess, weil der Formatierer den Stick exklusiv halten muss - sonst bindet die
 * PS5 ihn ein, bevor die Prüfung beginnt (siehe plat_wait_replug).
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stdint.h>
#include <limits.h>
#ifndef ULONG_WIDTH   /* C23-Makro, fehlt in der FreeBSD-libc der PS5 */
#define ULONG_WIDTH (__SIZEOF_LONG__ * 8)
#endif

#include "exfat_ondisk.h"   /* Reihenfolge wie in fsck.c */
#include "libexfat.h"
#include "exfat_fs.h"
#include "exfat_dir.h"
#include "fsck.h"

#include "exfat_fsck.h"
#include "i18n.h"

int exfatprogs_fsck_main(int argc, char *const argv[]);

/* globaler Zustand aus fsck.c - vor jedem Lauf zurücksetzen */
extern struct exfat_fsck exfat_fsck;
struct exfat_stat { long dir_count, file_count, error_count, fixed_count; };   /* wie in fsck.c */
extern struct exfat_stat exfat_stat;

#define DEV_NAME "/dev/usbfmt"

static int      g_fd = -1, g_dup = -1;
static uint64_t g_base, g_size;
static uint32_t g_ss;
static jmp_buf  g_jmp;
static int      g_exit_code;
static char    *g_log;
static size_t   g_loglen, g_logcap;

/* ------------------------------------------------------------ Ausgabe einsammeln (nur das Ende behalten) */

static void log_add(const char *s, size_t n) {
    if (!g_log || !g_logcap) return;
    if (n >= g_logcap) { s += n - (g_logcap - 1); n = g_logcap - 1; }
    if (g_loglen + n >= g_logcap) {
        size_t drop = g_loglen + n - (g_logcap - 1);
        memmove(g_log, g_log + drop, g_loglen - drop);
        g_loglen -= drop;
    }
    memcpy(g_log + g_loglen, s, n);
    g_loglen += n;
    g_log[g_loglen] = 0;
}

int usbfmt_vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) log_add(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    return n;
}

int usbfmt_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = usbfmt_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int usbfmt_fprintf(FILE *f, const char *fmt, ...) {
    (void)f;
    va_list ap;
    va_start(ap, fmt);
    int n = usbfmt_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

void usbfmt_exit(int code) {
    g_exit_code = code;
    longjmp(g_jmp, 1);
}

/* ------------------------------------------------------------ Geräte-Ein-/Ausgabe ab Partitionsbeginn */

static int full_io(int wr, void *buf, size_t n, uint64_t off) {
    for (size_t done = 0; done < n; ) {
        ssize_t k = wr ? pwrite(g_fd, (const uint8_t *)buf + done, n - done, (off_t)(off + done))
                       : pread(g_fd, (uint8_t *)buf + done, n - done, (off_t)(off + done));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        done += (size_t)k;
    }
    return 0;
}

/* Rohgeräte unter FreeBSD verlangen ganze Sektoren: Ränder über einen Zwischenpuffer (lesen, ändern, schreiben) */
static int dev_io(int wr, void *buf, size_t n, uint64_t off) {
    uint64_t abs = g_base + off;
    if (abs % g_ss == 0 && n % g_ss == 0) return full_io(wr, buf, n, abs);
    uint64_t s = abs - abs % g_ss, e = (abs + n + g_ss - 1) / g_ss * g_ss;
    void *b = NULL;
    if (posix_memalign(&b, 4096, (size_t)(e - s))) { errno = ENOMEM; return -1; }
    int rc = full_io(0, b, (size_t)(e - s), s);
    if (rc == 0 && wr) {
        memcpy((uint8_t *)b + (abs - s), buf, n);
        rc = full_io(1, b, (size_t)(e - s), s);
    } else if (rc == 0) {
        memcpy(buf, (uint8_t *)b + (abs - s), n);
    }
    free(b);
    return rc;
}

ssize_t usbfmt_pread(int fd, void *buf, size_t n, off_t off) {
    if (fd != g_dup || g_dup < 0) return pread(fd, buf, n, off);
    if (off < 0 || (uint64_t)off >= g_size) return 0;
    if (n > g_size - (uint64_t)off) n = (size_t)(g_size - (uint64_t)off);
    return dev_io(0, buf, n, (uint64_t)off) ? -1 : (ssize_t)n;
}

ssize_t usbfmt_pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (fd != g_dup || g_dup < 0) return pwrite(fd, buf, n, off);
    if (off < 0 || (uint64_t)off + n > g_size) { errno = ENOSPC; return -1; }
    return dev_io(1, (void *)buf, n, (uint64_t)off) ? -1 : (ssize_t)n;
}

int usbfmt_fsync(int fd) {
    if (fd != g_dup || g_dup < 0) return fsync(fd);
    if (fsync(g_fd) != 0 && errno != EINVAL && errno != ENOTSUP) return -1;   /* Rohgerät: nichts gepuffert */
    return 0;
}

/* Ersetzt die Linux-Fassung aus libexfat.c (sysfs, ioctl): Größe und Sektorgröße kennt der Formatierer */
int exfat_get_blk_dev_info(struct exfat_user_input *ui, struct exfat_blk_dev *bd) {
    g_dup = dup(g_fd);
    if (g_dup < 0) return -1;
    if (!ui->cluster_size) ui->cluster_size = 4096;
    if (!ui->boundary_align) ui->boundary_align = DEFAULT_BOUNDARY_ALIGNMENT;
    bd->dev_fd = g_dup;
    bd->offset = 0;
    bd->size = g_size;
    bd->dev_sector_size = g_ss;
    bd->sector_size = ui->sector_size ? ui->sector_size : g_ss;
    bd->sector_size_bits = 0;
    while ((1u << bd->sector_size_bits) < bd->sector_size) bd->sector_size_bits++;
    bd->num_sectors = g_size / bd->sector_size;
    bd->num_clusters = (unsigned int)(g_size / ui->cluster_size);
    return 0;
}

/* ------------------------------------------------------------ Aufruf */

static long num_after(const char *s, const char *key) {
    const char *p = s ? strstr(s, key) : NULL;
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

int exfat_fsck_run(int fd, uint64_t start, uint64_t size, uint32_t ss, int repair, fsck_result_t *r) {
    char *argv[] = {"fsck.exfat", repair ? "-y" : "-n", DEV_NAME, NULL};
    memset(r, 0, sizeof(*r));
    r->dirs = r->files = r->corrupted = r->fixed = -1;
    g_fd = fd;
    g_dup = -1;
    g_base = start;
    g_size = size;
    g_ss = ss ? ss : 512;
    g_log = r->log;
    g_logcap = sizeof(r->log);
    g_loglen = 0;
    memset(&exfat_fsck, 0, sizeof(exfat_fsck));
    memset(&exfat_stat, 0, sizeof(exfat_stat));
#if defined(__linux__)
    optind = 0;                 /* glibc: getopt neu beginnen */
#else
    optreset = 1;
    optind = 1;
#endif
    if (setjmp(g_jmp) == 0) r->code = exfatprogs_fsck_main(3, argv);
    else r->code = g_exit_code;
    if (g_dup >= 0 && fcntl(g_dup, F_GETFD) != -1) close(g_dup);   /* bei exit() nicht geschlossen */
    g_dup = -1;
    g_fd = -1;
    g_log = NULL;

    /* Zusammenfassung von fsck.exfat: "<gerät>: clean. directories 3, files 7" und ggf.
     * "<gerät>: files corrupted 1, files fixed 0" */
    const char *sum = strstr(r->log, "directories ");
    r->dirs = num_after(sum, "directories ");
    r->files = num_after(sum, ", files ");
    r->corrupted = num_after(sum, "files corrupted ");
    r->fixed = num_after(sum, "files fixed ");
    return 0;
}

void exfat_fsck_summary(const fsck_result_t *r, int repair, char *out, size_t n) {
    char counts[64] = "";
    if (r->dirs >= 0 && r->files >= 0) snprintf(counts, sizeof(counts), T(" (%ld Ordner, %ld Dateien)"), r->dirs, r->files);
    if (r->code == 0)
        snprintf(out, n, T("Keine Fehler gefunden%s."), counts);
    else if (r->code == 1)
        snprintf(out, n, T("Fehler gefunden und behoben%s."), counts);
    else if (r->code & 4)
        snprintf(out, n, repair ? T("Es sind Fehler übrig geblieben, die sich nicht beheben ließen%s. "
                                    "Wichtige Daten sichern und neu formatieren.")
                                : T("Fehler gefunden%s. Mit „Reparieren“ beheben."), counts);
    else
        snprintf(out, n, "%s", T("Prüfen war nicht möglich (exFAT-Dateisystem nicht lesbar oder beschädigt)."));
}
