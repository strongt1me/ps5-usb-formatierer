/* Prüft src/fsread.c an einer Image-Datei.
 *   fsread_test <image> <offset> type            -> exFAT | UFS2
 *   fsread_test <image> <offset> isdir <pfad>    -> Exit 0, wenn Ordner
 *   fsread_test <image> <offset> cat <pfad>      -> Inhalt auf stdout
 *   fsread_test <image> <offset> label           -> Bezeichnung */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/fsread.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Aufruf: %s <image> <offset> type|isdir|cat|label [pfad]\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 2; }
    fsr_t *f = fsr_open(fd, strtoull(argv[2], NULL, 10));
    if (!f) { printf("kein exFAT/UFS2\n"); return 1; }
    const char *cmd = argv[3];
    int rc = 0;
    if (strcmp(cmd, "type") == 0) {
        printf("%s\n", fsr_type(f));
    } else if (strcmp(cmd, "isdir") == 0 && argc > 4) {
        rc = fsr_is_dir(f, argv[4]) ? 0 : 1;
    } else if (strcmp(cmd, "cat") == 0 && argc > 4) {
        static char buf[4 << 20];
        long n = fsr_read_file(f, argv[4], buf, sizeof(buf));
        if (n < 0) rc = 1;
        else fwrite(buf, 1, (size_t)n, stdout);
    } else if (strcmp(cmd, "label") == 0) {
        char l[64];
        fsr_label(f, l, sizeof(l));
        printf("%s\n", l);
    } else {
        rc = 2;
    }
    fsr_close(f);
    close(fd);
    return rc;
}
