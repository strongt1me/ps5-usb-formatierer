/*
 * Mac OS Extended (HFS+) ohne Journal - leeres Dateisystem in eine Partition schreiben.
 * Referenz: Apple Technical Note TN1150 "HFS Plus Volume Format".
 *
 * Angelegt werden: Volume-Header (Byte 1024, Kopie 1024 Bytes vor dem Ende), Zuordnungs-Bitmap, Extents-Überlauf-
 * B-Baum (nur Kopfknoten) und Katalog-B-Baum (Kopfknoten + ein Blatt mit Wurzelordner und dessen Thread-Eintrag).
 * Alle Zahlen big-endian. macOS legt Journal, Attribut-Datei und "HFS+ Private Data" bei Bedarf selbst an.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mkfs.h"
#include "mkfs_int.h"

#define MIB              (1024u * 1024u)
#define HFS_EPOCH_DELTA  2082844800u      /* Sekunden vom 1.1.1904 (HFS) bis 1.1.1970 */
#define EXT_NODE         4096u            /* Knotengröße Extents-B-Baum (wie newfs_hfs) */
#define CAT_NODE         8192u            /* Knotengröße Katalog-B-Baum (wie newfs_hfs) */
#define ROOT_PARENT_ID   1u               /* kHFSRootParentID */
#define ROOT_FOLDER_ID   2u               /* kHFSRootFolderID */
#define FIRST_USER_ID    16u              /* kHFSFirstUserCatalogNodeID */

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v); }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }

/* HFSPlusForkData (80 Bytes) mit einer zusammenhängenden Ausdehnung */
static void put_fork(uint8_t *p, uint64_t bytes, uint32_t clump, uint32_t start, uint32_t blocks) {
    be64(p, bytes);
    be32(p + 8, clump);
    be32(p + 12, blocks);
    be32(p + 16, start);
    be32(p + 20, blocks);
}

/* Kopfknoten (Knoten 0) eines B-Baums: Deskriptor, Kopfsatz, 128 Bytes Benutzerdaten, Belegungskarte der Knoten */
static void btree_header(uint8_t *node, uint32_t nsize, uint32_t depth, uint32_t root, uint32_t leafrecs,
                         uint32_t leaf, uint32_t maxkey, uint32_t total, uint32_t used, uint32_t clump,
                         uint32_t attrs) {
    memset(node, 0, nsize);
    node[8] = 1;                         /* kBTHeaderNode */
    be16(node + 10, 3);                  /* drei Sätze */
    uint8_t *h = node + 14;              /* BTHeaderRec, 106 Bytes */
    be16(h, depth);
    be32(h + 2, root);
    be32(h + 6, leafrecs);
    be32(h + 10, leaf);                  /* erstes Blatt */
    be32(h + 14, leaf);                  /* letztes Blatt */
    be16(h + 18, nsize);
    be16(h + 20, maxkey);
    be32(h + 22, total);
    be32(h + 26, total - used);          /* freie Knoten */
    be32(h + 32, clump);
    h[36] = 0;                           /* btreeType: HFS-B-Baum */
    h[37] = 0;                           /* keyCompareType: bei HFS+ (nicht HFSX) reserviert */
    be32(h + 38, attrs);
    uint8_t *map = node + 248;           /* Belegungskarte: ein Bit je Knoten */
    for (uint32_t i = 0; i < used; i++) map[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    be16(node + nsize - 2, 14);          /* Satz-Anfänge, von hinten: Kopfsatz, Benutzerdaten, Karte, frei */
    be16(node + nsize - 4, 120);
    be16(node + nsize - 6, 248);
    be16(node + nsize - 8, nsize - 8);
}

/* HFSUniStr255: Länge + UTF-16BE (die Bezeichnung ist ASCII); liefert die geschriebenen Bytes */
static uint32_t put_name(uint8_t *p, const char *name) {
    uint32_t n = (uint32_t)strlen(name);
    be16(p, n);
    for (uint32_t i = 0; i < n; i++) be16(p + 2 + 2 * i, (uint8_t)name[i]);
    return 2 + 2 * n;
}

/* Blatt des Katalogs: Wurzelordner (Schlüssel: Eltern-ID 1 + Volume-Name) und sein Thread (Schlüssel: Ordner-ID 2,
 * leerer Name) - sortiert nach Eltern-ID */
static void catalog_leaf(uint8_t *node, const char *name, uint32_t now) {
    memset(node, 0, CAT_NODE);
    node[8] = 0xFF;                      /* kBTLeafNode (-1) */
    node[9] = 1;                         /* Höhe */
    be16(node + 10, 2);
    const uint32_t nlen = (uint32_t)strlen(name);

    uint32_t pos = 14, rec1;
    be16(node + pos, 6 + 2 * nlen);      /* Schlüssellänge ohne das Längenfeld */
    be32(node + pos + 2, ROOT_PARENT_ID);
    put_name(node + pos + 6, name);
    pos += 2 + 6 + 2 * nlen;
    uint8_t *f = node + pos;             /* HFSPlusCatalogFolder, 88 Bytes */
    be16(f, 1);                          /* kHFSPlusFolderRecord */
    be32(f + 8, ROOT_FOLDER_ID);
    be32(f + 12, now);                   /* erstellt, Inhalt/Attribute geändert, Zugriff */
    be32(f + 16, now);
    be32(f + 20, now);
    be32(f + 24, now);
    be32(f + 32, 99);                    /* Besitzer/Gruppe "unbekannt" wie bei Wechseldatenträgern */
    be32(f + 36, 99);
    be16(f + 42, 0040755);               /* Ordner, rwxr-xr-x */
    pos += 88;

    rec1 = pos;
    be16(node + pos, 6);                 /* Schlüssel: Eltern-ID = Wurzelordner, leerer Name */
    be32(node + pos + 2, ROOT_FOLDER_ID);
    pos += 8;
    be16(node + pos, 3);                 /* kHFSPlusFolderThreadRecord */
    be32(node + pos + 4, ROOT_PARENT_ID);
    pos += 8 + put_name(node + pos + 8, name);

    be16(node + CAT_NODE - 2, 14);
    be16(node + CAT_NODE - 4, rec1);
    be16(node + CAT_NODE - 6, pos);      /* Beginn des freien Platzes */
}

static uint64_t round_up(uint64_t v, uint64_t m) { return (v + m - 1) / m * m; }

/* Bits [from, to) setzen, soweit sie im Ausschnitt [b0, b1) der Bitmap liegen (b0 durch 8 teilbar) */
static void set_bits(uint8_t *buf, uint64_t b0, uint64_t b1, uint64_t from, uint64_t to) {
    if (from < b0) from = b0;
    if (to > b1) to = b1;
    for (uint64_t b = from; b < to; b++) buf[(b - b0) / 8] |= (uint8_t)(0x80u >> (b % 8));
}

int mkfs_hfsplus(blkdev_t *d, uint64_t nsect, const char *label, uint32_t serial, uint32_t block) {
    const uint32_t ss = d->ss;
    const uint32_t bs = block ? block : 4096;
    const char *name = label && *label ? label : "Ohne Titel";
    uint8_t *hdr = NULL, *cat = NULL, *bm = NULL, *vh = NULL;
    int rc = -1;

    if (ss < 512 || ss > 4096) return mk_fail("Nicht unterstützte Sektorgröße %u", ss);
    if (bs < 512 || bs > 64u * 1024u || (bs & (bs - 1))) return mk_fail("Ungültiger Zuordnungsblock für HFS+ (%u Bytes)", bs);
    if (bs < ss) return mk_fail("Zuordnungsblock kleiner als ein Sektor");
    if (strlen(name) > 255) return mk_fail("Bezeichnung zu lang");

    const uint64_t vbytes = nsect * ss;
    const uint64_t total64 = vbytes / bs;
    if (total64 > 0xFFFFFFFFull) return mk_fail("Zuordnungsblock zu klein für diese Größe – bitte einen größeren wählen");
    const uint32_t total = (uint32_t)total64;

    /* Größen der Systemdateien (ganze Blöcke und ganze Knoten), ungefähr wie newfs_hfs */
    const uint64_t ext_bytes = round_up(vbytes > 1024ull * MIB ? MIB : 256 * 1024, bs > EXT_NODE ? bs : EXT_NODE);
    const uint64_t cat_want = vbytes > 64ull * 1024 * MIB ? 8 * MIB : vbytes > 1024ull * MIB ? 4 * MIB : MIB;
    const uint64_t cat_bytes = round_up(cat_want, bs > CAT_NODE ? bs : CAT_NODE);
    const uint64_t bm_bytes = round_up((total + 7) / 8, bs);
    const uint32_t first = (uint32_t)((1536 + bs - 1) / bs);   /* Startblöcke + Volume-Header belegt */
    const uint32_t bm_blocks = (uint32_t)(bm_bytes / bs), ext_blocks = (uint32_t)(ext_bytes / bs),
                   cat_blocks = (uint32_t)(cat_bytes / bs);
    const uint32_t bm_start = first, ext_start = bm_start + bm_blocks, cat_start = ext_start + ext_blocks;
    const uint32_t next_free = cat_start + cat_blocks;
    /* Blöcke, die die Kopie des Volume-Headers (1024 Bytes vor dem Ende) berühren, sind belegt - und immer der letzte
     * Block, auch wenn die Kopie im Rest dahinter liegt (Größe kein Vielfaches des Blocks; so prüft fsck_hfs) */
    uint32_t tail_start = (uint32_t)((vbytes - 1024) / bs);
    if (tail_start > total - 1) tail_start = total - 1;
    if ((uint64_t)next_free + (total - tail_start) + 8 > total) return mk_fail("Partition zu klein für HFS+");
    const uint32_t cat_nodes = (uint32_t)(cat_bytes / CAT_NODE), ext_nodes = (uint32_t)(ext_bytes / EXT_NODE);
    if (cat_nodes > (CAT_NODE - 256) * 8 || ext_nodes > (EXT_NODE - 256) * 8) return mk_fail("Interner Fehler: B-Baum zu groß");
    const uint32_t used = next_free + (total - tail_start);

    const uint32_t now = (uint32_t)(time(NULL) + HFS_EPOCH_DELTA);
    time_t t = time(NULL);
    struct tm lt, *g = gmtime(&t);
    struct tm gt = *g;
    localtime_r(&t, &lt);
    gt.tm_isdst = lt.tm_isdst;
    const uint32_t now_local = now + (uint32_t)(int32_t)(mktime(&lt) - mktime(&gt));   /* Erstelldatum: Ortszeit */

    const uint32_t chunk = MIB;          /* Puffer: 1 MiB (mindestens ein Block) */
    const uint32_t buf_len = bs > chunk ? bs : chunk;
    if (!(bm = calloc(1, buf_len)) || !(hdr = calloc(1, EXT_NODE)) || !(cat = calloc(1, 2 * CAT_NODE)) ||
        !(vh = calloc(1, 4096))) {
        rc = mk_fail("Kein Speicher");
        goto out;
    }

    /* Zuordnungs-Bitmap in Stücken: belegt sind Start, Systemdateien und das Ende */
    mk_report(d, "Zuordnungs-Bitmap schreiben", 0);
    for (uint64_t off = 0; off < bm_bytes; off += buf_len) {
        uint64_t len = bm_bytes - off < buf_len ? bm_bytes - off : buf_len;
        memset(bm, 0, buf_len);
        uint64_t b0 = off * 8, b1 = (off + len) * 8;
        set_bits(bm, b0, b1, 0, next_free);
        set_bits(bm, b0, b1, tail_start, total);
        if (mk_write(d, ((uint64_t)bm_start * bs + off) / ss, bm, len / ss)) goto out;
        mk_report(d, "Zuordnungs-Bitmap schreiben", (int)((off + len) * 60 / bm_bytes));
    }

    /* Extents-Überlauf: nur der Kopfknoten, Rest der Datei leer */
    mk_report(d, "B-Bäume schreiben", 60);
    if (mk_zero(d, (uint64_t)ext_start * bs / ss, ext_bytes / ss, NULL)) goto out;
    btree_header(hdr, EXT_NODE, 0, 0, 0, 0, 10, ext_nodes, 1, (uint32_t)ext_bytes, 2 /* kBTBigKeysMask */);
    if (mk_write(d, (uint64_t)ext_start * bs / ss, hdr, EXT_NODE / ss)) goto out;

    /* Katalog: Kopfknoten + Blatt, Rest leer */
    mk_report(d, "B-Bäume schreiben", 75);
    if (mk_zero(d, (uint64_t)cat_start * bs / ss, cat_bytes / ss, NULL)) goto out;
    btree_header(cat, CAT_NODE, 1, 1, 2, 1, 516, cat_nodes, 2, (uint32_t)cat_bytes,
                 2 | 4 /* kBTBigKeysMask | kBTVariableIndexKeysMask */);
    catalog_leaf(cat + CAT_NODE, name, now);
    if (mk_write(d, (uint64_t)cat_start * bs / ss, cat, 2 * CAT_NODE / ss)) goto out;

    /* Volume-Header */
    uint8_t *v = vh + 1024;
    be16(v, 0x482B);                     /* 'H+' */
    be16(v + 2, 4);                      /* Version 4 = HFS+ */
    be32(v + 4, 1u << 8);                /* kHFSVolumeUnmountedBit: sauber ausgehängt */
    be32(v + 8, 0x31302E30);             /* lastMountedVersion '10.0' (ohne Journal) */
    be32(v + 16, now_local);             /* createDate (Ortszeit) */
    be32(v + 20, now);                   /* modifyDate */
    be32(v + 28, now);                   /* checkedDate */
    be32(v + 40, bs);
    be32(v + 44, total);
    be32(v + 48, total - used);
    be32(v + 52, next_free);             /* nextAllocation */
    be32(v + 56, bs > 65536 ? bs : 65536);   /* rsrcClumpSize */
    be32(v + 60, bs > 65536 ? bs : 65536);   /* dataClumpSize */
    be32(v + 64, FIRST_USER_ID);         /* nextCatalogID */
    be64(v + 72, 1);                     /* encodingsBitmap: MacRoman */
    be32(v + 80 + 24, serial);           /* finderInfo[6..7]: 64-Bit-Kennung des Volumes */
    arc4random_buf(v + 80 + 28, 4);
    put_fork(v + 112, bm_bytes, (uint32_t)bm_bytes, bm_start, bm_blocks);
    put_fork(v + 192, ext_bytes, (uint32_t)ext_bytes, ext_start, ext_blocks);
    put_fork(v + 272, cat_bytes, (uint32_t)cat_bytes, cat_start, cat_blocks);

    /* Kopie 1024 Bytes vor dem Ende (der Rest der letzten 4 KiB bleibt leer), dann der Haupt-Header vorne */
    mk_report(d, "Volume-Header schreiben", 90);
    uint8_t *alt = calloc(1, 4096);
    if (!alt) { rc = mk_fail("Kein Speicher"); goto out; }
    memcpy(alt + 4096 - 1024, v, 512);
    int wr = mk_write(d, (vbytes - 4096) / ss, alt, 4096 / ss);
    free(alt);
    /* vorne nur Startblöcke + Header (1536 Bytes, auf Sektoren gerundet): Bei Blöcken unter 4 KB beginnt
     * dahinter schon die Bitmap */
    if (wr || mk_write(d, 0, vh, (1536 + ss - 1) / ss) || mk_sync(d)) goto out;
    mk_report(d, "Volume-Header schreiben", 100);

    char bt[24];
    mk_summary("HFS+ (Mac OS Extended), %s Blöcke", mk_size_text(bs, bt, sizeof(bt)));
    rc = 0;
out:
    free(bm); free(hdr); free(cat); free(vh);
    return rc;
}
