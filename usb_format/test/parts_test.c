/* Expertenmodus auf dem Host: mehrere Partitionen anlegen und die eigenen Dateisysteme hineinschreiben.
 *   parts_test <image> <bytes> <sektorgröße> mbr|gpt|auto <fs>:<größe>[:<bezeichnung>[:<cluster>[:<typ>[:aktiv]]]] ...
 * fs: exfat | fat32 | hfsplus | ext4 | ntfs | none (ext4/ntfs/none: nur Partition, ohne Dateisystem)
 * größe: Bytes, mit K/M/G, oder "rest". typ: auto|windows|linux|swap|efi|apple
 * Ausgabe je Partition: "P <nr> <start> <anzahl>" (Sektoren), Fehler auf stderr mit Exit 1. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/mkfs.h"

static uint64_t parse_size(const char *s) {
    if (!strcmp(s, "rest")) return 0;
    char *e;
    uint64_t v = strtoull(s, &e, 10);
    if (*e == 'K') v <<= 10;
    else if (*e == 'M') v <<= 20;
    else if (*e == 'G') v <<= 30;
    return v;
}

static int parse_fs(const char *s) {
    return !strcmp(s, "exfat") ? FS_EXFAT : !strcmp(s, "fat32") ? FS_FAT32 : !strcmp(s, "hfsplus") ? FS_HFSPLUS
         : !strcmp(s, "ext4") ? FS_EXT4 : !strcmp(s, "ntfs") ? FS_NTFS : 0;
}

static int parse_type(const char *s) {
    return !strcmp(s, "windows") ? PT_WINDOWS : !strcmp(s, "linux") ? PT_LINUX : !strcmp(s, "swap") ? PT_SWAP
         : !strcmp(s, "efi") ? PT_EFI : !strcmp(s, "apple") ? PT_APPLE : PT_AUTO;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "Aufruf: %s <image> <bytes> <ss> mbr|gpt|auto <fs>:<größe>[:...] ...\n", argv[0]);
        return 2;
    }
    part_t p[MAX_PARTS + 4];
    char *label[MAX_PARTS + 4];
    uint32_t cluster[MAX_PARTS + 4];
    int n = 0;
    memset(p, 0, sizeof(p));
    for (int i = 5; i < argc && n < MAX_PARTS + 4; i++, n++) {
        char *f[6] = {0}, *s = strdup(argv[i]);
        for (int k = 0; k < 6 && s; k++) { f[k] = strsep(&s, ":"); }
        p[n].fs = parse_fs(f[0]);
        p[n].bytes = f[1] ? parse_size(f[1]) : 0;
        label[n] = f[2] ? f[2] : "";
        cluster[n] = f[3] ? (uint32_t)strtoul(f[3], NULL, 10) : 0;
        p[n].ptype = f[4] ? parse_type(f[4]) : PT_AUTO;
        p[n].active = f[5] && !strcmp(f[5], "aktiv");
        p[n].name = label[n];
    }
    const char *sch = argv[4];
    int scheme = !strcmp(sch, "mbr") ? LAYOUT_MBR : !strcmp(sch, "gpt") ? LAYOUT_GPT : LAYOUT_AUTO;

    blkdev_t d = {0};
    uint64_t bytes = strtoull(argv[2], NULL, 10);
    d.ss = (uint32_t)strtoul(argv[3], NULL, 10);
    d.sectors = bytes / d.ss;
    d.fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (d.fd < 0 || ftruncate(d.fd, (off_t)bytes)) { perror(argv[1]); return 1; }

    int gpt = 0;
    if (layout_disk(&d, scheme, 0, p, n, 0x12345678, &gpt)) {
        fprintf(stderr, "%s\n", mkfs_error());
        return 1;
    }
    for (int i = 0; i < n; i++) {
        d.base = p[i].start * d.ss;
        mkfs_progress_reset();
        int rc = 0;
        if (p[i].fs == FS_EXFAT) rc = mkfs_exfat(&d, p[i].start, p[i].count, label[i], 0x1000 + i, cluster[i], NULL);
        else if (p[i].fs == FS_FAT32) rc = mkfs_fat32(&d, p[i].start, p[i].count, label[i], 0x1000 + i, cluster[i], NULL);
        else if (p[i].fs == FS_HFSPLUS) rc = mkfs_hfsplus(&d, p[i].count, label[i], 0x1000 + i, cluster[i]);
        d.base = 0;
        if (rc) { fprintf(stderr, "Partition %d: %s\n", i + 1, mkfs_error()); return 1; }
        printf("P %d %llu %llu\n", i + 1, (unsigned long long)p[i].start, (unsigned long long)p[i].count);
    }
    printf("%s\n", gpt ? "GPT" : "MBR");
    close(d.fd);
    return 0;
}
