/*
 * Image-Sicherung und -Wiederherstellung (.img.zst), siehe imgbak.h.
 *
 * Aufbau der Datei:
 *   [überspringbarer Rahmen 0x184D2A5A] "USBFMT-IMAGE 2\n" {JSON mit Angaben}\n <Belegungskarte: 1 Bit je 64-KiB-Block>
 *   das Image der Reihe nach in Abschnitten, je einer von:
 *     [zstd-Rahmen] Daten (Inhaltsgröße im Rahmenkopf, XXH64-Prüfsumme) - belegte Blöcke und kurze Lücken
 *     [überspringbarer Rahmen 0x184D2A5C] "USBFMT-ZERO\0" <Länge u64> <Rahmengröße u64>, danach
 *     [zstd-Rahmen] aus lauter RLE-Blöcken "128 KiB Nullen" (4 Byte je Block) - freie Strecke ab 32 MiB
 *   [überspringbarer Rahmen 0x184D2A5B] "USBFMT-SHA256 <hex>\n" - SHA-256 der belegten Blöcke (laut Karte, der Reihe nach)
 *
 * Lange freie Strecken laufen so nicht als Nullen durch den Packer (bei 2 TB fast leer sonst eine Viertelstunde), und das
 * Wiederherstellen überspringt sie anhand des Hinweises, ohne Nullen zu erzeugen. zstd entpackt die Rahmen einfach
 * nacheinander; Fassung 1 (2.5.0: ein einziger Datenrahmen) wird weiter gelesen.
 *
 * Die Prüfsumme deckt nur die belegten Blöcke ab: über das ganze Image (bei 1 TB fast nur Nullen) dauerte sie Stunden.
 * Den ganzen Inhalt sichert die XXH64-Prüfsumme des zstd-Rahmens, die zstd beim Entpacken selbst kontrolliert.
 */
#include "imgbak.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "i18n.h"
#include "sha256.h"

#define PAGE       (1024u * 1024u)    /* Arbeitsstück: 16 Blöcke */
#define MAGIC_HEAD 0x184D2A5Au
#define MAGIC_TAIL 0x184D2A5Bu
#define MAGIC_ZERO 0x184D2A5Cu
#define ZERO_MIN   (32ull * 1024 * 1024)   /* freie Strecken ab dieser Länge als Null-Rahmen */
#define RLE_MAX    (128u * 1024u)           /* größter zstd-Block */
#define HEAD_MAX   (64u * 1024u * 1024u)   /* Kopf höchstens 64 MiB (Karte für 32 TiB) */
static const char HEAD_TAG[] = "USBFMT-IMAGE 2\n";
static const char HEAD_TAG1[] = "USBFMT-IMAGE 1\n";   /* 2.5.0: alles in einem Datenrahmen */
static const char ZERO_TAG[] = "USBFMT-ZERO";            /* mit Nullbyte: 12 Bytes */
static const char TAIL_TAG[] = "USBFMT-SHA256 ";

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static void *aligned(size_t n) {
    void *p = NULL;
    return posix_memalign(&p, 4096, n) ? NULL : p;
}

static int rd_all(int fd, void *buf, size_t len, uint64_t off) {
    for (size_t done = 0; done < len; ) {
        ssize_t r = pread(fd, (uint8_t *)buf + done, len - done, (off_t)(off + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int wr_all(int fd, const void *buf, size_t len, uint64_t off) {
    for (size_t done = 0; done < len; ) {
        ssize_t r = pwrite(fd, (const uint8_t *)buf + done, len - done, (off_t)(off + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

/* Beliebige Bytes über einen an Sektoren ausgerichteten Lesevorgang (Rohgeräte verlangen das) */
static int read_bytes(int fd, uint32_t ss, uint64_t off, void *out, size_t len) {
    uint64_t a = off / ss * ss, e = (off + len + ss - 1) / ss * ss;
    uint8_t *t = aligned((size_t)(e - a));
    if (!t) return -1;
    int rc = rd_all(fd, t, (size_t)(e - a), a);
    if (!rc) memcpy(out, t + (off - a), len);
    free(t);
    return rc;
}

/* ------------------------------------------------------------------ Belegungskarte (Bit gesetzt = belegt) */

typedef struct {
    uint8_t *bits;
    uint64_t n;   /* Blöcke */
} map_t;

static int map_used(const map_t *m, uint64_t b) { return (m->bits[b >> 3] >> (b & 7)) & 1; }

/* Blöcke, die ganz in [start, start + len) liegen, als frei markieren */
static void map_free(map_t *m, uint64_t start, uint64_t len) {
    uint64_t first = (start + IB_BLOCK - 1) / IB_BLOCK, last = (start + len) / IB_BLOCK;
    for (uint64_t b = first; b < last && b < m->n; b++) m->bits[b >> 3] &= (uint8_t)~(1u << (b & 7));
}

/* Lauf freier Cluster, der gerade endet, eintragen */
typedef struct {
    map_t   *m;
    uint64_t heap, csize;   /* Byte-Offset des ersten Clusters (Index 0), Clustergröße */
    uint64_t run_start, run_len;
} runs_t;

static void run_add(runs_t *r, uint64_t idx, int is_free) {
    if (is_free) {
        if (!r->run_len) r->run_start = idx;
        r->run_len++;
        return;
    }
    if (r->run_len) map_free(r->m, r->heap + r->run_start * r->csize, r->run_len * r->csize);
    r->run_len = 0;
}

/* exFAT: Zuordnungs-Bitmap (Eintrag 0x81 im Wurzelordner), Bit 0 = Cluster frei */
static void scan_exfat(int fd, uint32_t ss, uint64_t base, uint64_t plen, const uint8_t *bs, map_t *m) {
    unsigned bsh = bs[108], csh = bs[109];
    if (bsh < 9 || bsh > 12 || csh > 25 - bsh) return;
    const uint64_t bps = 1ull << bsh, csize = bps << csh;
    const uint64_t fat = (uint64_t)le32(bs + 80) * bps, heap = (uint64_t)le32(bs + 88) * bps;
    const uint32_t cc = le32(bs + 92), root = le32(bs + 96);
    if (!cc || heap + (uint64_t)cc * csize > plen || fat + 4ull * (cc + 2) > heap) return;
    const size_t chunk = csize < IB_BLOCK ? (size_t)csize : IB_BLOCK;
    uint8_t *buf = malloc(chunk);
    if (!buf) return;

    uint32_t bm_first = 0, c = root;
    uint64_t bm_len = 0;
    int end = 0;
    for (int k = 0; k < 256 && !end && !bm_first && c >= 2 && c < cc + 2; k++) {   /* Wurzelordner */
        for (uint64_t o = 0; o < csize && !end && !bm_first; o += chunk) {
            if (read_bytes(fd, ss, base + heap + (uint64_t)(c - 2) * csize + o, buf, chunk)) goto out;
            for (size_t e = 0; e + 32 <= chunk; e += 32) {
                if (buf[e] == 0x00) { end = 1; break; }
                if (buf[e] == 0x81) { bm_first = le32(buf + e + 20); bm_len = le64(buf + e + 24); break; }
            }
        }
        uint8_t nx[4];
        if (read_bytes(fd, ss, base + fat + 4ull * c, nx, 4)) goto out;
        c = le32(nx);
    }
    if (bm_first < 2 || bm_first >= cc + 2 || bm_len < (cc + 7) / 8) goto out;

    runs_t r = {m, base + heap, csize, 0, 0};
    uint64_t idx = 0;   /* Cluster-Index 0 = Cluster 2 */
    c = bm_first;
    while (idx < cc) {
        if (c < 2 || c >= cc + 2) goto out;
        for (uint64_t o = 0; o < csize && idx < cc; o += chunk) {
            if (read_bytes(fd, ss, base + heap + (uint64_t)(c - 2) * csize + o, buf, chunk)) goto out;
            for (size_t i = 0; i < chunk && idx < cc; i++)
                for (int bit = 0; bit < 8 && idx < cc; bit++, idx++) run_add(&r, idx, !((buf[i] >> bit) & 1));
        }
        uint8_t nx[4];
        if (read_bytes(fd, ss, base + fat + 4ull * c, nx, 4)) goto out;
        uint32_t next = le32(nx);
        c = next >= 2 && next < cc + 2 ? next : c + 1;   /* ohne gültige Kette: zusammenhängend (wie üblich) */
    }
    run_add(&r, idx, 0);
out:
    free(buf);
}

/* FAT32: FAT-Eintrag 0 = Cluster frei */
static void scan_fat32(int fd, uint32_t ss, uint64_t base, uint64_t plen, const uint8_t *bs, map_t *m) {
    const uint32_t bps = le16(bs + 11), spc = bs[13], rsvd = le16(bs + 14), nf = bs[16], fsz = le32(bs + 36);
    const uint64_t tot = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
    if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || !spc || (spc & (spc - 1)) || spc > 128 ||
        !nf || nf > 2 || !fsz || !rsvd)
        return;
    const uint64_t data = (uint64_t)rsvd + (uint64_t)nf * fsz, csize = (uint64_t)bps * spc;
    if (data >= tot) return;
    const uint64_t cc = (tot - data) / spc;
    if (data * bps + cc * csize > plen || cc + 2 > (uint64_t)fsz * bps / 4) return;
    uint8_t *buf = malloc(IB_BLOCK);
    if (!buf) return;
    runs_t r = {m, base + data * bps, csize, 0, 0};
    const uint64_t fat = base + (uint64_t)rsvd * bps;
    for (uint64_t c = 2; c < cc + 2; ) {
        uint64_t n = (cc + 2 - c) < IB_BLOCK / 4 ? (cc + 2 - c) : IB_BLOCK / 4;
        if (read_bytes(fd, ss, fat + 4 * c, buf, (size_t)(n * 4))) { free(buf); return; }
        for (uint64_t i = 0; i < n; i++) run_add(&r, c - 2 + i, (le32(buf + 4 * i) & 0x0FFFFFFF) == 0);
        c += n;
    }
    run_add(&r, cc, 0);
    free(buf);
}

/* Blöcke in [start, start + len) (auch angeschnittene) als belegt markieren */
static void map_use(map_t *m, uint64_t start, uint64_t len) {
    for (uint64_t b = start / IB_BLOCK; b < (start + len + IB_BLOCK - 1) / IB_BLOCK && b < m->n; b++)
        m->bits[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static void probe_part(int fd, uint32_t ss, uint64_t base, uint64_t plen, uint64_t size, map_t *m, uint64_t *end) {
    uint8_t bs[512];
    if (base >= size || base % ss || !plen) return;
    if (plen > size - base) plen = size - base;
    if (base + plen > *end) *end = base + plen;
    if (read_bytes(fd, ss, base, bs, sizeof(bs)) || le16(bs + 510) != 0xAA55) return;
    if (!memcmp(bs + 3, "EXFAT   ", 8)) scan_exfat(fd, ss, base, plen, bs, m);
    else if (!memcmp(bs + 82, "FAT32   ", 8)) scan_fat32(fd, ss, base, plen, bs, m);
}

/* Partitionen (MBR, GPT oder Dateisystem ohne Tabelle) durchgehen. Was hinter der letzten Partition liegt, wird
 * nicht gelesen (bei gefälschten Sticks der Teil ohne Speicher) - außer der GPT-Kopie am Ende. */
static void scan_disk(int fd, uint32_t ss, uint64_t size, map_t *m) {
    uint8_t s0[512];
    uint64_t end = 0;
    if (read_bytes(fd, ss, 0, s0, sizeof(s0)) || le16(s0 + 510) != 0xAA55) return;
    if (!memcmp(s0 + 3, "EXFAT   ", 8) || !memcmp(s0 + 82, "FAT32   ", 8)) {   /* ohne Partitionstabelle */
        probe_part(fd, ss, 0, size, size, m, &end);
        return;
    }
    int gpt = 0;
    for (int i = 0; i < 4; i++) gpt |= s0[446 + 16 * i + 4] == 0xEE;
    if (!gpt) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s0 + 446 + 16 * i;
            uint8_t t = e[4];
            if (t == 0x05 || t == 0x0F || t == 0x85) return;   /* erweiterte Partition: alles sichern */
            if (t) probe_part(fd, ss, (uint64_t)le32(e + 8) * ss, (uint64_t)le32(e + 12) * ss, size, m, &end);
        }
        if (end) map_free(m, end, size - end);
        return;
    }
    uint8_t h[92];
    if (read_bytes(fd, ss, ss, h, sizeof(h)) || memcmp(h, "EFI PART", 8)) return;
    uint64_t lba = le64(h + 72);
    uint32_t n = le32(h + 80), esz = le32(h + 84);
    if (esz < 128 || esz > 4096 || n > 256 || !n) return;
    uint64_t alt = le64(h + 32);   /* GPT-Kopie: Kopf im letzten Sektor, Einträge davor */
    uint8_t *ents = malloc((size_t)n * esz);
    if (!ents) return;
    if (!read_bytes(fd, ss, lba * ss, ents, (size_t)n * esz)) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = ents + (size_t)i * esz;
            int empty = 1;
            for (int k = 0; k < 16; k++) empty &= e[k] == 0;
            uint64_t first = le64(e + 32), last = le64(e + 40);
            if (!empty && last >= first) probe_part(fd, ss, first * ss, (last - first + 1) * ss, size, m, &end);
        }
        if (end && alt * ss < size && alt * ss >= end) {
            map_free(m, end, size - end);
            uint64_t tbl = (uint64_t)n * esz;
            map_use(m, alt * ss > tbl ? alt * ss - tbl : 0, tbl + ss);
        }
    }
    free(ents);
}

static int map_build(int fd, uint32_t ss, uint64_t size, map_t *m) {
    m->n = (size + IB_BLOCK - 1) / IB_BLOCK;
    m->bits = malloc((size_t)((m->n + 7) / 8));
    if (!m->bits) return -1;
    memset(m->bits, 0xFF, (size_t)((m->n + 7) / 8));
    scan_disk(fd, ss, size, m);
    return 0;
}

static uint64_t block_len(uint64_t b, uint64_t size) {
    uint64_t o = b * IB_BLOCK;
    return size - o < IB_BLOCK ? size - o : IB_BLOCK;
}

static uint64_t map_used_bytes(const map_t *m, uint64_t size) {
    uint64_t u = 0;
    for (uint64_t b = 0; b < m->n; b++) if (map_used(m, b)) u += block_len(b, size);
    return u;
}

uint64_t ib_used_bytes(int fd, uint32_t ss, uint64_t size) {
    map_t m;
    if (map_build(fd, ss, size, &m)) return size;
    uint64_t u = map_used_bytes(&m, size);
    free(m.bits);
    return u;
}

/* ------------------------------------------------------------------ JSON im Kopf */

static void json_put(char *o, size_t n, size_t *at, const char *key, const char *val, int last) {
    size_t k = *at;
    k += (size_t)snprintf(o + k, k < n ? n - k : 0, "\"%s\":\"", key);
    for (const unsigned char *p = (const unsigned char *)val; *p && k + 8 < n; p++) {
        if (*p == '"' || *p == '\\') { o[k++] = '\\'; o[k++] = (char)*p; }
        else if (*p < 0x20) k += (size_t)snprintf(o + k, n - k, "\\u%04x", *p);
        else o[k++] = (char)*p;
    }
    k += (size_t)snprintf(o + k, k < n ? n - k : 0, "\"%s", last ? "}" : ",");
    *at = k < n ? k : n - 1;
}

/* Wert eines Schlüssels aus dem flachen JSON-Objekt (Zeichenkette oder Zahl) */
static int json_get(const char *j, const char *key, char *out, size_t n) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return -1;
    p += strlen(pat);
    size_t k = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && k + 1 < n; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'u') { p += 4; out[k++] = '?'; continue; }
            }
            out[k++] = *p;
        }
    } else {
        while ((*p >= '0' && *p <= '9') && k + 1 < n) out[k++] = *p++;
    }
    out[k] = 0;
    return 0;
}

/* ------------------------------------------------------------------ Sichern */

typedef struct {
    ib_out_fn out;
    void     *ctx;
    sha256_t  sha;
    uint64_t  bytes;
} emit_t;

static int emit(emit_t *e, const void *p, size_t n) {
    if (!n) return 0;
    sha256_update(&e->sha, p, n);
    e->bytes += n;
    return e->out(e->ctx, p, n);
}

static int emit_skippable(emit_t *e, uint32_t magic, const void *payload, size_t n) {
    uint8_t h[8];
    put32(h, magic);
    put32(h + 4, (uint32_t)n);
    return emit(e, h, 8) || emit(e, payload, n);
}

/* Bytes freier Blöcke ab pos (pos auf Blockgrenze) */
static uint64_t free_len(const map_t *m, uint64_t pos, uint64_t size) {
    uint64_t b = pos / IB_BLOCK;
    while (b < m->n && !map_used(m, b)) b++;
    uint64_t e = b * IB_BLOCK;
    return (e > size ? size : e) - pos;
}

/* Ende eines Datenabschnitts ab pos: Beginn der nächsten freien Strecke ab ZERO_MIN, sonst das Ende */
static uint64_t data_end(const map_t *m, uint64_t pos, uint64_t size) {
    uint64_t run = 0;
    for (uint64_t b = pos / IB_BLOCK; b < m->n; b++) {
        if (map_used(m, b)) run = 0;
        else if (++run * IB_BLOCK >= ZERO_MIN) return (b + 1 - run) * IB_BLOCK;
    }
    return size;
}

static uint64_t zero_frame_bytes(uint64_t len) { return 14 + (len + RLE_MAX - 1) / RLE_MAX * 4; }

/* Hinweis + zstd-Rahmen aus RLE-Blöcken für len Null-Bytes */
static int emit_zero(emit_t *e, uint64_t len, volatile int *cancel, int *cancelled) {
    uint8_t note[28], buf[4096];
    memcpy(note, ZERO_TAG, 12);
    put64(note + 12, len);
    put64(note + 20, zero_frame_bytes(len));
    if (emit_skippable(e, MAGIC_ZERO, note, sizeof(note))) return -1;
    put32(buf, ZSTD_MAGICNUMBER);
    buf[4] = 0xC0;   /* Inhaltsgröße 8 Bytes, kein Einzelsegment, ohne Prüfsumme */
    buf[5] = 0x38;   /* Fenster 128 KiB = größter Block */
    put64(buf + 6, len);
    size_t k = 14;
    for (uint64_t left = len, n = 0; left; n++) {
        if (!(n & 0xFFFF) && __atomic_load_n(cancel, __ATOMIC_RELAXED)) { *cancelled = 1; return 0; }
        uint32_t b = left < RLE_MAX ? (uint32_t)left : RLE_MAX;
        left -= b;
        uint32_t h = (left ? 0 : 1) | 1u << 1 | b << 3;   /* letzter Block, Typ RLE, Länge */
        buf[k] = (uint8_t)h;
        buf[k + 1] = (uint8_t)(h >> 8);
        buf[k + 2] = (uint8_t)(h >> 16);
        buf[k + 3] = 0;
        k += 4;
        if (k + 4 > sizeof(buf) || !left) {
            if (emit(e, buf, k)) return -1;
            k = 0;
        }
    }
    return 0;
}

int ib_threads = -1;

/* Threads für zstd: gepackt wird nebenher, während dieser Thread liest, prüft (SHA-256) und schreibt */
static int compress_threads(void) {
    if (ib_threads >= 0) return ib_threads;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long n = (long)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n >= 4 ? 4 : n >= 2 ? (int)n - 1 : 0;
}

int ib_backup(int fd, uint32_t ss, uint64_t size, ib_info_t *info, ib_out_fn out, void *octx,
              ib_progress_fn progress, volatile int *cancel, ib_result_t *r, char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    map_t m = {0};
    uint8_t *page = NULL, *obuf = NULL, *head = NULL;
    ZSTD_CCtx *cc = NULL;
    int rc = -1;
    emit_t e = {out, octx, {{0}, 0, {0}, 0}, 0};
    sha256_t su;   /* über die belegten Blöcke */
    sha256_init(&e.sha);
    sha256_init(&su);
    if (!size || size % ss) { snprintf(err, errlen, "%s", T("Ungültige Größe")); return -1; }

    progress(T("Belegte Bereiche ermitteln"), 0);
    if (map_build(fd, ss, size, &m)) { snprintf(err, errlen, "%s", T("Kein Speicher")); return -1; }
    info->size = size;
    info->used = map_used_bytes(&m, size);
    info->has_map = 1;
    if (!info->created[0]) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(info->created, sizeof(info->created), "%Y-%m-%d %H:%M", &tmv);
    }

    /* Kopf: Kennung, Angaben, Karte */
    const size_t mapb = (size_t)((m.n + 7) / 8);
    char js[1024];
    size_t at = (size_t)snprintf(js, sizeof(js), "{\"size\":%llu,\"used\":%llu,\"block\":%u,",
                                 (unsigned long long)size, (unsigned long long)info->used, IB_BLOCK);
    json_put(js, sizeof(js), &at, "model", info->model, 0);
    json_put(js, sizeof(js), &at, "label", info->label, 0);
    json_put(js, sizeof(js), &at, "fs", info->fs, 0);
    json_put(js, sizeof(js), &at, "ident", info->ident, 0);
    json_put(js, sizeof(js), &at, "created", info->created, 1);
    const size_t hlen = strlen(HEAD_TAG) + at + 1 + mapb;
    if (hlen > HEAD_MAX || !(head = malloc(hlen))) { snprintf(err, errlen, "%s", T("Kein Speicher")); goto out; }
    memcpy(head, HEAD_TAG, strlen(HEAD_TAG));
    memcpy(head + strlen(HEAD_TAG), js, at);
    head[strlen(HEAD_TAG) + at] = '\n';
    memcpy(head + strlen(HEAD_TAG) + at + 1, m.bits, mapb);
    if (emit_skippable(&e, MAGIC_HEAD, head, hlen)) goto werr;

    cc = ZSTD_createCCtx();
    const size_t olen = ZSTD_CStreamOutSize();
    page = aligned(PAGE);
    obuf = malloc(olen);
    if (!cc || !page || !obuf) { snprintf(err, errlen, "%s", T("Kein Speicher")); goto out; }
    ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 3);
    ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1);
    const int workers = compress_threads();
    if (workers > 0 && !ZSTD_isError(ZSTD_CCtx_setParameter(cc, ZSTD_c_nbWorkers, workers)))
        ZSTD_CCtx_setParameter(cc, ZSTD_c_jobSize, 4 << 20);   /* kleine Aufträge: wenig Speicher je Thread */

    for (uint64_t seg = 0; seg < size; ) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
        const uint64_t zl = free_len(&m, seg, size);
        if (zl >= ZERO_MIN) {   /* lange freie Strecke: nicht packen, nur beschreiben */
            if (emit_zero(&e, zl, cancel, &r->cancelled)) goto werr;
            if (r->cancelled) { rc = 0; goto out; }
            seg += zl;
            progress(T("Sichern"), (int)(seg * 100 / size));
            continue;
        }
        const uint64_t end = data_end(&m, seg, size);
        ZSTD_CCtx_reset(cc, ZSTD_reset_session_only);
        ZSTD_CCtx_setPledgedSrcSize(cc, end - seg);
        for (uint64_t pos = seg; pos < end; pos += PAGE) {
            if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
            const size_t len = end - pos < PAGE ? (size_t)(end - pos) : PAGE;
            /* belegte Blöcke am Stück lesen, freie als Nullen */
            for (size_t o = 0; o < len; ) {
                uint64_t b = (pos + o) / IB_BLOCK;
                size_t run = 0;
                int used = map_used(&m, b);
                while (o + run < len && map_used(&m, (pos + o + run) / IB_BLOCK) == used)
                    run += (size_t)block_len((pos + o + run) / IB_BLOCK, size);
                if (used) {
                    if (rd_all(fd, page + o, run, pos + o)) {
                        snprintf(err, errlen, T("Lesefehler bei %llu MB: %s"), (unsigned long long)((pos + o) >> 20),
                                 strerror(errno));
                        goto out;
                    }
                    sha256_update(&su, page + o, run);
                    r->used += run;
                } else {
                    memset(page + o, 0, run);
                }
                o += run;
            }
            ZSTD_inBuffer in = {page, len, 0};
            while (in.pos < in.size) {
                ZSTD_outBuffer ob = {obuf, olen, 0};
                size_t z = ZSTD_compressStream2(cc, &ob, &in, ZSTD_e_continue);
                if (ZSTD_isError(z)) { snprintf(err, errlen, "zstd: %s", ZSTD_getErrorName(z)); goto out; }
                if (emit(&e, obuf, ob.pos)) goto werr;
            }
            progress(T("Sichern"), (int)((pos + len) * 100 / size));
        }
        for (;;) {
            ZSTD_inBuffer in = {NULL, 0, 0};
            ZSTD_outBuffer ob = {obuf, olen, 0};
            size_t z = ZSTD_compressStream2(cc, &ob, &in, ZSTD_e_end);
            if (ZSTD_isError(z)) { snprintf(err, errlen, "zstd: %s", ZSTD_getErrorName(z)); goto out; }
            if (emit(&e, obuf, ob.pos)) goto werr;
            if (!z) break;
        }
        seg = end;
    }
    uint8_t d[32];
    sha256_final(&su, d);
    sha256_hex(d, r->sha_img);
    char tail[96];
    int tl = snprintf(tail, sizeof(tail), "%s%s\n", TAIL_TAG, r->sha_img);
    if (emit_skippable(&e, MAGIC_TAIL, tail, (size_t)tl)) goto werr;
    sha256_final(&e.sha, d);
    sha256_hex(d, r->sha_file);
    r->size = size;
    r->file_bytes = e.bytes;
    rc = 0;
    goto out;
werr:
    if (!err[0]) snprintf(err, errlen, "%s", T("Schreiben der Sicherung fehlgeschlagen"));
out:
    ZSTD_freeCCtx(cc);
    free(page);
    free(obuf);
    free(head);
    free(m.bits);
    return rc;
}

/* ------------------------------------------------------------------ Wiederherstellen */

struct ib_reader {
    ib_in_fn  in;
    void     *ctx;
    uint8_t  *buf;
    size_t    cap, pos, len;
    ib_info_t info;
    map_t     map;
};

/* mindestens n Bytes im Puffer (soweit der Strom sie hat); Rückgabe: verfügbare Bytes, < 0 Fehler */
static long rd_peek(ib_reader_t *r, size_t n) {
    if (r->len - r->pos >= n) return (long)(r->len - r->pos);
    memmove(r->buf, r->buf + r->pos, r->len - r->pos);
    r->len -= r->pos;
    r->pos = 0;
    while (r->len < n) {
        long g = r->in(r->ctx, r->buf + r->len, r->cap - r->len);
        if (g < 0) return -1;
        if (g == 0) break;
        r->len += (size_t)g;
    }
    return (long)r->len;
}

static int rd_exact(ib_reader_t *r, void *dst, size_t n) {
    uint8_t *d = dst;
    while (n) {
        if (r->pos == r->len) {
            long g = rd_peek(r, 1);
            if (g <= 0) return -1;
        }
        size_t take = r->len - r->pos < n ? r->len - r->pos : n;
        memcpy(d, r->buf + r->pos, take);
        r->pos += take;
        d += take;
        n -= take;
    }
    return 0;
}

ib_reader_t *ib_open(ib_in_fn in, void *ctx, ib_info_t *info, char *err, size_t errlen) {
    ib_reader_t *r = calloc(1, sizeof(*r));
    if (!r || !(r->cap = ZSTD_DStreamInSize()) || !(r->buf = malloc(r->cap))) {
        snprintf(err, errlen, "%s", T("Kein Speicher"));
        if (r) free(r->buf);
        free(r);
        return NULL;
    }
    r->in = in;
    r->ctx = ctx;
    uint8_t *head = NULL;
    if (rd_peek(r, 8) < 8) goto bad;
    if (le32(r->buf + r->pos) == MAGIC_HEAD) {
        uint32_t hl = le32(r->buf + r->pos + 4);
        r->pos += 8;
        if (hl < strlen(HEAD_TAG) + 2 || hl > HEAD_MAX || !(head = malloc(hl + 1))) goto bad;
        if (rd_exact(r, head, hl)) goto bad;
        head[hl] = 0;
        if (memcmp(head, HEAD_TAG, strlen(HEAD_TAG)) && memcmp(head, HEAD_TAG1, strlen(HEAD_TAG1))) goto bad;
        char *js = (char *)head + strlen(HEAD_TAG), *nl = memchr(js, '\n', hl - strlen(HEAD_TAG));
        if (!nl) goto bad;
        *nl = 0;
        char v[64];
        if (json_get(js, "size", v, sizeof(v)) || !(r->info.size = strtoull(v, NULL, 10))) goto bad;
        if (!json_get(js, "used", v, sizeof(v))) r->info.used = strtoull(v, NULL, 10);
        if (json_get(js, "block", v, sizeof(v)) || strtoul(v, NULL, 10) != IB_BLOCK) goto bad;
        json_get(js, "model", r->info.model, sizeof(r->info.model));
        json_get(js, "label", r->info.label, sizeof(r->info.label));
        json_get(js, "fs", r->info.fs, sizeof(r->info.fs));
        json_get(js, "ident", r->info.ident, sizeof(r->info.ident));
        json_get(js, "created", r->info.created, sizeof(r->info.created));
        r->map.n = (r->info.size + IB_BLOCK - 1) / IB_BLOCK;
        size_t mapb = (size_t)((r->map.n + 7) / 8), off = (size_t)((uint8_t *)nl + 1 - head);
        if (hl - off != mapb || !(r->map.bits = malloc(mapb))) goto bad;
        memcpy(r->map.bits, head + off, mapb);
        r->info.has_map = 1;
        free(head);
        head = NULL;
        if (rd_peek(r, 18) < 4) goto bad;
    }
    /* der zstd-Rahmen: Inhaltsgröße aus dem Rahmenkopf (auch für mit "zstd" selbst gepackte Images) */
    long av = rd_peek(r, 18);
    if (r->info.has_map && av >= 4 && le32(r->buf + r->pos) == MAGIC_ZERO) {   /* beginnt mit einer freien Strecke */
        *info = r->info;
        return r;
    }
    if (av < 4 || le32(r->buf + r->pos) != ZSTD_MAGICNUMBER) goto bad;
    unsigned long long cs = ZSTD_getFrameContentSize(r->buf + r->pos, (size_t)av);
    if (cs == ZSTD_CONTENTSIZE_UNKNOWN || cs == ZSTD_CONTENTSIZE_ERROR || !cs) {
        snprintf(err, errlen, "%s", T("Die Größe des Images ist unbekannt (mit zstd ohne Größenangabe gepackt)."));
        ib_close(r);
        return NULL;
    }
    if (!r->info.has_map) r->info.size = cs;   /* eigene Images: Größe aus dem Kopf, verteilt auf mehrere Rahmen */
    if (!r->info.has_map) r->info.used = cs;
    *info = r->info;
    return r;
bad:
    free(head);
    if (!err[0]) snprintf(err, errlen, "%s", T("Keine gültige Image-Datei (.img.zst)."));
    ib_close(r);
    return NULL;
}

void ib_close(ib_reader_t *r) {
    if (!r) return;
    free(r->buf);
    free(r->map.bits);
    free(r);
}

/* belegte Blöcke einer fertigen Seite schreiben; sha_w über das Geschriebene */
static int write_page(ib_reader_t *rd, int fd, const uint8_t *page, uint64_t pos, size_t len, sha256_t *sw,
                      uint64_t *written, char *err, size_t errlen) {
    for (size_t o = 0; o < len; ) {
        uint64_t b = (pos + o) / IB_BLOCK;
        int used = !rd->info.has_map || map_used(&rd->map, b);
        size_t run = 0;
        while (o + run < len && (!rd->info.has_map || map_used(&rd->map, (pos + o + run) / IB_BLOCK)) == used)
            run += (size_t)block_len((pos + o + run) / IB_BLOCK, rd->info.size);
        if (used) {
            if (wr_all(fd, page + o, run, pos + o)) {
                snprintf(err, errlen, T("Schreibfehler bei %llu MB: %s"), (unsigned long long)((pos + o) >> 20),
                         strerror(errno));
                return -1;
            }
            sha256_update(sw, page + o, run);
            *written += run;
        }
        o += run;
    }
    return 0;
}

/* Hinweis auf eine freie Strecke und den Null-Rahmen dahinter lesen, prüfen und überspringen (die Blöcke sind laut
 * Karte frei und werden ohnehin nicht geschrieben). 0 = weiter, 1 = abgebrochen, -1 = Fehler. */
static int skip_zero(ib_reader_t *rd, uint64_t *pos, uint64_t size, volatile int *cancel, ib_progress_fn progress,
                     char *err, size_t errlen) {
    uint8_t note[36], h[14], blk[4];
    if (rd_exact(rd, note, sizeof(note))) goto eof;
    if (le32(note + 4) != 28 || memcmp(note + 8, ZERO_TAG, 12)) goto bad;
    const uint64_t len = le64(note + 20);
    if (!len || len > size - *pos || le64(note + 28) != zero_frame_bytes(len) || *pos % IB_BLOCK) goto bad;
    for (uint64_t b = *pos / IB_BLOCK; b < (*pos + len + IB_BLOCK - 1) / IB_BLOCK; b++)
        if (map_used(&rd->map, b)) goto bad;
    if (rd_exact(rd, h, sizeof(h))) goto eof;
    if (le32(h) != ZSTD_MAGICNUMBER || h[4] != 0xC0 || h[5] != 0x38 || le64(h + 6) != len) goto bad;
    for (uint64_t left = len, n = 0; left; n++) {
        if (!(n & 0xFFFF)) {
            if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) return 1;
            progress(T("Wiederherstellen"), (int)((*pos + len - left) * 100 / size));
        }
        if (rd_exact(rd, blk, 4)) goto eof;
        const uint32_t hv = (uint32_t)blk[0] | (uint32_t)blk[1] << 8 | (uint32_t)blk[2] << 16;
        const uint32_t b = hv >> 3, want = left < RLE_MAX ? (uint32_t)left : RLE_MAX;
        left -= want;
        if ((hv >> 1 & 3) != 1 || blk[3] || b != want || (hv & 1) != !left) goto bad;
    }
    *pos += len;
    progress(T("Wiederherstellen"), (int)(*pos * 100 / size));
    return 0;
eof:   /* Ende der Datei bzw. Lesefehler (Hochladen abgebrochen) */
    snprintf(err, errlen, "%s", T("Die Image-Datei ist unvollständig."));
    return -1;
bad:
    snprintf(err, errlen, "%s", T("Die Image-Datei ist beschädigt."));
    return -1;
}

int ib_restore(ib_reader_t *rd, int fd, int vfd, uint32_t ss, uint64_t devsize, ib_progress_fn progress,
               volatile int *cancel, ib_result_t *r, char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    const uint64_t size = rd->info.size;
    if (size > devsize) {
        snprintf(err, errlen, "%s", T("Das Image ist größer als der Datenträger."));
        return -1;
    }
    if (size % ss) { snprintf(err, errlen, "%s", T("Ungültige Größe")); return -1; }
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    uint8_t *page = aligned(PAGE);
    int rc = -1;
    if (!dc || !page) { snprintf(err, errlen, "%s", T("Kein Speicher")); goto out; }
    ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 27);

    sha256_t sw;   /* über das Geschriebene - bei eigenen Images die belegten Blöcke, wie beim Sichern */
    sha256_init(&sw);
    uint64_t pos = 0;
    size_t fill = 0;
    /* Rahmen für Rahmen; fremde Images haben genau einen */
    for (int frame = 0; pos < size && (rd->info.has_map || !frame); frame++) {
        long av = rd_peek(rd, 8);
        if (av < 0) { snprintf(err, errlen, "%s", T("Lesen der Sicherung fehlgeschlagen")); goto out; }
        if (rd->info.has_map && av >= 8 && le32(rd->buf + rd->pos) == MAGIC_ZERO) {
            int zr = skip_zero(rd, &pos, size, cancel, progress, err, errlen);
            if (zr < 0) goto out;
            if (zr > 0) { r->cancelled = 1; rc = 0; goto out; }
            continue;
        }
        int done = 0;
        while (!done) {
            if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
            if (rd->pos == rd->len) {
                long g = rd_peek(rd, 1);
                if (g < 0) { snprintf(err, errlen, "%s", T("Lesen der Sicherung fehlgeschlagen")); goto out; }
                if (g == 0) { snprintf(err, errlen, "%s", T("Die Image-Datei ist unvollständig.")); goto out; }
            }
            ZSTD_inBuffer in = {rd->buf + rd->pos, rd->len - rd->pos, 0};
            ZSTD_outBuffer ob = {page + fill, PAGE - fill, 0};
            size_t z = ZSTD_decompressStream(dc, &ob, &in);
            rd->pos += in.pos;
            if (ZSTD_isError(z)) {
                snprintf(err, errlen, T("Die Image-Datei ist beschädigt (zstd: %s)."), ZSTD_getErrorName(z));
                goto out;
            }
            fill += ob.pos;
            done = z == 0;
            if (fill == PAGE || (done && fill)) {
                if (pos + fill > size) { snprintf(err, errlen, "%s", T("Die Image-Datei ist beschädigt.")); goto out; }
                if (write_page(rd, fd, page, pos, fill, &sw, &r->used, err, errlen)) goto out;
                pos += fill;
                fill = 0;
                progress(T("Wiederherstellen"), (int)(pos * 100 / size));
            }
        }
    }
    if (pos != size) { snprintf(err, errlen, "%s", T("Die Image-Datei ist unvollständig.")); goto out; }
    /* dahinter noch ein zstd-Rahmen (z. B. von pzstd gepackt): die Größe galt nur für den ersten - nicht als fertig melden */
    if (rd_peek(rd, 4) >= 4 && le32(rd->buf + rd->pos) == ZSTD_MAGICNUMBER) {
        snprintf(err, errlen, "%s", T("Das Image besteht aus mehreren zstd-Rahmen und wurde nur teilweise geschrieben – bitte "
                                      "mit „zstd -d“ entpacken und mit „zstd“ neu packen."));
        goto out;
    }
    uint8_t want[32], got[32];
    sha256_final(&sw, want);
    sha256_hex(want, r->sha_img);
    /* Prüfsumme am Ende (eigene Images): belegte Blöcke unverändert? */
    if (rd_peek(rd, 8) >= 8 && le32(rd->buf + rd->pos) == MAGIC_TAIL) {
        uint32_t tl = le32(rd->buf + rd->pos + 4);
        char tail[128] = "";
        rd->pos += 8;
        if (tl < sizeof(tail) && !rd_exact(rd, tail, tl)) {
            tail[tl] = 0;
            if (!strncmp(tail, TAIL_TAG, strlen(TAIL_TAG)) && strncmp(tail + strlen(TAIL_TAG), r->sha_img, 64)) {
                snprintf(err, errlen, "%s", T("Die Prüfsumme des Images stimmt nicht – die Datei ist beschädigt."));
                goto out;
            }
        }
    }
    (void)fsync(fd);

    /* Nachlesen: steht auf dem Datenträger, was geschrieben wurde? */
    sha256_t sv;
    sha256_init(&sv);
    uint64_t checked = 0;
    for (uint64_t b = 0; b < rd->map.n || (!rd->info.has_map && b * IB_BLOCK < size); ) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
        if (rd->info.has_map && !map_used(&rd->map, b)) { b++; continue; }
        size_t run = 0;
        uint64_t start = b * IB_BLOCK;
        while (b * IB_BLOCK < size && run < PAGE && (!rd->info.has_map || map_used(&rd->map, b))) {
            run += (size_t)block_len(b, size);
            b++;
        }
        if (rd_all(vfd, page, run, start)) {
            snprintf(err, errlen, T("Lesefehler bei %llu MB: %s"), (unsigned long long)(start >> 20), strerror(errno));
            goto out;
        }
        sha256_update(&sv, page, run);
        checked += run;
        progress(T("Geschriebenes prüfen"), r->used ? (int)(checked * 100 / r->used) : 100);
    }
    sha256_final(&sv, got);
    if (memcmp(want, got, 32)) {
        snprintf(err, errlen, "%s", T("Prüfung nach dem Schreiben: Der Datenträger liefert andere Daten zurück – er ist "
                                      "vermutlich defekt oder gefälscht."));
        goto out;
    }
    r->size = size;
    rc = 0;
out:
    ZSTD_freeDCtx(dc);
    free(page);
    return rc;
}

void ib_file_name(const ib_info_t *info, char *out, size_t n) {
    char part[3][64];
    const char *src[3] = {info->model[0] ? info->model : "USB", info->label, info->created};
    for (int k = 0; k < 3; k++) {
        size_t j = 0;
        for (const char *p = src[k]; *p && j + 1 < sizeof(part[k]); p++) {
            char c = *p;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') part[k][j++] = c;
            else if (j && part[k][j - 1] != '_') part[k][j++] = '_';
        }
        while (j && part[k][j - 1] == '_') j--;
        part[k][j] = 0;
    }
    snprintf(out, n, "%s%s%s%s%s.img.zst", part[0], part[1][0] ? "_" : "", part[1], part[2][0] ? "_" : "", part[2]);
}
