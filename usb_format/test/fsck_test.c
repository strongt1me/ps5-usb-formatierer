/* Prüft src/exfat_fsck.c: fsck_test <image> <offset> <bytes> check|repair  ->  code=<n> | <Zusammenfassung> */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/exfat_fsck.h"

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "Aufruf: %s <image> <offset> <bytes> check|repair\n", argv[0]); return 2; }
    int repair = strcmp(argv[4], "repair") == 0, fd = open(argv[1], repair ? O_RDWR : O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 2; }
    fsck_result_t r;
    char sum[256];
    for (int run = 0; run < 2; run++) {   /* zweimal hintereinander: der Zustand muss sauber zurückgesetzt werden */
        exfat_fsck_run(fd, strtoull(argv[2], NULL, 10), strtoull(argv[3], NULL, 10), 512, repair && run == 0, &r);
        exfat_fsck_summary(&r, repair && run == 0, sum, sizeof(sum));
        printf("lauf%d code=%d dirs=%ld files=%ld | %s\n", run + 1, r.code, r.dirs, r.files, sum);
        if (getenv("FSCK_LOG")) printf("%s\n", r.log);
    }
    close(fd);
    return 0;
}
