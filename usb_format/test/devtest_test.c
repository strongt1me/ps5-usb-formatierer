/* Prüft src/devtest.c an einer Image-Datei (mit -DUSBFMT_HOST; gefälschte Sticks über USBFMT_FAKE_REAL/_MODE).
 *   devtest_test read|verify|full <image> <bytes>
 * Ausgabe: read=<MB/s> write=<MB/s> fake=<0|1> real=<Bytes> bad=<0|1> bad_at=<Bytes> */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/devtest.h"

static void quiet(const char *step, int pct) { (void)step; (void)pct; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Aufruf: %s read|verify|full <image> <bytes>\n", argv[0]); return 2; }
    int fd = open(argv[2], strcmp(argv[1], "read") == 0 ? O_RDONLY : O_RDWR);
    if (fd < 0) { perror(argv[2]); return 2; }
    uint64_t size = strtoull(argv[3], NULL, 10);
    volatile int cancel = 0;
    devtest_t r;
    char err[256] = "";
    int rc = !strcmp(argv[1], "read")   ? dt_read_speed(fd, 512, size, quiet, &cancel, &r, err, sizeof(err))
           : !strcmp(argv[1], "verify") ? dt_verify(fd, 512, size, quiet, &cancel, &r, err, sizeof(err))
                                        : dt_full(fd, 512, size, quiet, &cancel, &r, err, sizeof(err));
    close(fd);
    if (rc) { printf("FEHLER: %s\n", err); return 1; }
    printf("read=%.0f write=%.0f fake=%d real=%llu bad=%d bad_at=%llu\n", r.read_mbps, r.write_mbps, r.fake,
           (unsigned long long)r.real_bytes, r.bad, (unsigned long long)r.bad_at);
    return 0;
}
