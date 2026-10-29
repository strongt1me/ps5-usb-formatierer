/*
 * Nur lesender Zugriff auf exFAT und UFS2 (siehe fsread.h).
 * Referenzen: Microsoft exFAT File System Specification; FreeBSD sys/ufs/ffs/fs.h und sys/ufs/ufs/dinode.h, dir.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsread.h"

#define BLK        4096
#define NO_BLOCK   UINT64_MAX
#define T_EXFAT    1
#define T_UFS2     2
#define UFS2_MAGIC 0x19540119u
#define UFS_ROOTINO 2
#define UFS_NDADDR 12

struct fsr {
    int      fd, type;
    uint64_t base;
    uint8_t *blk;           /* 4-KiB-Puffer, ausgerichtet */
    uint64_t blk_off;       /* absolute Byte-Position des Pufferinhalts */
    /* exFAT */
    uint32_t cs, clus_count, root_clus;
    uint64_t fat_off, heap_off;  /* relativ zu base */
    /* UFS2 */
    uint32_t bsize, fsize, ipg, inopb, iblkno, fragshift;
    int64_t  fpg;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* len Bytes ab off (relativ zu base) lesen; hinter dem Ende einer Abbild-Datei kommen Nullen */
static int rd(fsr_t *f, uint64_t off, void *buf, size_t len) {
    uint8_t *out = buf;
    while (len) {
        uint64_t abs = f->base + off, start = abs & ~(uint64_t)(BLK - 1);
        if (start != f->blk_off) {
            ssize_t n = pread(f->fd, f->blk, BLK, (off_t)start);
            if (n <= 0) return -1;
            if (n < BLK) memset(f->blk + n, 0, BLK - (size_t)n);
            f->blk_off = start;
        }
        size_t in = (size_t)(abs - start), take = BLK - in < len ? BLK - in : len;
        memcpy(out, f->blk + in, take);
        out += take; off += take; len -= take;
    }
    return 0;
}

void fsr_utf16_to_utf8(const uint8_t *u16, int n, char *out, size_t outlen) {
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = le16(u16 + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t lo = le16(u16 + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;   /* einzelnes Surrogat: kein gültiges Unicode-Zeichen */
        char t[4];
        int k = 0;
        if (c < 0x80) t[k++] = (char)c;
        else if (c < 0x800) { t[k++] = (char)(0xC0 | c >> 6); t[k++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { t[k++] = (char)(0xE0 | c >> 12); t[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
                                t[k++] = (char)(0x80 | (c & 0x3F)); }
        else { t[k++] = (char)(0xF0 | c >> 18); t[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
               t[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); t[k++] = (char)(0x80 | (c & 0x3F)); }
        if (o + (size_t)k + 1 > outlen) break;
        memcpy(out + o, t, (size_t)k);
        o += (size_t)k;
    }
    if (outlen) out[o] = 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* ------------------------------------------------------------ exFAT */

typedef struct {
    uint32_t clus;     /* erster Cluster */
    uint64_t len;      /* Bytes, NO_BLOCK = bis zum Ende der FAT-Kette */
    int      nofat;    /* zusammenhängend, ohne FAT-Kette */
    int      dir;
} exnode_t;

typedef struct {
    exnode_t node;
    uint32_t clus, pos, steps;
    uint64_t done;
} dirit_t;

static int ex_valid(const fsr_t *f, uint32_t c) { return c >= 2 && c <= f->clus_count + 1; }

static uint64_t ex_clus_off(const fsr_t *f, uint32_t c) { return f->heap_off + (uint64_t)(c - 2) * f->cs; }

static int ex_next_clus(fsr_t *f, uint32_t c, int nofat, uint32_t *next) {
    if (nofat) { *next = c + 1; return ex_valid(f, *next) ? 0 : -1; }
    uint8_t v[4];
    if (rd(f, f->fat_off + (uint64_t)c * 4, v, 4)) return -1;
    *next = le32(v);
    return ex_valid(f, *next) ? 0 : -1;    /* 0xFFFFFFFF = Ende der Kette */
}

static void dir_begin(dirit_t *it, const exnode_t *n) {
    memset(it, 0, sizeof(*it));
    it->node = *n;
    it->clus = n->clus;
}

/* nächster Verzeichniseintrag (32 Bytes) und seine Position; 1 = Eintrag, 0 = Ende */
static int dir_next(fsr_t *f, dirit_t *it, uint8_t e[32], uint64_t *pos) {
    if (!ex_valid(f, it->clus)) return 0;
    if (it->node.len != NO_BLOCK && it->done >= it->node.len) return 0;
    /* exFAT erlaubt höchstens 256 MiB je Verzeichnis - schützt vor Endlos-Ketten auf kaputten Sticks */
    if (it->done >= 256ull * 1024 * 1024) return 0;
    if (it->pos >= f->cs) {
        uint32_t prev = it->clus;
        if (++it->steps > f->clus_count || ex_next_clus(f, it->clus, it->node.nofat, &it->clus)) return 0;
        if (it->clus == prev) return 0;   /* Cluster zeigt auf sich selbst: kaputte FAT */
        it->pos = 0;
    }
    uint64_t off = ex_clus_off(f, it->clus) + it->pos;
    if (rd(f, off, e, 32)) return 0;
    if (pos) *pos = off;
    it->pos += 32;
    it->done += 32;
    return 1;
}

/* Eintrag name (ASCII, nlen Zeichen) im Ordner dir suchen */
static int ex_find(fsr_t *f, const exnode_t *dir, const char *name, size_t nlen, exnode_t *out) {
    dirit_t it;
    uint8_t e[32];
    dir_begin(&it, dir);
    while (dir_next(f, &it, e, NULL)) {
        if (e[0] == 0x00) return 0;
        if (e[0] != 0x85) continue;
        int secondary = e[1], attr = le16(e + 4), namelen = 0, got = 0, match = 1;
        exnode_t n = {0};
        for (int s = 0; s < secondary; s++) {
            if (!dir_next(f, &it, e, NULL)) return 0;
            if (e[0] == 0xC0) {
                n.nofat = (e[1] & 2) != 0;
                namelen = e[3];
                n.clus = le32(e + 20);
                n.len = le64(e + 24);
                if ((size_t)namelen != nlen) match = 0;
            } else if (e[0] == 0xC1) {
                for (int k = 0; k < 15 && got < namelen; k++, got++) {
                    uint16_t c = le16(e + 2 + 2 * k);
                    if ((size_t)got >= nlen || c >= 0x80 || lower(c) != lower((unsigned char)name[got])) match = 0;
                }
            }
        }
        if (match && namelen && got == namelen) {
            n.dir = (attr & 0x10) != 0;
            *out = n;
            return 1;
        }
    }
    return 0;
}

static int ex_lookup(fsr_t *f, const char *path, exnode_t *out) {
    exnode_t cur = { f->root_clus, NO_BLOCK, 0, 1 };
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        size_t n = strcspn(path, "/");
        if (!cur.dir || !ex_find(f, &cur, path, n, &cur)) return 0;
        path += n;
    }
    *out = cur;
    return 1;
}

static long ex_read(fsr_t *f, const exnode_t *n, void *buf, size_t max) {
    uint64_t want = n->len < max ? n->len : max, done = 0;
    uint32_t c = n->clus, steps = 0;
    if (!want) return 0;
    if (!ex_valid(f, c)) return -1;
    while (done < want) {
        uint64_t take = want - done < f->cs ? want - done : f->cs;
        if (rd(f, ex_clus_off(f, c), (uint8_t *)buf + done, (size_t)take)) return -1;
        done += take;
        if (done < want && (++steps > f->clus_count || ex_next_clus(f, c, n->nofat, &c))) return -1;
    }
    return (long)done;
}

static int ex_open(fsr_t *f, const uint8_t *bs) {
    uint32_t ss_shift = bs[108], spc_shift = bs[109];
    if (memcmp(bs + 3, "EXFAT   ", 8) != 0 || ss_shift < 9 || ss_shift > 12 || ss_shift + spc_shift > 25) return -1;
    uint32_t ss = 1u << ss_shift;
    f->cs = ss << spc_shift;
    f->fat_off = (uint64_t)le32(bs + 80) * ss;
    f->heap_off = (uint64_t)le32(bs + 88) * ss;
    f->clus_count = le32(bs + 92);
    f->root_clus = le32(bs + 96);
    if (!ex_valid(f, f->root_clus)) return -1;
    f->type = T_EXFAT;
    return 0;
}

int fsr_exfat_label_slot(fsr_t *f, uint64_t *off, int *found) {
    if (f->type != T_EXFAT) return -1;
    exnode_t root = { f->root_clus, NO_BLOCK, 0, 1 };
    dirit_t it;
    uint8_t e[32];
    uint64_t pos, free_pos = NO_BLOCK;
    dir_begin(&it, &root);
    while (dir_next(f, &it, e, &pos)) {
        if (e[0] == 0x83) { *off = f->base + pos; *found = 1; return 0; }
        if (free_pos == NO_BLOCK && (e[0] == 0x00 || !(e[0] & 0x80))) free_pos = pos;
        if (e[0] == 0x00) break;
    }
    if (free_pos == NO_BLOCK) return -1;
    *off = f->base + free_pos;
    *found = 0;
    return 0;
}

/* ------------------------------------------------------------ UFS2 */

typedef struct {
    int      dir, reg;
    uint64_t size;
    int64_t  db[UFS_NDADDR], ib[3];   /* direkte Blöcke, einfach/zweifach/dreifach indirekt (in Fragmenten) */
} ufsino_t;

static int ufs_inode(fsr_t *f, uint32_t ino, ufsino_t *out) {
    uint8_t d[256];
    uint64_t cg = ino / f->ipg;
    int64_t fsba = f->fpg * (int64_t)cg + f->iblkno + (((int64_t)(ino % f->ipg) / f->inopb) << f->fragshift);
    if (fsba < 0 || rd(f, (uint64_t)fsba * f->fsize + (uint64_t)(ino % f->inopb) * 256, d, sizeof(d))) return -1;
    out->dir = (le16(d) & 0xF000) == 0x4000;
    out->reg = (le16(d) & 0xF000) == 0x8000;
    out->size = le64(d + 16);
    for (int i = 0; i < UFS_NDADDR; i++) out->db[i] = (int64_t)le64(d + 112 + 8 * i);
    for (int i = 0; i < 3; i++) out->ib[i] = (int64_t)le64(d + 208 + 8 * i);
    return 0;
}

/* Inhalt (nur direkte Blöcke - reicht für Ordner und param.json) nach buf, höchstens max */
static long ufs_read(fsr_t *f, const ufsino_t *n, void *buf, size_t max) {
    uint64_t want = n->size < max ? n->size : max, done = 0;
    for (int b = 0; done < want; b++) {
        if (b >= UFS_NDADDR) return -1;
        uint64_t take = want - done < f->bsize ? want - done : f->bsize;
        if (n->db[b] <= 0) memset((uint8_t *)buf + done, 0, (size_t)take);
        else if (rd(f, (uint64_t)n->db[b] * f->fsize, (uint8_t *)buf + done, (size_t)take)) return -1;
        done += take;
    }
    return (long)done;
}

static int ufs_find(fsr_t *f, const ufsino_t *dir, const char *name, size_t nlen, ufsino_t *out) {
    size_t max = dir->size < 1024 * 1024 ? (size_t)dir->size : 1024 * 1024;
    uint8_t *buf = malloc(max ? max : 1);
    long len = buf ? ufs_read(f, dir, buf, max) : -1;
    int found = 0;
    for (long p = 0; len > 0 && p + 8 <= len; ) {
        uint32_t ino = le32(buf + p);
        uint16_t reclen = le16(buf + p + 4);
        uint8_t namlen = buf[p + 7];
        if (reclen < 8 || p + reclen > len) break;
        if (ino && namlen == nlen && 8 + (long)namlen <= reclen) {
            size_t k = 0;
            while (k < nlen && lower(buf[p + 8 + k]) == lower((unsigned char)name[k])) k++;
            if (k == nlen) { found = ufs_inode(f, ino, out) == 0; break; }
        }
        p += reclen;
    }
    free(buf);
    return found;
}

static int ufs_lookup(fsr_t *f, const char *path, ufsino_t *out) {
    ufsino_t cur;
    if (ufs_inode(f, UFS_ROOTINO, &cur) || !cur.dir) return 0;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        size_t n = strcspn(path, "/");
        if (!cur.dir || !ufs_find(f, &cur, path, n, &cur)) return 0;
        path += n;
    }
    *out = cur;
    return 1;
}

static int ufs_open(fsr_t *f) {
    /* Superblock wie FreeBSD (SBLOCKSEARCH) suchen: meist bei 64 KiB, bei kleinen Blöcken (makefs) auch bei 8 KiB */
    static const uint64_t SBOFF[] = {65536, 8192, 262144};
    uint8_t sb[2048];
    int found = 0;
    for (int i = 0; i < 3 && !found; i++) found = rd(f, SBOFF[i], sb, sizeof(sb)) == 0 && le32(sb + 0x55c) == UFS2_MAGIC;
    if (!found) return -1;
    f->iblkno = le32(sb + 0x10);
    f->bsize = le32(sb + 0x30);
    f->fsize = le32(sb + 0x34);
    f->fragshift = le32(sb + 0x60);
    f->inopb = le32(sb + 0x78);
    f->ipg = le32(sb + 0xb8);
    f->fpg = (int32_t)le32(sb + 0xbc);
    if (f->bsize < 4096 || f->bsize > 65536 || (f->bsize & (f->bsize - 1)) || !f->fsize || f->fsize > f->bsize ||
        f->fragshift > 3 || !f->ipg || f->inopb != f->bsize / 256 || f->fpg <= 0)
        return -1;
    f->type = T_UFS2;
    return 0;
}

/* ------------------------------------------------------------ gemeinsam */

fsr_t *fsr_open(int fd, uint64_t base) {
    fsr_t *f = calloc(1, sizeof(*f));
    uint8_t bs[512];
    if (!f || posix_memalign((void **)&f->blk, BLK, BLK)) { free(f); return NULL; }
    f->fd = fd;
    f->base = base;
    f->blk_off = NO_BLOCK;
    if (rd(f, 0, bs, sizeof(bs)) == 0 && ex_open(f, bs) == 0) return f;
    if (ufs_open(f) == 0) return f;
    fsr_close(f);
    return NULL;
}

void fsr_close(fsr_t *f) {
    if (!f) return;
    free(f->blk);
    free(f);
}

const char *fsr_type(const fsr_t *f) { return f->type == T_EXFAT ? "exFAT" : "UFS2"; }

int fsr_is_dir(fsr_t *f, const char *path) {
    if (f->type == T_EXFAT) {
        exnode_t n;
        return ex_lookup(f, path, &n) && n.dir;
    }
    ufsino_t n;
    return ufs_lookup(f, path, &n) && n.dir;
}

/* ------------------------------------------------------------ Dateien der Reihe nach lesen, Ordner durchgehen
 * Für Abbild-Dateien: die Daten kommen ohne den 4-KiB-Puffer direkt per pread (schneller; Rohgeräte verlangten
 * ausgerichtete Zugriffe). Der Puffer bleibt so für Verzeichnisse und die Zeiger-Blöcke von UFS2 frei. */

struct fsr_file {
    fsr_t   *f;
    uint64_t size, pos;
    exnode_t ex;       /* exFAT */
    uint64_t valid;    /* exFAT: ValidDataLength - dahinter liest ein Betriebssystem Nullen */
    uint32_t clus;     /* exFAT: Cluster an pos */
    uint32_t steps;    /* exFAT: weitergeschaltete Cluster (Schutz vor Schleifen in der FAT) */
    ufsino_t ufs;      /* UFS2 */
};

#define WALK_DEPTH    32
#define WALK_DIRS     100000    /* höchstens so viele Ordner bzw. ... */
#define WALK_ENTRIES  1000000   /* ... Einträge je Durchgang: kaputte Abbilder können riesig wirken */
#define UFS_DIR_MAX   (2u * 1024 * 1024)   /* je Ordner im Speicher (33 Ebenen: höchstens 66 MiB) */
#define REL_MAX       1024

/* Zustand eines Durchgangs: Zähler, Abbruch und die Ordner auf dem Weg dorthin (Schleifen erkennen) */
typedef struct {
    fsr_walk_cb cb;
    void *ctx;
    const volatile int *cancel;
    long dirs, entries;
    uint64_t path[WALK_DEPTH + 2];   /* erster Cluster (exFAT) bzw. Inode (UFS2) jeder Ebene */
} walk_t;

/* 0 = weiter; sonst der Rückgabewert des Durchgangs */
static int walk_step(walk_t *w, int dir) {
    if (w->cancel && *w->cancel) return -2;
    if (++w->entries > WALK_ENTRIES || (dir && ++w->dirs > WALK_DIRS)) return -1;
    return 0;
}

/* id liegt schon auf dem Weg (Ordner zeigt auf sich selbst oder einen Vorfahren)? */
static int walk_loop(const walk_t *w, int depth, uint64_t id) {
    for (int i = 0; i < depth; i++)
        if (w->path[i] == id) return 1;
    return 0;
}

static int rd_direct(fsr_t *f, uint64_t off, void *buf, size_t len) {
    uint8_t *out = buf;
    while (len) {
        ssize_t n = pread(f->fd, out, len, (off_t)(f->base + off));
        if (n <= 0) return -1;
        out += n; off += (uint64_t)n; len -= (size_t)n;
    }
    return 0;
}

static long ex_fread(fsr_file_t *h, uint8_t *buf, size_t len) {
    fsr_t *f = h->f;
    size_t got = 0;
    if (h->size > (uint64_t)f->clus_count * f->cs) return -1;   /* größer als das ganze Dateisystem: kaputt */
    while (got < len && h->pos < h->size) {
        uint64_t in = h->pos % f->cs;
        if (in == 0 && h->pos && (++h->steps > f->clus_count || ex_next_clus(f, h->clus, h->ex.nofat, &h->clus))) return -1;
        if (!ex_valid(f, h->clus)) return -1;
        uint64_t take = f->cs - in;
        if (take > h->size - h->pos) take = h->size - h->pos;
        if (take > len - got) take = len - got;
        if (h->pos >= h->valid) memset(buf + got, 0, (size_t)take);   /* hinter ValidDataLength: Nullen */
        else {
            uint64_t real = h->valid - h->pos < take ? h->valid - h->pos : take;
            if (rd_direct(f, ex_clus_off(f, h->clus) + in, buf + got, (size_t)real)) return -1;
            if (real < take) memset(buf + got + real, 0, (size_t)(take - real));
        }
        got += take;
        h->pos += take;
    }
    return (long)got;
}

static int ufs_ptr(fsr_t *f, int64_t blk, uint64_t idx, int64_t *out) {
    uint8_t v[8];
    if (blk <= 0) { *out = 0; return 0; }   /* Loch in einer dünn belegten Datei */
    if (rd(f, (uint64_t)blk * f->fsize + idx * 8, v, 8)) return -1;
    *out = (int64_t)le64(v);
    return 0;
}

/* Block lbn der Datei -> Adresse in Fragmenten (0 = Loch) */
static int ufs_bmap(fsr_t *f, const ufsino_t *n, uint64_t lbn, int64_t *out) {
    if (lbn < UFS_NDADDR) { *out = n->db[lbn]; return 0; }
    lbn -= UFS_NDADDR;
    const uint64_t per = f->bsize / 8;
    uint64_t span = per;
    for (int lvl = 0; lvl < 3; lvl++, lbn -= span, span *= per) {
        if (lbn >= span) continue;
        int64_t b = n->ib[lvl];
        for (uint64_t div = span / per;; div /= per) {
            if (ufs_ptr(f, b, (lbn / div) % per, &b)) return -1;
            if (div == 1) break;
        }
        *out = b;
        return 0;
    }
    return -1;
}

static long ufs_fread(fsr_file_t *h, uint8_t *buf, size_t len) {
    fsr_t *f = h->f;
    size_t got = 0;
    while (got < len && h->pos < h->size) {
        uint64_t in = h->pos % f->bsize, take = f->bsize - in;
        int64_t b;
        if (take > h->size - h->pos) take = h->size - h->pos;
        if (take > len - got) take = len - got;
        if (ufs_bmap(f, &h->ufs, h->pos / f->bsize, &b)) return -1;
        if (b <= 0) memset(buf + got, 0, (size_t)take);
        else if (rd_direct(f, (uint64_t)b * f->fsize + in, buf + got, (size_t)take)) return -1;
        got += take;
        h->pos += take;
    }
    return (long)got;
}

long fsr_fread(fsr_file_t *h, void *buf, size_t len) {
    return h->f->type == T_EXFAT ? ex_fread(h, buf, len) : ufs_fread(h, buf, len);
}

/* rel (REL_MAX Bytes, bisher rl Zeichen) um name verlängern; 0 = ok */
static int rel_push(char *rel, size_t rl, const char *name) {
    return strchr(name, '\n') || snprintf(rel + rl, REL_MAX - rl, "%s%s", rl ? "/" : "", name) >= (int)(REL_MAX - rl) ? -1 : 0;
}

static int ex_walk(fsr_t *f, const exnode_t *dir, char *rel, int depth, walk_t *w) {
    dirit_t it;
    uint8_t e[32], u16[255 * 2];
    char name[255 * 3 + 1];
    const size_t rl = strlen(rel);
    int rc = 0;
    if (depth > WALK_DEPTH) return -1;
    w->path[depth] = dir->clus;
    dir_begin(&it, dir);
    while (!rc && dir_next(f, &it, e, NULL)) {
        if (e[0] == 0x00) break;
        if (e[0] != 0x85) continue;
        int secondary = e[1], attr = le16(e + 4), namelen = 0, got = 0;
        uint64_t valid = 0;
        exnode_t n = {0};
        for (int s = 0; s < secondary; s++) {
            if (!dir_next(f, &it, e, NULL)) return -1;
            if (e[0] == 0xC0) {
                n.nofat = (e[1] & 2) != 0;
                namelen = e[3];
                valid = le64(e + 8);
                n.clus = le32(e + 20);
                n.len = le64(e + 24);
            } else if (e[0] == 0xC1) {
                for (int k = 0; k < 15 && got < namelen; k++, got++) memcpy(u16 + 2 * got, e + 2 + 2 * k, 2);
            }
        }
        if (!namelen || got != namelen) continue;
        if ((rc = walk_step(w, attr & 0x10)) != 0) break;
        fsr_utf16_to_utf8(u16, namelen, name, sizeof(name));
        if (rel_push(rel, rl, name)) { rc = -1; break; }
        if (attr & 0x10) {
            n.dir = 1;
            rc = walk_loop(w, depth + 1, n.clus) ? -1 : ex_walk(f, &n, rel, depth + 1, w);
        } else {
            fsr_file_t h = { .f = f, .size = n.len, .ex = n, .valid = valid < n.len ? valid : n.len, .clus = n.clus };
            rc = w->cb(w->ctx, rel, &h, n.len);
        }
        rel[rl] = 0;
    }
    return rc;
}

static int ufs_walk(fsr_t *f, const ufsino_t *dir, uint32_t ino, char *rel, int depth, walk_t *w) {
    const size_t rl = strlen(rel);
    if (depth > WALK_DEPTH || dir->size > UFS_DIR_MAX) return -1;
    w->path[depth] = ino;
    uint8_t *buf = malloc(dir->size ? (size_t)dir->size : 1);
    fsr_file_t dh = { .f = f, .size = dir->size, .ufs = *dir };
    long len = buf ? ufs_fread(&dh, buf, (size_t)dir->size) : -1;
    int rc = len < 0 ? -1 : 0;
    for (long p = 0; !rc && p + 8 <= len; ) {
        uint32_t ino = le32(buf + p);
        uint16_t reclen = le16(buf + p + 4);
        uint8_t namlen = buf[p + 7];
        if (reclen < 8 || p + reclen > len) break;
        if (ino && namlen && 8 + (long)namlen <= reclen) {
            char name[256];
            memcpy(name, buf + p + 8, namlen);
            name[namlen] = 0;
            ufsino_t n;
            if (strcmp(name, ".") && strcmp(name, "..")) {
                if (ufs_inode(f, ino, &n) || rel_push(rel, rl, name)) rc = -1;
                else if ((rc = walk_step(w, n.dir)) != 0) {}
                else if (n.dir) rc = walk_loop(w, depth + 1, ino) ? -1 : ufs_walk(f, &n, ino, rel, depth + 1, w);
                else if (n.reg) {
                    fsr_file_t h = { .f = f, .size = n.size, .ufs = n };
                    rc = w->cb(w->ctx, rel, &h, n.size);
                }
                rel[rl] = 0;
            }
        }
        p += reclen;
    }
    free(buf);
    return rc;
}

int fsr_walk(fsr_t *f, const char *dir, fsr_walk_cb cb, void *ctx, const volatile int *cancel) {
    char rel[REL_MAX] = "";
    walk_t w = {cb, ctx, cancel, 0, 0, {0}};
    if (f->type == T_EXFAT) {
        exnode_t n;
        return ex_lookup(f, dir, &n) && n.dir ? ex_walk(f, &n, rel, 0, &w) : -1;
    }
    ufsino_t n;
    /* Inode des Startordners: nur die Wurzel kennt man ohne Suche - tiefer genügt 0 (Schleifen erkennen die Kinder) */
    return ufs_lookup(f, dir, &n) && n.dir ? ufs_walk(f, &n, dir[0] ? 0 : UFS_ROOTINO, rel, 0, &w) : -1;
}

int fsr_label(fsr_t *f, char *out, size_t n) {
    out[0] = 0;
    if (f->type != T_EXFAT) return 0;
    exnode_t root = { f->root_clus, NO_BLOCK, 0, 1 };
    dirit_t it;
    uint8_t e[32];
    dir_begin(&it, &root);
    while (dir_next(f, &it, e, NULL)) {
        if (e[0] == 0x00) break;
        if (e[0] == 0x83) {
            fsr_utf16_to_utf8(e + 2, e[1] > 11 ? 11 : e[1], out, n);
            break;
        }
    }
    return 0;
}

long fsr_read_file(fsr_t *f, const char *path, void *buf, size_t max) {
    if (f->type == T_EXFAT) {
        exnode_t n;
        return ex_lookup(f, path, &n) && !n.dir ? ex_read(f, &n, buf, max) : -1;
    }
    ufsino_t n;
    if (!ufs_lookup(f, path, &n) || n.dir) return -1;
    fsr_file_t h = { .f = f, .size = n.size, .ufs = n };   /* auch über indirekte Blöcke (große Dateien) */
    return ufs_fread(&h, buf, max);
}
