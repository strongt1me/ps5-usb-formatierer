/* Prüft src/imgbak.c an Image-Dateien.
 *   imgbak_test used <image> [ss]                 -> belegte Bytes
 *   imgbak_test backup <image> <ziel.img.zst> [ss] -> Sichern, Ausgabe: belegt, Dateigröße, SHA (Image, Datei)
 *   imgbak_test info <quelle.img.zst>              -> Angaben aus dem Kopf
 *   imgbak_test restore <quelle.img.zst> <image> [ss] [kaputt] -> Wiederherstellen (kaputt: Nachlesen verfälschen)
 *   imgbak_test name <modell> <label> <zeit>      -> Dateiname */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/imgbak.h"

static void prog(const char *step, int pct) { (void)step; (void)pct; }
static int out_fd(void *ctx, const void *p, size_t n) { return write(*(int *)ctx, p, n) == (ssize_t)n ? 0 : -1; }
static long in_fd(void *ctx, void *p, size_t n) { return (long)read(*(int *)ctx, p, n); }

int main(int argc, char **argv) {
    static volatile int cancel;
    char err[256] = "";
    if (argc < 3) return 2;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "name") && argc == 5) {
        ib_info_t i = {0};
        snprintf(i.model, sizeof(i.model), "%s", argv[2]);
        snprintf(i.label, sizeof(i.label), "%s", argv[3]);
        snprintf(i.created, sizeof(i.created), "%s", argv[4]);
        char n[200];
        ib_file_name(&i, n, sizeof(n));
        puts(n);
        return 0;
    }
    if (!strcmp(cmd, "used") || !strcmp(cmd, "backup")) {
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) { perror(argv[2]); return 2; }
        struct stat st;
        fstat(fd, &st);
        uint32_t ss = argc > (cmd[0] == 'u' ? 3 : 4) ? (uint32_t)atoi(argv[cmd[0] == 'u' ? 3 : 4]) : 512;
        if (cmd[0] == 'u') { printf("%llu\n", (unsigned long long)ib_used_bytes(fd, ss, (uint64_t)st.st_size)); return 0; }
        int ofd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) { perror(argv[3]); return 2; }
        ib_info_t info = {0};
        strcpy(info.model, "Test \"Stick\"");
        strcpy(info.label, "DATEN");
        strcpy(info.fs, "exFAT");
        strcpy(info.ident, "serial:ABC");
        ib_result_t r;
        if (ib_backup(fd, ss, (uint64_t)st.st_size, &info, out_fd, &ofd, prog, &cancel, &r, err, sizeof(err))) {
            fprintf(stderr, "Fehler: %s\n", err);
            return 1;
        }
        printf("%llu %llu %s %s\n", (unsigned long long)info.used, (unsigned long long)r.file_bytes, r.sha_img, r.sha_file);
        return close(ofd);
    }
    if (!strcmp(cmd, "info") || !strcmp(cmd, "restore")) {
        int ifd = open(argv[2], O_RDONLY);
        if (ifd < 0) { perror(argv[2]); return 2; }
        ib_info_t info;
        ib_reader_t *rd = ib_open(in_fd, &ifd, &info, err, sizeof(err));
        if (!rd) { fprintf(stderr, "Fehler: %s\n", err); return 1; }
        if (cmd[0] == 'i') {
            printf("size=%llu used=%llu map=%d model=%s label=%s fs=%s ident=%s created=%s\n",
                   (unsigned long long)info.size, (unsigned long long)info.used, info.has_map, info.model, info.label,
                   info.fs, info.ident, info.created);
            ib_close(rd);
            return 0;
        }
        if (argc < 4) return 2;
        int fd = open(argv[3], O_RDWR);
        if (fd < 0) { perror(argv[3]); return 2; }
        struct stat st;
        fstat(fd, &st);
        uint32_t ss = argc > 4 ? (uint32_t)atoi(argv[4]) : 512;
        int vfd = fd;
        if (argc > 5) {   /* gefälschter Datenträger: Nachlesen liefert eine andere Datei */
            vfd = open(argv[5], O_RDONLY);
            if (vfd < 0) { perror(argv[5]); return 2; }
        }
        ib_result_t r;
        int rc = ib_restore(rd, fd, vfd, ss, (uint64_t)st.st_size, prog, &cancel, &r, err, sizeof(err));
        ib_close(rd);
        if (rc) { fprintf(stderr, "Fehler: %s\n", err); return 1; }
        printf("%llu %s\n", (unsigned long long)r.used, r.sha_img);
        return 0;
    }
    return 2;
}
