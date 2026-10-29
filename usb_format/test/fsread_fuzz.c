/* Fuzz-Test des Abbild-Lesers (src/fsread.c) unter ASAN/UBSAN: ein gültiges exFAT- bzw. UFS2-Abbild wird zufällig
 * verstümmelt (Bytes und 4-/8-Byte-Felder, meist im ersten MiB mit Bootsektor, FAT, Superblock und Inodes) und dann
 * ganz durchgegangen: Typ, Bezeichnung, param.json, fsr_walk mit fsr_fread jeder Datei. Erwartet: kein Absturz, kein
 * Sanitizer-Fund, kein Hänger (je Durchlauf höchstens 10 s). Abbilder kommen von beliebigen Sticks.
 * Aufruf: fsread_fuzz <abbild> <durchläufe> <seed>   (das Abbild wird verändert und nach jedem Durchlauf zurückgesetzt)
 * Bau/Lauf: test/fsread_fuzz.sh */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/fsread.h"

static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 11);
}

static long iter;
static void on_alarm(int sig) {
    (void)sig;
    static const char msg[] = "FEHLER  Hänger (über 10 s in einem Durchlauf)\n";
    if (write(2, msg, sizeof(msg) - 1) < 0) _exit(3);
    _exit(3);
}

typedef struct { long files; long long bytes; char *buf; } walk_t;

static int cb(void *ctx, const char *rel, fsr_file_t *h, uint64_t size) {
    walk_t *w = ctx;
    (void)size;
    if (!rel[0] || strlen(rel) >= 1024) { fprintf(stderr, "FEHLER  ungültiger Pfad\n"); exit(2); }
    if (++w->files > 20000) return 1;   /* genug gesehen (kaputte Verzeichnisse können riesig wirken) */
    for (;;) {
        long r = fsr_fread(h, w->buf, 1 << 20);
        if (r < 0) break;
        if (r == 0) break;
        if (r > 1 << 20) { fprintf(stderr, "FEHLER  fsr_fread zu viel\n"); exit(2); }
        w->bytes += r;
        if (w->bytes > (256ll << 20)) return 1;   /* riesige "Dateien" nicht ganz lesen */
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Aufruf: %s <abbild> <durchläufe> <seed>\n", argv[0]); return 2; }
    const long n = atol(argv[2]);
    rng ^= (uint64_t)atoll(argv[3]) * 0x9E3779B97F4A7C15ull;
    int fd = open(argv[1], O_RDWR);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 65536) { perror(argv[1]); return 2; }
    const uint64_t size = (uint64_t)st.st_size, hot = size < (1u << 20) ? size : (1u << 20);
    char *buf = malloc(1 << 20), *small = malloc(64 << 10);
    if (!buf || !small) return 2;
    signal(SIGALRM, on_alarm);
    long opened = 0, walked = 0, files = 0;
    for (iter = 0; iter < n; iter++) {
        enum { M = 24 };
        uint64_t off[M];
        uint8_t orig[M][8];
        int len[M], k = 1 + (int)(rnd() % 16);
        for (int i = 0; i < k; i++) {   /* verändern, Originale merken */
            const uint32_t r = rnd() % 10;
            len[i] = r < 6 ? 1 : r < 9 ? 4 : 8;
            uint64_t base = rnd() % 10 < 7 ? hot : size;
            off[i] = ((uint64_t)rnd() << 20 ^ rnd()) % (base - 8);
            if (len[i] > 1) off[i] &= ~(uint64_t)(len[i] - 1);
            if (pread(fd, orig[i], (size_t)len[i], (off_t)off[i]) != len[i]) return 2;
            uint8_t v[8];
            const uint32_t how = rnd() % 5;
            for (int b = 0; b < 8; b++) v[b] = how == 0 ? 0 : how == 1 ? 0xFF : how == 2 ? (uint8_t)(orig[i][b] ^ (1u << (rnd() % 8)))
                                         : how == 3 && b == 0 ? (uint8_t)(rnd() % 4) : (uint8_t)rnd();
            if (pwrite(fd, v, (size_t)len[i], (off_t)off[i]) != len[i]) return 2;
        }
        alarm(10);
        fsr_t *f = fsr_open(fd, 0);
        if (f) {
            char label[64];
            opened++;
            fsr_type(f);
            fsr_label(f, label, sizeof(label));
            fsr_read_file(f, "sce_sys/param.json", small, 64 << 10);
            fsr_is_dir(f, "sce_sys");
            walk_t w = {0, 0, buf};
            if (fsr_walk(f, "", cb, &w, NULL) == 0) walked++;
            files += w.files;
            fsr_walk(f, "data", cb, &w, NULL);
            fsr_close(f);
        }
        alarm(0);
        for (int i = k - 1; i >= 0; i--)   /* zurücksetzen (rückwärts: überlappende Änderungen) */
            if (pwrite(fd, orig[i], (size_t)len[i], (off_t)off[i]) != len[i]) return 2;
    }
    printf("%ld Durchläufe: %ld lesbar, %ld ganz durchgegangen, %ld Dateien gelesen - kein Absturz\n", n, opened, walked, files);
    free(buf);
    free(small);
    close(fd);
    return 0;
}
