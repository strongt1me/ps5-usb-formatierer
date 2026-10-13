/* Prüft src/library.c: library_test <wurzel> [usb] [--log=<debug.log>] [--games=<api-antwort.json>]
 * -> je Titel eine Zeile "id|name|art|pfad", dann "pkgs=<n> ordner=<a/b/c>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/library.h"

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Aufruf: %s <wurzel> [usb] [--log=..] [--games=..]\n", argv[0]); return 2; }
    lib_hint_t *h = NULL;
    int nh = 0, usb = 0;
    for (int i = 2; i < argc; i++) {
        size_t len;
        if (!strcmp(argv[i], "usb")) usb = 1;
        else if (!strncmp(argv[i], "--log=", 6)) { char *b = slurp(argv[i] + 6, &len); lib_parse_smp_log(b, len, &h, &nh); free(b); }
        else if (!strncmp(argv[i], "--games=", 8)) { char *b = slurp(argv[i] + 8, &len); lib_parse_smp_games(b, len, &h, &nh); free(b); }
    }
    lib_set_hints(h, nh);
    free(h);
    lib_root_t r;
    lib_scan(argv[1], usb, &r);
    for (int i = 0; i < r.ntitles; i++)
        printf("%s|%s|%s|%s\n", r.titles[i].id, r.titles[i].name, r.titles[i].kind, r.titles[i].path);
    printf("pkgs=%d ordner=%s\n", r.pkgs, r.folders);
    lib_free(&r);
    return 0;
}
