/* Prüft src/vollabel.c und die Bezeichnung in fs_probe:
 *   label_test set <image> <bytes> <bezeichnung>   -> ändert sie
 *   label_test get <image> <bytes>                 -> "fs=<..> schema=<..> start=<..> bezeichnung=<..>" */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/fsdetect.h"
#include "../src/vollabel.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Aufruf: %s set|get <image> <bytes> [bezeichnung]\n", argv[0]); return 2; }
    int set = strcmp(argv[1], "set") == 0, fd = open(argv[2], set ? O_RDWR : O_RDONLY);
    if (fd < 0) { perror(argv[2]); return 2; }
    uint64_t size = strtoull(argv[3], NULL, 10);
    int rc = 0;
    if (set) {
        char err[256];
        if (vol_set_label(fd, 512, size, argc > 4 ? argv[4] : "", err, sizeof(err))) { printf("FEHLER: %s\n", err); rc = 1; }
    } else {
        fsinfo_t i;
        fs_probe(fd, 512, size, &i);
        printf("fs=%s schema=%s start=%llu bezeichnung=%s\n", i.fs, i.scheme, (unsigned long long)i.start, i.label);
    }
    close(fd);
    return rc;
}
