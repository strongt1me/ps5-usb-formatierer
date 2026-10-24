/* Prüft src/imgbak.c an Image-Dateien.
 *   imgbak_test used <image> [ss]                 -> belegte Bytes
 *   imgbak_test backup <image> <ziel.img.zst> [ss] -> Sichern, Ausgabe: belegt, Dateigröße, SHA (Image, Datei)
 *   imgbak_test info <quelle.img.zst>              -> Angaben aus dem Kopf
 *   imgbak_test restore <quelle.img.zst> <image> [ss] [kaputt] -> Wiederherstellen (kaputt: Nachlesen verfälschen)
 *   imgbak_test name <modell> <label> <zeit>      -> Dateiname
 * IB_THREADS=n (Umgebung): Kompressions-Threads beim Sichern, 0 = ohne Hilfsthreads */
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

/* Merkpunkte im Speicher: nach "stop" gespeicherten Punkten wird abgebrochen */
typedef struct { ib_ckpt_t c; int n, stop; volatile int *cancel; int ofd; } ck_t;
static int ck_save(void *ctx, const ib_ckpt_t *c) {
    ck_t *k = ctx;
    free(k->c.frames);
    k->c = *c;
    k->c.frames = malloc((size_t)c->nframes * sizeof(ib_frame_t) + 1);
    memcpy(k->c.frames, c->frames, (size_t)c->nframes * sizeof(ib_frame_t));
    fsync(k->ofd);
    if (++k->n >= k->stop && k->stop > 0) *k->cancel = 1;
    return 0;
}

int main(int argc, char **argv) {
    static volatile int cancel;
    char err[256] = "";
    if (argc < 3) return 2;
    const char *cmd = argv[1];
    if (getenv("IB_THREADS")) ib_threads = atoi(getenv("IB_THREADS"));   /* sonst nach Kernzahl */
    if (getenv("IB_CKPT_BYTES")) { ib_ckpt_bytes = strtoull(getenv("IB_CKPT_BYTES"), NULL, 10); ib_ckpt_secs = 0; }
    if (getenv("IB_FRAME_MAX")) ib_frame_max = strtoull(getenv("IB_FRAME_MAX"), NULL, 10);   /* kleine Rahmen: viele Merkpunkte */
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
    if (!strcmp(cmd, "resume") && argc >= 5) {
        /* resume <image> <ziel.img.zst> <nach_n_merkpunkten> [gruendlich 0|1] [veraendern]: Sichern, nach n Merkpunkten abbrechen,
         * danach fortsetzen; Ausgabe wie "backup". veraendern: zwischen den Läufen ein Byte der Quelle ändern (muss erkannt werden) */
        int fd = open(argv[2], O_RDWR);
        if (fd < 0) { perror(argv[2]); return 2; }
        struct stat st;
        fstat(fd, &st);
        int ofd = open(argv[3], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) { perror(argv[3]); return 2; }
        ib_info_t info = {0};
        strcpy(info.model, "Test \"Stick\"");
        strcpy(info.label, "DATEN");
        strcpy(info.fs, "exFAT");
        strcpy(info.ident, "serial:ABC");
        strcpy(info.created, "2026-01-01 00:00");
        ck_t k;
        memset(&k, 0, sizeof(k));
        k.stop = atoi(argv[4]);
        k.cancel = &cancel;
        k.ofd = ofd;
        ib_resume_t rs1 = {ck_save, &k, NULL, 0};
        ib_result_t r;
        if (ib_backup_ex(fd, 512, (uint64_t)st.st_size, &info, out_fd, &ofd, prog, &cancel, &r, err, sizeof(err), &rs1)) {
            fprintf(stderr, "Fehler 1: %s\n", err);
            return 1;
        }
        if (!r.cancelled) { fprintf(stderr, "nicht unterbrochen (zu wenige Merkpunkte: %d)\n", k.n); return 3; }
        fprintf(stderr, "unterbrochen nach %d Merkpunkten bei %llu, Datei %llu Bytes\n", k.n, (unsigned long long)k.c.seg,
                (unsigned long long)k.c.file_bytes);
        int thorough = argc > 5 && !strcmp(argv[5], "1");
        if (argc > 6) {   /* Quelle verändern: ein Byte im schon gesicherten Teil */
            uint8_t b;
            off_t at = 1024 * 1024 + 5;   /* Beginn der Partition: belegt, im ersten Rahmen nach der MBR */
            if (pread(fd, &b, 1, at) == 1) { b ^= 0xFF; if (pwrite(fd, &b, 1, at) != 1) return 2; }
        }
        if (ftruncate(ofd, (off_t)k.c.file_bytes) != 0 || lseek(ofd, (off_t)k.c.file_bytes, SEEK_SET) < 0) return 2;
        cancel = 0;
        k.stop = 0;
        ib_resume_t rs2 = {ck_save, &k, &k.c, thorough};
        ib_info_t info2 = info;
        if (ib_backup_ex(fd, 512, (uint64_t)st.st_size, &info2, out_fd, &ofd, prog, &cancel, &r, err, sizeof(err), &rs2)) {
            fprintf(stderr, "Fehler 2: %s\n", err);
            return 1;
        }
        printf("%llu %llu %s %s\n", (unsigned long long)info2.used, (unsigned long long)r.file_bytes, r.sha_img, r.sha_file);
        return close(ofd);
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
        if (getenv("IB_CREATED")) snprintf(info.created, sizeof(info.created), "%s", getenv("IB_CREATED"));
        ib_result_t r;
        if (ib_backup(fd, ss, (uint64_t)st.st_size, &info, out_fd, &ofd, prog, &cancel, &r, err, sizeof(err))) {
            fprintf(stderr, "Fehler: %s\n", err);
            return 1;
        }
        printf("%llu %llu %s %s\n", (unsigned long long)info.used, (unsigned long long)r.file_bytes, r.sha_img, r.sha_file);
        return close(ofd);
    }
    if (!strcmp(cmd, "info") || !strcmp(cmd, "restore") || !strcmp(cmd, "verify")) {
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
        if (cmd[0] == 'v') {   /* verify <img.zst>: geprüfte Bytes, Prüfsumme verglichen, SHA der Daten und der Datei */
            ib_result_t r;
            int rc = ib_verify(rd, prog, &cancel, &r, err, sizeof(err));
            ib_close(rd);
            if (rc) { fprintf(stderr, "Fehler: %s\n", err); return 1; }
            printf("%llu %d %s %s %llu\n", (unsigned long long)r.used, r.checked, r.sha_img, r.sha_file,
                   (unsigned long long)r.file_bytes);
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
