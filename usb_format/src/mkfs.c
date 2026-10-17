/*
 * MBR/GPT-, FAT32- und exFAT-Formatierung auf ein Block-Device oder eine Image-Datei.
 * Referenzen: Microsoft FAT32 File System Specification (fatgen103),
 *             Microsoft exFAT File System Specification,
 *             UEFI Specification 2.10, Kapitel 5 (GUID Partition Table).
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "mkfs.h"
#include "mkfs_int.h"
#include "upcase.h"

#define MIB (1024u * 1024u)
/* 65525 ist die harte Grenze; etwas Abstand, weil manche Treiber knapp daneben zaehlen */
#define FAT32_MIN_CLUSTERS 65536u
#define FAT32_MAX_CLUSTERS 0x0FFFFFF4u
#define EXFAT_MAX_CLUSTERS 0xFFFFFFF5u

static char errbuf[256];
static char sumbuf[256];
static uint8_t *zbuf;

const char *mkfs_error(void) { return errbuf; }
const char *mkfs_summary(void) { return sumbuf; }

/* Fehlertext in der Sprache des Threads (fmt ist der deutsche Text, siehe i18n.h) */
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof(errbuf), T(fmt), ap);
    va_end(ap);
    return -1;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static int log2u(uint32_t v) {
    int n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

/* 512 -> "512 Byte", 65536 -> "64 KB", 33554432 -> "32 MB" */
static const char *size_text(uint32_t bytes, char *buf, size_t n) {
    if (bytes >= MIB && bytes % MIB == 0) snprintf(buf, n, "%u %s", bytes / MIB, T("MB"));
    else if (bytes >= 1024 && bytes % 1024 == 0) snprintf(buf, n, "%u %s", bytes / 1024, T("KB"));
    else snprintf(buf, n, "%u %s", bytes, T("Byte"));
    return buf;
}

/* Fortschritt über das ganze Formatieren: Jeder Schritt bekommt einen festen Anteil (phase), innerhalb dessen er
 * 0-100 % meldet. So steigt die Anzeige stetig, statt je Schritt bei 0 zu beginnen und lange still zu stehen
 * (langsame Sticks brauchen Sekunden je verstreutem Schreibzugriff). Nie rückwärts. */
static int ph_lo = 0, ph_hi = 100, ph_last = -1;

static void phase(int lo, int hi) { ph_lo = lo; ph_hi = hi; }

static void report(blkdev_t *d, const char *step, int pct) {
    int all = ph_lo + pct * (ph_hi - ph_lo) / 100;
    if (all < ph_last) all = ph_last;
    ph_last = all;
    if (d->progress) d->progress(T(step), all);   /* Schrittname in der Sprache des Threads */
}

static int dev_write(blkdev_t *d, uint64_t sec, const void *buf, uint64_t nsec) {
    uint64_t off = d->base + sec * d->ss;
    uint64_t len = nsec * d->ss;
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = pwrite(d->fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0)
            return fail("Schreibfehler bei Byte %llu: %s", (unsigned long long)off,
                        n < 0 ? strerror(errno) : "nichts geschrieben");
        p += n; off += n; len -= n;
    }
    return 0;
}

static int dev_zero(blkdev_t *d, uint64_t sec, uint64_t nsec, const char *step) {
    uint64_t chunk = MIB / d->ss, done = 0;
    int report_it = step != NULL;   /* je MiB: auch kleine Bereiche, langsame Sticks brauchen dafür Sekunden */
    if (report_it) report(d, step, 0);

    if (!zbuf) {
        if (posix_memalign((void **)&zbuf, 4096, MIB)) return fail("Kein Speicher");
        memset(zbuf, 0, MIB);
    }
    while (done < nsec) {
        uint64_t n = nsec - done < chunk ? nsec - done : chunk;
        if (dev_write(d, sec + done, zbuf, n)) return -1;
        done += n;
        if (report_it) report(d, step, (int)(done * 100 / nsec));
    }
    return 0;
}

static uint8_t *sec_alloc(const blkdev_t *d, uint64_t nsec) {
    void *p;
    if (posix_memalign(&p, 4096, nsec * d->ss)) {
        fail("Kein Speicher");
        return NULL;
    }
    memset(p, 0, nsec * d->ss);
    return p;
}

/* Höchstlänge der Bezeichnung: FAT32/exFAT 11, NTFS 32, ext4 16 Zeichen */
int label_max(int fs) {
    return fs == FS_NTFS || fs == FS_HFSPLUS ? 32 : fs == FS_EXT4 ? 16 : 11;
}

int label_valid(const char *l, int fs) {
    size_t n = strlen(l);
    if (n > (size_t)label_max(fs)) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = l[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static void dos_time(uint16_t *t, uint16_t *dt) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 80) { *t = 0; *dt = (1 << 5) | 1; return; }
    *t = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    *dt = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

/* ------------------------------------------------------------------ Ordner/Dateien gleich beim Formatieren */

/* Die Ordner-Vorlage (z. B. homebrew, etaHEN/games) entsteht direkt im neuen Dateisystem - ohne darauf zu warten,
 * dass die PS5 den Stick wieder einbindet. Jeder Ordner belegt einen Cluster, Dateien zusammenhängende Cluster. */
#define POP_MAX 32

typedef struct {
    char           name[64];
    int            parent;     /* Index, -1 = Wurzel */
    int            dir;
    const uint8_t *data;
    uint32_t       len;
    uint32_t       clus, nclus;
} popnode_t;

static int pop_add(popnode_t *n, int *cnt, int parent, const char *name, size_t len, int dir) {
    if (!len || len >= sizeof(n->name) || (len <= 2 && name[0] == '.' && (len == 1 || name[1] == '.')))
        return fail("Ungültiger Ordner- oder Dateiname: %.*s", (int)len, name);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7F || strchr("\"*:<>?\\|", c))
            return fail("Ungültiges Zeichen im Namen %.*s", (int)len, name);
    }
    for (int i = 0; i < *cnt; i++)
        if (n[i].parent == parent && strlen(n[i].name) == len && strncasecmp(n[i].name, name, len) == 0) {
            if (!dir || !n[i].dir) return fail("Name doppelt vergeben: %.*s", (int)len, name);
            return i;
        }
    if (*cnt >= POP_MAX) return fail("Zu viele Ordner und Dateien");
    popnode_t *p = &n[*cnt];
    memset(p, 0, sizeof(*p));
    memcpy(p->name, name, len);
    p->parent = parent;
    p->dir = dir;
    return (*cnt)++;
}

/* Pfad zerlegen; Ordner auf dem Weg werden ergänzt. Liefert den Index des letzten Teils oder -1. */
static int pop_path(popnode_t *n, int *cnt, const char *path, int last_dir) {
    int parent = -1;
    while (*path) {
        while (*path == '/') path++;
        size_t len = strcspn(path, "/");
        if (!len) break;
        int last = path[len] == 0 || path[len + strspn(path + len, "/")] == 0;
        parent = pop_add(n, cnt, parent, path, len, last ? last_dir : 1);
        if (parent < 0) return -1;
        path += len;
    }
    return parent;
}

static int pop_build(const mkfs_opts_t *o, popnode_t *n, int *cnt) {
    *cnt = 0;
    if (!o) return 0;
    for (int k = 0; o->dirs && o->dirs[k]; k++)
        if (pop_path(n, cnt, o->dirs[k], 1) < 0) return -1;
    for (int k = 0; k < o->nfiles; k++) {
        int i = pop_path(n, cnt, o->files[k].path, 0);
        if (i < 0) return -1;
        n[i].data = o->files[k].data;
        n[i].len = o->files[k].len;
    }
    return 0;
}

/* ------------------------------------------------------------------ FAT32 */

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* Kurzname 8.3 für einen Ordner: Großbuchstaben, ungültige Zeichen als '_', gekürzt bzw. bei Kollision mit "~n" */
static void fat_short_name(const char *name, int tail, uint8_t out[11]) {
    char base[9];
    size_t k = 0;
    memset(out, ' ', 11);
    for (const char *p = name; *p && k < 8; p++) {
        char c = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') base[k++] = c;
        else if (c != ' ' && c != '.') base[k++] = '_';
    }
    if (!k) base[k++] = '_';
    base[k] = 0;
    if (tail) {
        char t[8];
        int tl = snprintf(t, sizeof(t), "~%d", tail);
        if (k + (size_t)tl > 8) k = 8 - (size_t)tl;
        memcpy(base + k, t, (size_t)tl);
        k += (size_t)tl;
    }
    memcpy(out, base, k);
}

static uint8_t fat_lfn_sum(const uint8_t sn[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + sn[i]);
    return s;
}

/* Ordner-Eintrag mit langem Namen (LFN) und Kurzname an buf + *pos */
static int fat_dir_entry(uint8_t *buf, size_t cap, size_t *pos, const char *name, const uint8_t sn[11], uint32_t clus) {
    static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    size_t L = strlen(name);
    int nl = (int)((L + 12) / 13);
    uint16_t t, dt;
    if (*pos + 32u * (size_t)(nl + 1) > cap) return fail("Zu viele Einträge in einem Ordner");
    dos_time(&t, &dt);
    uint8_t sum = fat_lfn_sum(sn);
    for (int i = nl; i >= 1; i--) {
        uint8_t *e = buf + *pos;
        memset(e, 0, 32);
        e[0] = (uint8_t)(i | (i == nl ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (int k = 0; k < 13; k++) {
            size_t ci = (size_t)(i - 1) * 13 + (size_t)k;
            put16(e + offs[k], ci < L ? (uint8_t)name[ci] : ci == L ? 0x0000 : 0xFFFF);
        }
        *pos += 32;
    }
    uint8_t *e = buf + *pos;
    memset(e, 0, 32);
    memcpy(e, sn, 11);
    e[11] = 0x10;
    put16(e + 14, t); put16(e + 16, dt); put16(e + 18, dt);
    put16(e + 20, (uint16_t)(clus >> 16));
    put16(e + 22, t); put16(e + 24, dt);
    put16(e + 26, (uint16_t)clus);
    *pos += 32;
    return 0;
}

/* Kinder von parent in einen FAT32-Ordner schreiben (Kurznamen je Ordner eindeutig) */
static int fat_children(uint8_t *buf, size_t cap, size_t *pos, const popnode_t *pn, int npn, int parent) {
    uint8_t used[POP_MAX][11];
    int nused = 0;
    for (int i = 0; i < npn; i++) {
        if (pn[i].parent != parent) continue;
        uint8_t sn[11];
        for (int tail = 0;; tail++) {
            fat_short_name(pn[i].name, tail, sn);
            int clash = 0;
            for (int k = 0; k < nused; k++) clash |= memcmp(used[k], sn, 11) == 0;
            if (!clash) break;
        }
        memcpy(used[nused++], sn, 11);
        if (fat_dir_entry(buf, cap, pos, pn[i].name, sn, pn[i].clus)) return -1;
    }
    return 0;
}

int mkfs_fat32(blkdev_t *d, uint64_t hidden, uint64_t nsect, const char *label, uint32_t serial, uint32_t cluster,
               const mkfs_opts_t *o) {
    const uint32_t ss = d->ss, align = MIB / ss;
    uint32_t csize, spc = 0, rsvd = 0, fatsz = 0;
    uint64_t clusters = 0, data = 0, bytes = nsect * ss;
    uint8_t lab[11];
    uint8_t *bs = NULL, *fsi = NULL, *fat = NULL, *root = NULL, *dirbuf = NULL;
    popnode_t pn[POP_MAX];
    int npn = 0, rc = -1;

    if (pop_build(o, pn, &npn)) return -1;
    for (int i = 0; i < npn; i++)
        if (!pn[i].dir) return fail("Dateien gleich beim Formatieren gibt es nur für exFAT");

    if (nsect > 0xFFFFFFFFull) return fail("Partition zu groß für FAT32 (max. 2^32 Sektoren). Bitte exFAT verwenden.");
    if (hidden > 0xFFFFFFFFull) return fail("Partitionsbeginn außerhalb des FAT32-Bereichs");
    if (nsect < 4ull * align) return fail("Datenträger zu klein für FAT32");
    if (cluster && (!is_pow2(cluster) || cluster < ss || cluster > 65536))
        return fail("Ungültige Zuordnungseinheit für FAT32 (%u Bytes)", cluster);

    /* Clustergroesse wie Windows: <=8 GB 4K, <=16 GB 8K, <=32 GB 16K, darüber 32K */
    csize = cluster ? cluster
                    : bytes <= (8ull << 30) ? 4096 : bytes <= (16ull << 30) ? 8192 : bytes <= (32ull << 30) ? 16384 : 32768;
    if (csize < ss) csize = ss;

    for (;;) {
        spc = csize / ss;
        rsvd = 32;
        /* FAT für die Obergrenze an Clustern dimensionieren; Ausrichtungs-Padding senkt die Zahl nur */
        uint64_t est = (nsect - rsvd) / spc;
        fatsz = (uint32_t)(((est + 2) * 4 + ss - 1) / ss);
        data = rsvd + 2ull * fatsz;
        uint32_t pad = (uint32_t)((align - data % align) % align);
        rsvd += pad;
        data += pad;
        clusters = data < nsect ? (nsect - data) / spc : 0;
        if (cluster || clusters >= FAT32_MIN_CLUSTERS || csize == ss) break;  /* fest gewählt: nicht verkleinern */
        csize /= 2;
    }
    if (cluster && clusters < FAT32_MIN_CLUSTERS)
        return fail("Zuordnungseinheit %u KB ist für diese Datenträgergröße zu groß (FAT32 braucht mind. 65.525 Cluster). "
                    "Bitte eine kleinere wählen.", csize / 1024);
    if (cluster && clusters > FAT32_MAX_CLUSTERS)
        return fail("Zuordnungseinheit ist für diese Datenträgergröße zu klein (zu viele Cluster für FAT32). "
                    "Bitte eine größere wählen.");
    if (clusters < FAT32_MIN_CLUSTERS) return fail("Datenträger zu klein für FAT32 (min. ca. 33 MB). Bitte exFAT verwenden.");
    if (clusters > FAT32_MAX_CLUSTERS) return fail("Datenträger zu groß für FAT32. Bitte exFAT verwenden.");

    memset(lab, ' ', sizeof(lab));
    if (label && *label) {
        for (size_t i = 0; i < 11 && label[i]; i++)
            lab[i] = (label[i] >= 'a' && label[i] <= 'z') ? label[i] - 32 : label[i];
    } else {
        memcpy(lab, "NO NAME    ", 11);
    }

    /* Root = Cluster 2, angelegte Ordner ab Cluster 3 (je einer) */
    uint32_t next = 3;
    for (int i = 0; i < npn; i++) { pn[i].clus = next++; pn[i].nclus = 1; }
    if (next - 2 > clusters) return fail("Datenträger zu klein für die Ordner");
    const uint64_t fat_secs = (4ull * next + ss - 1) / ss;

    if (!(bs = sec_alloc(d, 1)) || !(fsi = sec_alloc(d, 1)) || !(fat = sec_alloc(d, fat_secs)) ||
        !(root = sec_alloc(d, spc)) || (npn && !(dirbuf = sec_alloc(d, spc))))
        goto out;

    /* Reservierter Bereich, beide FATs, Root-Cluster und die Ordner-Cluster nullen */
    phase(8, 10);
    if (dev_zero(d, 0, rsvd, "Bootbereich vorbereiten")) goto out;
    phase(10, 75);
    if (dev_zero(d, rsvd, 2ull * fatsz, "FAT-Tabellen schreiben")) goto out;
    phase(75, 80);
    if (dev_zero(d, data, (uint64_t)(next - 2) * spc, "Ordner vorbereiten")) goto out;
    phase(80, 85);

    static const uint8_t jmp[3] = {0xEB, 0x58, 0x90};
    memcpy(bs, jmp, 3);
    memcpy(bs + 3, "MSWIN4.1", 8);
    put16(bs + 11, (uint16_t)ss);
    bs[13] = (uint8_t)spc;
    put16(bs + 14, (uint16_t)rsvd);
    bs[16] = 2;                         /* Anzahl FATs */
    bs[21] = 0xF8;                      /* Media: Festplatte */
    put16(bs + 24, 63);                 /* Sektoren pro Spur */
    put16(bs + 26, 255);                /* Koepfe */
    put32(bs + 28, (uint32_t)hidden);
    put32(bs + 32, (uint32_t)nsect);
    put32(bs + 36, fatsz);
    put32(bs + 44, 2);                  /* Root-Cluster */
    put16(bs + 48, 1);                  /* FSInfo-Sektor */
    put16(bs + 50, 6);                  /* Backup-Bootsektor */
    bs[64] = 0x80;
    bs[66] = 0x29;
    put32(bs + 67, serial);
    memcpy(bs + 71, lab, 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55;
    bs[511] = 0xAA;

    put32(fsi, 0x41615252);
    put32(fsi + 484, 0x61417272);
    put32(fsi + 488, (uint32_t)(clusters - (next - 2)));   /* freie Cluster */
    put32(fsi + 492, next);                                 /* nächster freier */
    put32(fsi + 508, 0xAA550000);

    put32(fat, 0x0FFFFFF8);
    put32(fat + 4, 0x0FFFFFFF);
    for (uint32_t c = 2; c < next; c++)
        put32(fat + 4ull * c, 0x0FFFFFFF);   /* Root und jeder Ordner: ein Cluster, Kettenende */

    size_t rpos = 0;
    if (label && *label) {
        uint16_t t, dt;
        dos_time(&t, &dt);
        memcpy(root, lab, 11);
        root[11] = 0x08;                /* Attribut: Volume-ID */
        put16(root + 22, t);
        put16(root + 24, dt);
        rpos = 32;
    }
    if (fat_children(root, csize, &rpos, pn, npn, -1)) goto out;

    /* Ordner: ".", ".." (0 = Root), dann ihre Unterordner */
    for (int i = 0; i < npn; i++) {
        uint16_t t, dt;
        dos_time(&t, &dt);
        memset(dirbuf, 0, csize);
        for (int k = 0; k < 2; k++) {
            uint8_t *e = dirbuf + 32 * k;
            uint32_t c = k == 0 ? pn[i].clus : pn[i].parent < 0 ? 0 : pn[pn[i].parent].clus;
            memcpy(e, k == 0 ? ".          " : "..         ", 11);
            e[11] = 0x10;
            put16(e + 14, t); put16(e + 16, dt); put16(e + 18, dt);
            put16(e + 20, (uint16_t)(c >> 16)); put16(e + 22, t); put16(e + 24, dt); put16(e + 26, (uint16_t)c);
        }
        size_t pos = 64;
        if (fat_children(dirbuf, csize, &pos, pn, npn, i) ||
            dev_write(d, data + (uint64_t)(pn[i].clus - 2) * spc, dirbuf, spc))
            goto out;
        report(d, "Ordner anlegen", (i + 1) * 100 / npn);
    }

    phase(85, 95);
    report(d, "Dateisystem schreiben", 0);
    if (dev_write(d, data, root, spc) ||
        dev_write(d, rsvd, fat, fat_secs) ||
        dev_write(d, rsvd + fatsz, fat, fat_secs) ||
        dev_write(d, 1, fsi, 1) ||
        dev_write(d, 7, fsi, 1) ||
        dev_write(d, 6, bs, 1) ||
        dev_write(d, 0, bs, 1))         /* Bootsektor zuletzt */
        goto out;
    report(d, "Dateisystem schreiben", 100);

    char cs[24];
    snprintf(sumbuf, sizeof(sumbuf), T("FAT32, %s Cluster, %llu Cluster"), size_text(csize, cs, sizeof(cs)),
             (unsigned long long)clusters);
    rc = 0;
out:
    free(bs); free(fsi); free(fat); free(root); free(dirbuf);
    return rc;
}

/* ------------------------------------------------------------------ exFAT */

static uint32_t exfat_boot_checksum(const uint8_t *p, uint64_t len) {
    uint32_t c = 0;
    for (uint64_t i = 0; i < len; i++) {
        if (i == 106 || i == 107 || i == 112) continue;  /* VolumeFlags, PercentInUse */
        c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + p[i];
    }
    return c;
}

static uint32_t exfat_table_checksum(const uint8_t *p, uint64_t len) {
    uint32_t c = 0;
    for (uint64_t i = 0; i < len; i++)
        c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + p[i];
    return c;
}

static void fat_chain(uint8_t *fat, uint32_t first, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        put32(fat + 4ull * (first + i), i + 1 < n ? first + i + 1 : 0xFFFFFFFFu);
}

/* Namens-Hash über den großgeschriebenen UTF-16-Namen (hier nur ASCII) */
static uint16_t exfat_name_hash(const char *name) {
    uint16_t h = 0;
    for (const char *p = name; *p; p++) {
        uint16_t c = (uint8_t)*p;
        if (c >= 'a' && c <= 'z') c -= 32;
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

/* Eintragssatz für einen Ordner oder eine Datei: File (0x85), Stream (0xC0), Namen (0xC1) samt Prüfsumme */
static int exfat_entry_set(uint8_t *buf, size_t cap, size_t *pos, const char *name, int dir, uint32_t first,
                           uint64_t len) {
    size_t L = strlen(name);
    int nn = (int)((L + 14) / 15), total = 2 + nn;
    uint8_t *e = buf + *pos;
    uint16_t t, dt;
    if (*pos + 32u * (size_t)total > cap) return fail("Zu viele Einträge in einem Ordner");
    dos_time(&t, &dt);
    const uint32_t ts = (uint32_t)dt << 16 | t;
    memset(e, 0, 32u * (size_t)total);
    e[0] = 0x85;
    e[1] = (uint8_t)(total - 1);
    put16(e + 4, dir ? 0x10 : 0x20);
    put32(e + 8, ts); put32(e + 12, ts); put32(e + 16, ts);
    uint8_t *s = e + 32;
    s[0] = 0xC0;
    s[1] = 0x01;                        /* AllocationPossible, FAT-Kette */
    s[3] = (uint8_t)L;
    put16(s + 4, exfat_name_hash(name));
    put64(s + 8, len);
    put32(s + 20, first);
    put64(s + 24, len);
    for (int i = 0; i < nn; i++) {
        uint8_t *m = e + 64 + 32 * i;
        m[0] = 0xC1;
        for (int k = 0; k < 15 && (size_t)(i * 15 + k) < L; k++) put16(m + 2 + 2 * k, (uint8_t)name[i * 15 + k]);
    }
    uint16_t sum = 0;
    for (int i = 0; i < 32 * total; i++)
        if (i != 2 && i != 3) sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + e[i]);
    put16(e + 2, sum);
    *pos += 32u * (size_t)total;
    return 0;
}

static int exfat_children(uint8_t *buf, size_t cap, size_t *pos, const popnode_t *pn, int npn, int parent,
                          uint32_t csize) {
    for (int i = 0; i < npn; i++)
        if (pn[i].parent == parent &&
            exfat_entry_set(buf, cap, pos, pn[i].name, pn[i].dir, pn[i].clus, pn[i].dir ? csize : pn[i].len))
            return -1;
    return 0;
}

int mkfs_exfat(blkdev_t *d, uint64_t hidden, uint64_t nsect, const char *label, uint32_t serial, uint32_t cluster,
               const mkfs_opts_t *o) {
    const uint32_t ss = d->ss, align = MIB / ss, csize = cluster ? cluster : EXFAT_CLUSTER_SIZE;
    uint8_t *br = NULL, *fat = NULL, *bm = NULL, *up = NULL, *root = NULL, *dirbuf = NULL;
    popnode_t pn[POP_MAX];
    int npn = 0, rc = -1;

    if (pop_build(o, pn, &npn)) return -1;

    if (ss < 512 || ss > 4096) return fail("Nicht unterstützte Sektorgröße %u", ss);
    /* Spezifikation: Zweierpotenz, mindestens ein Sektor, höchstens 32 MB */
    if (!is_pow2(csize) || csize < ss || csize > 32u * MIB)
        return fail("Ungültige Zuordnungseinheit für exFAT (%u Bytes)", csize);
    const uint32_t spc = csize / ss;
    const uint32_t halign = spc > align ? spc : align;   /* Cluster-Heap auf 1 MiB bzw. Clustergröße ausrichten */
    if (nsect < 8ull * align) return fail("Datenträger zu klein für exFAT");

    const uint32_t fat_off = align;
    const uint64_t est = (nsect - fat_off) / spc;
    const uint64_t fatlen = ((est + 2) * 4 + ss - 1) / ss;
    const uint64_t heap = (fat_off + fatlen + halign - 1) / halign * halign;
    if (heap >= nsect || heap > 0xFFFFFFFFull) return fail("Ungültige exFAT-Geometrie");

    const uint64_t count = (nsect - heap) / spc;
    if (count > EXFAT_MAX_CLUSTERS) return fail("Zu viele Cluster für exFAT – bitte eine größere Zuordnungseinheit wählen");

    const uint64_t bm_bytes = (count + 7) / 8;
    const uint32_t bm_clus = (uint32_t)((bm_bytes + csize - 1) / csize);
    const uint32_t up_clus = (exfat_upcase_table_size + csize - 1) / csize;
    const uint32_t c_bm = 2, c_up = c_bm + bm_clus, c_root = c_up + up_clus;

    /* Angelegte Ordner (je ein Cluster) und Dateien (zusammenhängend) folgen auf den Root-Cluster */
    uint64_t next = c_root + 1;
    for (int i = 0; i < npn; i++) {
        pn[i].nclus = pn[i].dir ? 1 : (uint32_t)(((uint64_t)pn[i].len + csize - 1) / csize);
        pn[i].clus = pn[i].nclus ? (uint32_t)next : 0;
        next += pn[i].nclus;
    }
    if (next - 2 >= count) return fail("Datenträger zu klein für exFAT");
    const uint32_t used = (uint32_t)(next - 2);

#define CLUSTER_SEC(c) (heap + (uint64_t)((c) - 2) * spc)

    const uint64_t fat_bytes = 4ull * next;
    const uint64_t fat_secs = (fat_bytes + ss - 1) / ss;
    const uint64_t bm_head_secs = ((used + 7) / 8 + ss - 1) / ss;
    const uint64_t up_secs = (exfat_upcase_table_size + ss - 1) / ss;
    const uint32_t dirbytes = csize < 65536 ? csize : 65536;   /* beschriebener Teil eines Ordner-Clusters */

    /* Puffer nur so groß wie der Inhalt; der Rest der (ggf. sehr großen) Cluster wird genullt */
    if (!(br = sec_alloc(d, 12)) || !(fat = sec_alloc(d, fat_secs)) || !(bm = sec_alloc(d, bm_head_secs)) ||
        !(up = sec_alloc(d, up_secs)) || !(root = sec_alloc(d, dirbytes / ss)) ||
        (npn && !(dirbuf = sec_alloc(d, dirbytes / ss))))
        goto out;

    /* Bootbereiche, FAT und Luecke bis zum Cluster-Heap nullen, dann Bitmap-, Up-case-, Root- und neue Cluster */
    phase(8, 10);
    if (dev_zero(d, 0, fat_off, "Bootbereich vorbereiten")) goto out;
    phase(10, 45);
    if (dev_zero(d, fat_off, heap - fat_off, "FAT schreiben")) goto out;
    phase(45, 75);
    if (dev_zero(d, CLUSTER_SEC(c_bm), (uint64_t)bm_clus * spc, "Zuordnungstabelle schreiben")) goto out;
    phase(75, 80);
    if (dev_zero(d, CLUSTER_SEC(c_up), (uint64_t)(next - c_up) * spc, "Ordner vorbereiten")) goto out;
    phase(80, 85);

    /* FAT */
    put32(fat, 0xFFFFFFF8u);
    put32(fat + 4, 0xFFFFFFFFu);
    fat_chain(fat, c_bm, bm_clus);
    fat_chain(fat, c_up, up_clus);
    fat_chain(fat, c_root, 1);
    for (int i = 0; i < npn; i++)
        if (pn[i].nclus) fat_chain(fat, pn[i].clus, pn[i].nclus);

    /* Allocation Bitmap: Bit n = Cluster n+2 */
    for (uint32_t i = 0; i < used; i++)
        bm[i / 8] |= (uint8_t)(1u << (i % 8));

    memcpy(up, exfat_upcase_table, exfat_upcase_table_size);
    const uint32_t up_sum = exfat_table_checksum(exfat_upcase_table, exfat_upcase_table_size);

    /* Root-Verzeichnis */
    uint8_t *e = root;
    size_t lablen = label ? strlen(label) : 0;
    if (lablen) {
        e[0] = 0x83;
        e[1] = (uint8_t)lablen;
        for (size_t i = 0; i < lablen; i++) put16(e + 2 + 2 * i, (uint8_t)label[i]);
        e += 32;
    }
    e[0] = 0x81;
    put32(e + 20, c_bm);
    put64(e + 24, bm_bytes);
    e += 32;
    e[0] = 0x82;
    put32(e + 4, up_sum);
    put32(e + 20, c_up);
    put64(e + 24, exfat_upcase_table_size);
    e += 32;
    size_t rpos = (size_t)(e - root);
    if (exfat_children(root, dirbytes, &rpos, pn, npn, -1, csize)) goto out;

    /* Ordner-Cluster und Dateiinhalte */
    for (int i = 0; i < npn; i++) {
        if (pn[i].dir) {
            size_t pos = 0;
            memset(dirbuf, 0, dirbytes);
            if (exfat_children(dirbuf, dirbytes, &pos, pn, npn, i, csize) ||
                dev_write(d, CLUSTER_SEC(pn[i].clus), dirbuf, dirbytes / ss))
                goto out;
        } else if (pn[i].len) {
            uint64_t secs = (pn[i].len + ss - 1) / ss;
            uint8_t *fb = sec_alloc(d, secs);
            if (!fb) goto out;
            memcpy(fb, pn[i].data, pn[i].len);
            int wr = dev_write(d, CLUSTER_SEC(pn[i].clus), fb, secs);
            free(fb);
            if (wr) goto out;
        }
        report(d, "Ordner anlegen", (i + 1) * 100 / npn);
    }

    /* Main Boot Region: Sektor 0 Bootsektor, 1-8 Extended, 9 OEM, 10 reserviert, 11 Checksumme */
    uint8_t *bs = br;
    static const uint8_t jmp[3] = {0xEB, 0x76, 0x90};
    memcpy(bs, jmp, 3);
    memcpy(bs + 3, "EXFAT   ", 8);
    put64(bs + 64, hidden);
    put64(bs + 72, nsect);
    put32(bs + 80, fat_off);
    put32(bs + 84, (uint32_t)fatlen);
    put32(bs + 88, (uint32_t)heap);
    put32(bs + 92, (uint32_t)count);
    put32(bs + 96, c_root);
    put32(bs + 100, serial);
    put16(bs + 104, 0x0100);            /* Revision 1.00 */
    bs[108] = (uint8_t)log2u(ss);
    bs[109] = (uint8_t)log2u(spc);
    bs[110] = 1;                        /* Anzahl FATs */
    bs[111] = 0x80;
    bs[112] = (uint8_t)((uint64_t)used * 100 / count);
    memset(bs + 120, 0xF4, 390);        /* Bootcode: hlt */
    bs[510] = 0x55;
    bs[511] = 0xAA;
    for (int s = 1; s <= 8; s++)
        put32(br + (uint64_t)s * ss + ss - 4, 0xAA550000u);
    uint32_t sum = exfat_boot_checksum(br, 11ull * ss);
    for (uint32_t i = 0; i < ss; i += 4)
        put32(br + 11ull * ss + i, sum);

    phase(85, 95);
    report(d, "Dateisystem schreiben", 0);
    if (dev_write(d, fat_off, fat, fat_secs) ||
        dev_write(d, CLUSTER_SEC(c_bm), bm, bm_head_secs) ||
        dev_write(d, CLUSTER_SEC(c_up), up, up_secs) ||
        dev_write(d, CLUSTER_SEC(c_root), root, dirbytes / ss) ||
        dev_write(d, 12, br, 12) ||
        dev_write(d, 0, br, 12))        /* Haupt-Bootbereich zuletzt */
        goto out;
    report(d, "Dateisystem schreiben", 100);
#undef CLUSTER_SEC

    char cs[24];
    snprintf(sumbuf, sizeof(sumbuf), T("exFAT, %s Cluster, %llu Cluster"), size_text(csize, cs, sizeof(cs)),
             (unsigned long long)count);
    rc = 0;
out:
    free(br); free(fat); free(bm); free(up); free(root); free(dirbuf);
    return rc;
}

/* ------------------------------------------------------------------ MBR */

static void chs(uint8_t *p, uint64_t lba) {
    if (lba >= 1024ull * 255 * 63) { p[0] = 0xFE; p[1] = 0xFF; p[2] = 0xFF; return; }
    uint32_t c = (uint32_t)(lba / (255 * 63)), h = (uint32_t)(lba / 63 % 255), s = (uint32_t)(lba % 63 + 1);
    p[0] = (uint8_t)h;
    p[1] = (uint8_t)(s | ((c >> 2) & 0xC0));
    p[2] = (uint8_t)c;
}

uint32_t mkfs_crc32(const void *p, uint64_t len) {
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    for (uint64_t i = 0; i < len; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* Lage der einen Partition: ab 1 MiB bis zum Ende des genutzten Bereichs (end Sektoren). MBR fasst höchstens
 * 2^32 Sektoren; GPT braucht am Ende Platz für die Sicherungskopie (Kopf + 16 KiB Einträge). */
typedef struct {
    uint64_t start, count, end;
    int      gpt;
} layout_t;

#define GPT_ENTRY_BYTES (128u * 128u)

/* alte Partitionstabellen am Anfang und Ende (Backup-GPT) löschen */
static int begin_layout(blkdev_t *d, int fs, const char *label, mkfs_opts_t *o, layout_t *l) {
    const uint64_t edge = MIB / d->ss;
    if (fs < FS_EXFAT || fs > FS_HFSPLUS) return fail("Unbekanntes Dateisystem");
    if (!label_valid(label, fs)) return fail("Ungültige Bezeichnung (max. %d Zeichen: A-Z, 0-9, Leerzeichen, - _)",
                                             label_max(fs));
    l->end = d->sectors;
    if (o && o->limit && o->limit / d->ss < l->end) l->end = o->limit / d->ss;
    if (l->end < 16 * edge) return fail("Datenträger zu klein");
    const int scheme = o ? o->scheme : LAYOUT_AUTO;
    l->gpt = scheme == LAYOUT_GPT || (scheme == LAYOUT_AUTO && l->end - edge > 0xFFFFFFFFull);
    l->start = edge;
    if (l->gpt) {
        l->count = l->end - 1 - GPT_ENTRY_BYTES / d->ss - edge;   /* bis zum letzten nutzbaren Sektor */
    } else {
        l->count = l->end - edge;
        /* MBR: die Partition muss in den ersten 2^32 Sektoren enden (wie layout_disk) - sonst lehnen manche Systeme
         * die Tabelle ab, und die Oberfläche hat mit dieser Grenze gerechnet */
        if (l->count > 0xFFFFFFFFull - edge) l->count = 0xFFFFFFFFull - edge;
    }
    if (o) o->is_gpt = l->gpt;

    d->base = 0;
    ph_last = -1;   /* neues Formatieren: Fortschritt von vorn */
    phase(0, 4);
    if (dev_zero(d, 0, edge, "Alte Partitionstabellen löschen")) return -1;
    phase(4, 8);
    return dev_zero(d, l->end - edge, edge, "Alte Partitionstabellen löschen");
}

static int sync_dev(blkdev_t *d) {
    if (fsync(d->fd) && errno != EINVAL && errno != ENOTSUP) return fail("fsync: %s", strerror(errno));
    return 0;
}

/* Partitionstyp "Basic Data" (exFAT, FAT32, NTFS) bzw. "Linux filesystem" usw., wie auf dem Datenträger gespeichert */
static const uint8_t GUID_BASIC_DATA[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                            0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
static const uint8_t GUID_LINUX_FS[16]   = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                            0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
static const uint8_t GUID_LINUX_SWAP[16] = {0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,   /* 0657FD6D-A4AB-43C4- */
                                            0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F};
static const uint8_t GUID_EFI_SYSTEM[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,   /* C12A7328-F81F-11D2- */
                                            0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const uint8_t GUID_APPLE_HFS[16]  = {0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11,   /* 48465300-0000-11AA- */
                                            0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC};

static int part_kind(const part_t *p) {
    if (p->ptype != PT_AUTO) return p->ptype;
    return p->fs == FS_EXT4 ? PT_LINUX : p->fs == FS_HFSPLUS ? PT_APPLE : PT_WINDOWS;
}

static uint8_t part_mbr_type(const part_t *p) {
    if (p->mbr_type) return p->mbr_type;
    switch (part_kind(p)) {
    case PT_LINUX: return 0x83;
    case PT_SWAP:  return 0x82;
    case PT_EFI:   return 0xEF;
    case PT_APPLE: return 0xAF;
    default:       return p->fs == FS_FAT32 ? 0x0C : 0x07;   /* FAT32 (LBA), sonst exFAT/NTFS */
    }
}

static const uint8_t *part_gpt_type(const part_t *p) {
    switch (part_kind(p)) {
    case PT_LINUX: return GUID_LINUX_FS;
    case PT_SWAP:  return GUID_LINUX_SWAP;
    case PT_EFI:   return GUID_EFI_SYSTEM;
    case PT_APPLE: return GUID_APPLE_HFS;
    default:       return GUID_BASIC_DATA;
    }
}

static int write_mbr(blkdev_t *d, const part_t *p, int n, uint32_t serial) {
    uint8_t *mbr;
    int rc;

    report(d, "Partitionstabelle schreiben", 0);
    d->base = 0;
    if (!(mbr = sec_alloc(d, 1))) return -1;
    put32(mbr + 440, serial);           /* Datenträger-Signatur */
    for (int i = 0; i < n && i < 4; i++) {
        uint8_t *pe = mbr + 446 + 16 * i;
        pe[0] = p[i].active ? 0x80 : 0x00;
        chs(pe + 1, p[i].start);
        pe[4] = part_mbr_type(&p[i]);
        chs(pe + 5, p[i].start + p[i].count - 1);
        put32(pe + 8, (uint32_t)p[i].start);
        put32(pe + 12, (uint32_t)p[i].count);
    }
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    rc = dev_write(d, 0, mbr, 1);
    free(mbr);
    return rc ? -1 : sync_dev(d);
}

static void random_guid(uint8_t g[16]) {
    arc4random_buf(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* Version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);   /* Variante RFC 4122 */
}

/* Schutz-MBR, GPT-Kopf mit 128 Einträgen und die Sicherungskopie am Ende (end = Ende des genutzten Bereichs) */
static int write_gpt(blkdev_t *d, const part_t *p, int n, uint64_t end) {
    const uint32_t ss = d->ss;
    const uint64_t ent_secs = GPT_ENTRY_BYTES / ss, last = end - 1;
    uint8_t *mbr = NULL, *hdr = NULL, *ents = NULL;
    int rc = -1;

    report(d, "Partitionstabelle (GPT) schreiben", 0);
    d->base = 0;
    if (!(mbr = sec_alloc(d, 1)) || !(hdr = sec_alloc(d, 1)) || !(ents = sec_alloc(d, ent_secs))) goto out;

    for (int i = 0; i < n && i < 128; i++) {
        uint8_t *e = ents + 128 * i;
        memcpy(e, part_gpt_type(&p[i]), 16);
        random_guid(e + 16);
        put64(e + 32, p[i].start);
        put64(e + 40, p[i].start + p[i].count - 1);
        const char *name = p[i].name && *p[i].name ? p[i].name : "Daten";
        for (int k = 0; k < 36 && name[k]; k++) put16(e + 56 + 2 * k, (uint8_t)name[k]);
    }

    memcpy(hdr, "EFI PART", 8);
    put32(hdr + 8, 0x00010000);         /* Revision 1.0 */
    put32(hdr + 12, 92);                /* Kopfgröße */
    put64(hdr + 24, 1);                 /* dieser Kopf */
    put64(hdr + 32, last);              /* Sicherungskopf */
    put64(hdr + 40, 2 + ent_secs);      /* erster nutzbarer Sektor */
    put64(hdr + 48, last - ent_secs - 1);
    random_guid(hdr + 56);
    put64(hdr + 72, 2);                 /* Einträge ab */
    put32(hdr + 80, 128);
    put32(hdr + 84, 128);
    put32(hdr + 88, mkfs_crc32(ents, GPT_ENTRY_BYTES));
    put32(hdr + 16, mkfs_crc32(hdr, 92));

    /* Sicherungskopie: Einträge direkt vor dem letzten Sektor, Kopf im letzten */
    if (dev_write(d, last - ent_secs, ents, ent_secs)) goto out;
    put32(hdr + 16, 0);
    put64(hdr + 24, last);
    put64(hdr + 32, 1);
    put64(hdr + 72, last - ent_secs);
    put32(hdr + 16, mkfs_crc32(hdr, 92));
    if (dev_write(d, last, hdr, 1)) goto out;
    put32(hdr + 16, 0);
    put64(hdr + 24, 1);
    put64(hdr + 32, last);
    put64(hdr + 72, 2);
    put32(hdr + 16, mkfs_crc32(hdr, 92));
    if (dev_write(d, 2, ents, ent_secs) || dev_write(d, 1, hdr, 1)) goto out;

    uint8_t *pe = mbr + 446;            /* Schutz-MBR zuletzt: eine Partition 0xEE über den ganzen Datenträger */
    pe[2] = 0x02;
    pe[4] = 0xEE;
    pe[5] = pe[6] = pe[7] = 0xFF;
    put32(pe + 8, 1);
    put32(pe + 12, last > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)last);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (dev_write(d, 0, mbr, 1)) goto out;
    rc = sync_dev(d);
out:
    free(mbr); free(hdr); free(ents);
    return rc;
}

/* eine Partition nach dem alten Muster (PS5, Standard): Name = Bezeichnung, Typ passend zu fs */
static int write_table(blkdev_t *d, int fs, const layout_t *l, uint32_t serial, const char *label,
                       const mkfs_opts_t *o) {
    part_t p = {0};
    p.fs = fs;
    p.mbr_type = o ? o->mbr_type : 0;
    p.name = label;
    p.start = l->start;
    p.count = l->count;
    return l->gpt ? write_gpt(d, &p, 1, l->end) : write_mbr(d, &p, 1, serial);
}

int format_disk(blkdev_t *d, int fs, const char *label, uint32_t serial, uint32_t cluster, mkfs_opts_t *o) {
    layout_t l;
    int rc;

    if (fs != FS_EXFAT && fs != FS_FAT32 && fs != FS_HFSPLUS) return fail("Dieses Dateisystem schreibt ein externes Werkzeug");
    if (begin_layout(d, fs, label, o, &l)) return -1;

    d->base = l.start * d->ss;
    rc = fs == FS_EXFAT ? mkfs_exfat(d, l.start, l.count, label, serial, cluster, o)
       : fs == FS_FAT32 ? mkfs_fat32(d, l.start, l.count, label, serial, cluster, o)
                        : mkfs_hfsplus(d, l.count, label, serial, cluster);
    d->base = 0;
    phase(95, 100);
    rc = rc || write_table(d, fs, &l, serial, label, o);
    if (!rc) report(d, "Fertig", 100);
    phase(0, 100);   /* für spätere Aufrufe ohne format_disk (Tests) */
    ph_last = -1;
    return rc ? -1 : 0;
}

int prepare_partition(blkdev_t *d, int fs, const char *label, uint32_t serial, mkfs_opts_t *o,
                      uint64_t *start, uint64_t *count) {
    const uint64_t edge = MIB / d->ss;
    layout_t l;

    if (fs != FS_NTFS && fs != FS_EXT4) return fail("Unbekanntes Dateisystem");
    if (begin_layout(d, fs, label, o, &l)) return -1;
    *start = l.start;
    *count = l.count;

    /* Alte Dateisystem-Signaturen am Partitionsanfang und -ende löschen (NTFS legt hinten eine Kopie des
     * Bootsektors ab), damit niemand den Zwischenstand für ein gültiges Dateisystem hält */
    d->base = l.start * d->ss;
    phase(8, 10);
    report(d, "Partition vorbereiten", 0);
    int rc = dev_zero(d, 0, 2 * edge, NULL) || dev_zero(d, l.count - edge, edge, NULL);
    d->base = 0;
    rc = rc || write_table(d, fs, &l, serial, label, o);
    phase(0, 100);   /* den Rest meldet das externe Werkzeug selbst */
    ph_last = -1;
    return rc ? -1 : 0;
}

/* ------------------------------------------------------------------ Mehrere Partitionen (Expertenmodus) */

void mkfs_progress_reset(void) {
    phase(0, 100);
    ph_last = -1;
}

int layout_plan(const blkdev_t *d, int scheme, uint64_t limit, part_t *p, int n, int *is_gpt) {
    const uint64_t edge = MIB / d->ss;
    uint64_t end = d->sectors;
    if (limit && limit / d->ss < end) end = limit / d->ss;
    if (n < 1 || n > MAX_PARTS) return fail("Zwischen 1 und %d Partitionen möglich", MAX_PARTS);
    if (end < 16 * edge) return fail("Datenträger zu klein");
    const int gpt = scheme == LAYOUT_GPT || (scheme == LAYOUT_AUTO && end - edge > 0xFFFFFFFFull);
    if (!gpt && n > 4) return fail("MBR fasst höchstens 4 Partitionen – bitte GPT wählen");
    /* letzter nutzbarer Sektor + 1: GPT braucht hinten Platz für die Sicherungskopie, MBR zählt höchstens 2^32 */
    uint64_t usable = gpt ? end - 1 - GPT_ENTRY_BYTES / d->ss : end;
    if (!gpt && usable > 0xFFFFFFFFull) usable = 0xFFFFFFFFull;

    uint64_t at = edge;
    for (int i = 0; i < n; i++) {
        at = (at + edge - 1) / edge * edge;             /* jede Partition an einer MiB-Grenze */
        if (at + edge > usable) return fail("Partition %d passt nicht mehr auf den Datenträger", i + 1);
        uint64_t count;
        if (p[i].bytes) {
            count = p[i].bytes / d->ss / edge * edge;   /* ganze MiB */
            if (count < edge) return fail("Partition %d ist kleiner als 1 MB", i + 1);
            if (at + count > usable) return fail("Partition %d ist zu groß – es fehlen %llu MB", i + 1,
                                                 (unsigned long long)((at + count - usable) * d->ss / MIB + 1));
        } else {
            if (i != n - 1) return fail("Nur die letzte Partition darf den Rest bekommen");
            count = usable - at;
        }
        p[i].start = at;
        p[i].count = count;
        at += count;
    }
    if (is_gpt) *is_gpt = gpt;
    return 0;
}

int layout_disk(blkdev_t *d, int scheme, uint64_t limit, part_t *p, int n, uint32_t serial, int *is_gpt) {
    int gpt = 0;
    if (layout_plan(d, scheme, limit, p, n, &gpt)) return -1;
    if (is_gpt) *is_gpt = gpt;
    const uint64_t edge = MIB / d->ss;
    uint64_t end = d->sectors;
    if (limit && limit / d->ss < end) end = limit / d->ss;

    /* alte Tabellen vorne und hinten, dann Anfang und Ende jeder Partition (alte Dateisystem-Signaturen) */
    d->base = 0;
    ph_last = -1;
    phase(0, 4);
    if (dev_zero(d, 0, edge, "Alte Partitionstabellen löschen")) return -1;
    phase(4, 8);
    if (dev_zero(d, end - edge, edge, "Alte Partitionstabellen löschen")) return -1;
    for (int i = 0; i < n; i++) {
        phase(8 + 2 * i / n, 8 + 2 * (i + 1) / n);
        uint64_t head = p[i].count < 2 * edge ? p[i].count : edge;
        if (dev_zero(d, p[i].start, head, "Partitionen vorbereiten") ||
            dev_zero(d, p[i].start + p[i].count - head, head, NULL))
            return -1;
    }
    phase(10, 10);
    int rc = gpt ? write_gpt(d, p, n, end) : write_mbr(d, p, n, serial);
    mkfs_progress_reset();
    return rc;
}

/* ------------------------------------------------------------------ für hfsplus.c (mkfs_int.h) */

int mk_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof(errbuf), T(fmt), ap);
    va_end(ap);
    return -1;
}

void mk_summary(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sumbuf, sizeof(sumbuf), T(fmt), ap);
    va_end(ap);
}

void mk_report(blkdev_t *d, const char *step, int pct) { report(d, step, pct); }
int mk_write(blkdev_t *d, uint64_t sec, const void *buf, uint64_t nsec) { return dev_write(d, sec, buf, nsec); }
int mk_zero(blkdev_t *d, uint64_t sec, uint64_t nsec, const char *step) { return dev_zero(d, sec, nsec, step); }
uint8_t *mk_alloc(const blkdev_t *d, uint64_t nsec) { return sec_alloc(d, nsec); }
int mk_sync(blkdev_t *d) { return sync_dev(d); }
const char *mk_size_text(uint32_t bytes, char *buf, size_t n) { return size_text(bytes, buf, n); }