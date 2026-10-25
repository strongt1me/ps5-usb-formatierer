/* Prüft src/pkgsign.c: pkgsign_test <datei.pkg>... gibt je Datei 1 (fPKG), 0 (offiziell) oder -1 (nicht feststellbar) aus */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "../src/pkgsign.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        printf("%d\n", fd >= 0 ? pkg4_is_fake(fd) : -1);
        if (fd >= 0) close(fd);
    }
    return 0;
}
