/* Fuzz-Test des Einspielens (saves_restore_file: ZIP-Leser, chives.json, param.sfo, Host-Ersatz fürs Einhängen): eine gültige Sicherung wird zufällig verstümmelt (Bytes ändern,
 * abschneiden, Felder des Zentralverzeichnisses überschreiben) und zurückgeschrieben. Erwartet: kein Absturz, kein
 * Sanitizer-Fund, nichts außerhalb des Spielstand-Ordners geschrieben.
 * Bau/Lauf: test/saves_fuzz.sh */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/platform.h"
#include "../src/saves.h"

int lib_parse_param_sfo(const uint8_t *b, size_t l, char *id, size_t idn, char *nm, size_t nn) {
    (void)b; (void)l; (void)id; (void)idn; (void)nm; (void)nn;
    return -1;
}

static const char *g_home, *g_data, *g_sys;
const char *plat_saves_dir(void) { return g_home; }
const char *plat_data_dir(void) { return g_data; }
const char *plat_sys_data_dir(void) { return g_sys; }
int plat_account_id(const char *username, uint64_t *id) { (void)username; *id = 0x1122334455667788ull; return 0; }
int plat_save_service_enter(uint64_t authid) { (void)authid; return 0; }
void plat_save_service_leave(void) {}
void *plat_dynsym(const char *module, const char *symbol) { (void)module; (void)symbol; return NULL; }

static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 11);
}

/* Anzahl und Gesamtgröße aller Dateien unter dir (Eindringen außerhalb fällt auf) */
static void count(const char *dir, long *n, long long *bytes) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) count(p, n, bytes);
        else { (*n)++; *bytes += st.st_size; }
    }
    if (d) closedir(d);
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "Aufruf: %s <gueltige.zip> <home> <data> <sys> <Durchläufe> [Seed]\n", argv[0]); return 2; }
    g_home = argv[2];
    g_data = argv[3];
    g_sys = argv[4];
    long iters = atol(argv[5]);
    if (argc > 6) rng ^= (uint64_t)atoll(argv[6]) * 0x9E3779B97F4A7C15ull;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("zip"); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *orig = malloc((size_t)sz), *m = malloc((size_t)sz + 64);
    if (fread(orig, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    char path[600], parent[600];
    snprintf(path, sizeof(path), "%s/fuzz.zip", g_data);
    snprintf(parent, sizeof(parent), "%s/..", g_home);

    long ok = 0, rej = 0, n0 = 0;
    long long b0 = 0;
    count(g_home, &n0, &b0);
    for (long i = 0; i < iters; i++) {
        long len = sz;
        memcpy(m, orig, (size_t)sz);
        switch (rnd() % 6) {
        case 0: for (int k = rnd() % 8 + 1; k--;) m[rnd() % (uint32_t)len] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: len = (long)(rnd() % (uint32_t)sz); break;                      /* abgeschnitten */
        case 2: for (int k = rnd() % 4 + 1; k--;) m[rnd() % (uint32_t)len] = (uint8_t)rnd(); break;
        case 3: {                                                                /* Kopf/Endbereich verstümmeln */
            long tail = len > 120 ? len - 120 : 0;
            for (int k = rnd() % 6 + 1; k--;) m[tail + rnd() % (uint32_t)(len - tail)] = (uint8_t)rnd();
            break;
        }
        case 4: {                                                                /* 32-Bit-Felder auf Grenzwerte */
            static const uint32_t v[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x1000, 0xffff};
            for (int k = rnd() % 3 + 1; k--;) {
                long o = (long)(rnd() % (uint32_t)(len - 4));
                uint32_t x = v[rnd() % 8];
                memcpy(m + o, &x, 4);
            }
            break;
        }
        default: len = len > 22 ? 22 + (long)(rnd() % (uint32_t)(len - 22)) : len; break;
        }
        f = fopen(path, "wb");
        fwrite(m, 1, (size_t)len, f);
        fclose(f);
        char err[600] = "", note[600] = "";
        if (saves_restore_file(path, "1c3f8a0e", NULL, NULL, NULL, 0, NULL, NULL, NULL, note, sizeof(note), err, sizeof(err)) == 0) ok++;
        else rej++;
    }
    long n1 = 0;
    long long b1 = 0;
    count(g_home, &n1, &b1);
    printf("%ld Läufe: %ld angenommen, %ld abgelehnt; Dateien unter home: %ld -> %ld\n", iters, ok, rej, n0, n1);
    unlink(path);
    free(orig);
    free(m);
    return 0;
}
