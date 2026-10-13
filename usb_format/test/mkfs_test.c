/* Formatiert eine Image-Datei auf dem Host, zum Pruefen mit fsck.
 *   mkfs_test part|disk <image> exfat|fat32 <bytes> <sektorgroesse> [label] [cluster] [Optionen]
 * part: nur Dateisystem ab Offset 0 (Partitionsbeginn 2048 im Bootsektor)
 * disk: komplettes Layout mit Partitionstabelle wie auf der PS5
 * cluster: Zuordnungseinheit in Bytes, 0/fehlt = Standard
 * Optionen: --gpt | --mbr, --limit=<bytes>, --dir=<pfad> (mehrfach), --file=<pfad>=<lokale datei> (mehrfach) */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/mkfs.h"

#define OPT_MAX 32

static void *slurp(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    void *p = malloc(n > 0 ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(1); }
    fclose(f);
    *len = (uint32_t)n;
    return p;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "Aufruf: %s part|disk <image> exfat|fat32 <bytes> <ss> [label] [cluster] [Optionen]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1], *img = argv[2], *label = "";
    int fs = strcmp(argv[3], "exfat") == 0 ? FS_EXFAT : FS_FAT32;
    unsigned long long bytes = strtoull(argv[4], NULL, 10);
    uint32_t cluster = 0;
    const char *dirs[OPT_MAX + 1] = {0};
    mkfs_file_t files[OPT_MAX];
    mkfs_opts_t o = {0};
    int nd = 0, pos = 0;

    for (int i = 6; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--gpt") == 0) o.scheme = LAYOUT_GPT;
        else if (strcmp(a, "--mbr") == 0) o.scheme = LAYOUT_MBR;
        else if (strncmp(a, "--limit=", 8) == 0) o.limit = strtoull(a + 8, NULL, 10);
        else if (strncmp(a, "--dir=", 6) == 0 && nd < OPT_MAX) dirs[nd++] = a + 6;
        else if (strncmp(a, "--file=", 7) == 0 && o.nfiles < OPT_MAX) {
            char *spec = strdup(a + 7), *eq = strchr(spec, '=');
            if (!eq) { fprintf(stderr, "--file=<pfad>=<datei>\n"); return 2; }
            *eq = 0;
            files[o.nfiles].path = spec;
            files[o.nfiles].data = slurp(eq + 1, &files[o.nfiles].len);
            o.nfiles++;
        } else if (pos == 0) { label = a; pos++; }
        else if (pos == 1) { cluster = (uint32_t)strtoul(a, NULL, 10); pos++; }
    }
    o.dirs = dirs;
    o.files = files;

    blkdev_t d = {0};
    d.ss = (uint32_t)strtoul(argv[5], NULL, 10);
    d.sectors = bytes / d.ss;
    d.fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (d.fd < 0 || ftruncate(d.fd, (off_t)bytes)) { perror(img); return 1; }

    int rc;
    if (strcmp(mode, "disk") == 0)
        rc = format_disk(&d, fs, label, 0x1234ABCD, cluster, &o);
    else if (fs == FS_EXFAT)
        rc = mkfs_exfat(&d, 2048, d.sectors, label, 0x1234ABCD, cluster, &o);
    else
        rc = mkfs_fat32(&d, 2048, d.sectors, label, 0x1234ABCD, cluster, &o);
    close(d.fd);

    if (rc) { printf("FEHLER: %s\n", mkfs_error()); return 1; }
    printf("%s%s\n", mkfs_summary(), o.is_gpt ? ", GPT" : "");
    return 0;
}
