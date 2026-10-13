/* Erkennt das Dateisystem eines Datenträgers (nur lesen): erste Partition laut MBR oder GPT, sonst ein
 * Dateisystem ohne Partitionstabelle ("Superfloppy"). Gelesen wird in ganzen 4-KiB-Blöcken ab Sektorgrenzen,
 * wie es Rohgeräte unter FreeBSD (PS5) verlangen. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsdetect.h"
#include "fsread.h"

#define BLK 4096

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* 4 KiB ab off (auf 4 KiB abgerundet gelesen; buf bekommt den Block, Rückgabe zeigt auf off darin) */
static const uint8_t *rd(int fd, uint8_t *buf, uint64_t off) {
    uint64_t start = off - off % BLK;
    if (pread(fd, buf, BLK, (off_t)start) != BLK) return NULL;
    return buf + (off - start);
}

const char *fs_detect_buf(const uint8_t *b, size_t len) {
    if (len < 512) return NULL;
    if (!memcmp(b + 3, "EXFAT   ", 8)) return "exFAT";
    if (!memcmp(b + 3, "NTFS    ", 8)) return "NTFS";
    if (!memcmp(b + 0x52, "FAT32   ", 8)) return "FAT32";
    if (!memcmp(b + 0x36, "FAT16   ", 8)) return "FAT16";
    if (!memcmp(b + 0x36, "FAT12   ", 8)) return "FAT12";
    if (len >= 2048 && b[1080] == 0x53 && b[1081] == 0xEF) {      /* ext2/3/4: Superblock ab Byte 1024 */
        uint32_t compat = le32(b + 1024 + 0x5C), incompat = le32(b + 1024 + 0x60);
        if (incompat & (0x40 | 0x80 | 0x200)) return "ext4";      /* extents, 64bit, flex_bg */
        return (compat & 0x4) ? "ext3" : "ext2";                  /* has_journal */
    }
    return NULL;
}

static int all_zero(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (b[i]) return 0;
    return 1;
}

/* Bezeichnung aus 11 Zeichen (FAT): Leerzeichen am Ende weg, "NO NAME" = keine */
static void fat_name(const uint8_t *p, char *out, size_t n) {
    size_t len = 11;
    while (len && p[len - 1] == ' ') len--;
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    if (strcmp(out, "NO NAME") == 0) out[0] = 0;
}

int fs_fat32_label_slot(int fd, uint64_t start, uint64_t *off, int *found) {
    uint8_t *buf = malloc(BLK);
    const uint8_t *b;
    int rc = -1;
    if (!buf || !(b = rd(fd, buf, start))) goto out;
    uint32_t bps = le16(b + 11), spc = b[13], rsvd = le16(b + 14), nfats = b[16], fatsz = le32(b + 36);
    uint32_t root = le32(b + 44);
    if (bps < 512 || bps > 4096 || !spc || root < 2) goto out;
    uint64_t first = start + ((uint64_t)rsvd + (uint64_t)nfats * fatsz + (uint64_t)(root - 2) * spc) * bps;
    uint64_t len = (uint64_t)spc * bps, free_off = 0;
    if (len > 65536) len = 65536;
    for (uint64_t p = 0; p < len; p += 32) {
        const uint8_t *e = rd(fd, buf, first + p);
        if (!e) goto out;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (!free_off) free_off = first + p;
            if (e[0] == 0x00) break;
            continue;
        }
        if (e[11] != 0x0F && (e[11] & 0x08) && !(e[11] & 0x10)) {
            *off = first + p;
            *found = 1;
            rc = 0;
            goto out;
        }
    }
    if (free_off) { *off = free_off; *found = 0; rc = 0; }
out:
    free(buf);
    return rc;
}

static void read_label(int fd, const char *fs, uint64_t start, char *out, size_t n) {
    uint8_t *buf = malloc(BLK);
    const uint8_t *b;
    out[0] = 0;
    if (!buf) return;
    if (!strcmp(fs, "exFAT")) {
        fsr_t *f = fsr_open(fd, start);
        if (f) { fsr_label(f, out, n); fsr_close(f); }
    } else if (!strcmp(fs, "FAT32")) {
        uint64_t off;
        int found = 0;
        if (fs_fat32_label_slot(fd, start, &off, &found) == 0 && found && (b = rd(fd, buf, off))) fat_name(b, out, n);
        else if ((b = rd(fd, buf, start))) fat_name(b + 0x47, out, n);
    } else if (!strcmp(fs, "FAT16") || !strcmp(fs, "FAT12")) {
        if ((b = rd(fd, buf, start))) fat_name(b + 0x2B, out, n);
    } else if (!strncmp(fs, "ext", 3)) {
        if ((b = rd(fd, buf, start + 1024))) {
            size_t len = strnlen((const char *)b + 0x78, 16);
            if (len >= n) len = n - 1;
            memcpy(out, b + 0x78, len);
            out[len] = 0;
        }
    }
    free(buf);
}

static const char *detect(int fd, uint32_t ss, uint64_t devsize, uint8_t *buf, fsinfo_t *o) {
    const uint8_t *b = rd(fd, buf, 0);
    o->scheme = "";
    o->start = 0;
    o->size = devsize;
    if (!b) return "unbekannt";

    const char *fs = fs_detect_buf(b, BLK);   /* ohne Partitionstabelle */
    if (fs) return fs;
    if (all_zero(b, BLK)) return "leer";
    if (b[510] != 0x55 || b[511] != 0xAA) return "unbekannt";

    const uint8_t *pe = b + 446;
    uint64_t start = 0, size = 0;
    if (pe[4] == 0xEE) {                         /* GPT: Kopf in LBA 1, erster Eintrag */
        o->scheme = "GPT";
        const uint8_t *h = rd(fd, buf, ss);
        if (!h || memcmp(h, "EFI PART", 8) != 0) return "unbekannt";
        const uint8_t *e = rd(fd, buf, le64(h + 72) * ss);
        if (!e) return "unbekannt";
        start = le64(e + 32);
        uint64_t end = le64(e + 40);
        size = start && end >= start ? (end - start + 1) * ss : 0;
    } else if (pe[4] != 0) {
        o->scheme = "MBR";
        start = le32(pe + 8);
        size = (uint64_t)le32(pe + 12) * ss;
    }
    if (!start) return "keine Partition";
    o->start = start * ss;
    o->size = size;
    if (!(b = rd(fd, buf, o->start))) return "unbekannt";
    fs = fs_detect_buf(b, BLK);
    if (fs) return fs;
    return all_zero(b, BLK) ? "leer" : "unbekannt";
}

int fs_probe(int fd, uint32_t ss, uint64_t devsize, fsinfo_t *out) {
    uint8_t *buf = malloc(BLK);   /* nicht auf den Stapel: Threads haben auf der PS5 nur 64 KiB */
    memset(out, 0, sizeof(*out));
    if (!buf) { out->fs = "unbekannt"; out->scheme = ""; return -1; }
    out->fs = detect(fd, ss ? ss : 512, devsize, buf, out);
    free(buf);
    read_label(fd, out->fs, out->start, out->label, sizeof(out->label));
    return 0;
}

const char *fs_detect(int fd, uint32_t ss) {
    fsinfo_t i;
    fs_probe(fd, ss, 0, &i);
    return i.fs;
}
