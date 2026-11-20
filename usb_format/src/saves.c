/*
 * PS4/PS5-Spielstände sichern und einspielen (saves.h) - das Verfahren des PS5 Cooling Center (savexport.c, saveimport.c),
 * übernommen für den Formatierer: Auslesen über das Einhängen einer Kopie, Einspielen in ein neues Abbild der Konsole mit
 * Besitzer und Datenbankzeile.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "chives.h"
#include "i18n.h"
#include "library.h"
#include "platform.h"
#include "savedb.h"
#include "savemnt.h"
#include "saves.h"
#include "savesfo.h"
#include "savezip.h"

#define SAVE_SUBDIR     "savedata_prospero"
#define SAVE_SUBDIR_PS4 "savedata"
#define IMG_MIN         0x1000ull
#define SAVES_MAX       4096
#define FREE_MARGIN     (1ull << 20)
#define MIN_FREE        (64ull << 20)      /* Reserve auf der internen Platte */
#define BUF             (1u << 20)
#define SX_MAX_ENT      5000u
#define SX_REL_MAX      200u
#define SX_DEPTH        8
#define SX_ZIP_MAX      (3500ull << 20)    /* eine ZIP ohne ZIP64 endet bei 4 GiB: deutlich darunter bleiben */
#define SI_MAX_FILES    5000u
#define SI_MAX_SFO      0x8000u
#define SI_MEM_MAX      (64u << 20)        /* eine Deflate-Datei wird bis zu dieser Größe im Speicher entpackt */
#define DONOR_MAX       (256ull << 20)     /* ein Besitzer-Spender wird kopiert: größere Abbilder nicht */
#define ZIP_MAXSAVES    64
#define SP              1100               /* Pfadpuffer */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Alles mit den Spielständen läuft nacheinander: ein Arbeitsordner, ein Einhängeordner, eine Kennung des Dienstes */
static pthread_mutex_t g_op = PTHREAD_MUTEX_INITIALIZER;

static int op_enter(char *err, size_t errsz) {
    if (pthread_mutex_trylock(&g_op) == 0) return 0;
    snprintf(err, errsz, "%s", T("Es läuft bereits ein Spielstand-Vorgang"));
    return -1;
}
static void op_leave(void) { pthread_mutex_unlock(&g_op); }

/* ── Hilfen ── */

static void scpy(char *dst, size_t n, const char *src) {
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

static int path_fmt(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int path_fmt(char *out, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) { out[0] = 0; return -1; }
    return 0;
}

static int efail(char *err, size_t errsz, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int efail(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
    return -1;
}

int saves_name_ok(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= max || s[0] == '.') return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

/* Name hinter "sdimg_": Buchstaben, Ziffern, "_", "-", "."; nie mit Punkt vorn und nie eine Systemkopie (sce_bu_) */
static int slot_ok(const char *s) {
    const size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 64 || s[0] == '.' || !strncmp(s, "sce_bu_", 7)) return 0;
    /* nie die Dateien eines abgebrochenen Einspielens (sdimg_<Name>.usbfmt-alt / -neu) */
    if (n > 11 && (!strcmp(s + n - 11, ".usbfmt-alt") || !strcmp(s + n - 11, ".usbfmt-neu"))) return 0;
    if (n > 10 && (!strcmp(s + n - 10, ".ps5cc-alt") || !strcmp(s + n - 10, ".ps5cc-neu"))) return 0;   /* ebenso die des Cooling Center */
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int tid_kind_ok(const char *s, const char *pre) {
    if (!s || strlen(s) != 9 || strncmp(s, pre, 4)) return 0;
    for (int i = 4; i < 9; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}
static int ppsa_ok(const char *s) { return tid_kind_ok(s, "PPSA"); }
static int cusa_ok(const char *s) { return tid_kind_ok(s, "CUSA"); }

static int uid_ok(const char *s) {
    if (!s || strlen(s) != 8) return 0;
    for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static uint64_t le_num(const uint8_t *v, size_t n) {
    uint64_t x = 0;
    for (size_t i = n > 8 ? 8 : n; i > 0; i--) x = (x << 8) | v[i - 1];
    return x;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { if (w == 0) errno = EIO; return -1; }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static int free_space(const char *dir, uint64_t *avail) {
    struct statvfs sv;
    if (statvfs(dir, &sv) != 0) return -1;
    *avail = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
    return 0;
}

const char *saves_workdir(void) {
    static char dir[300];
    const char *d = plat_data_dir();
    snprintf(dir, sizeof(dir), "%s/save_tmp", d ? d : "/tmp");
    if (d) mkdir(d, 0755);
    mkdir(dir, 0755);
    return dir;
}

static void mkdirs(const char *path) {
    char p[SP];
    scpy(p, sizeof(p), path);
    for (char *c = p + 1; *c; c++) {
        if (*c != '/') continue;
        *c = 0;
        mkdir(p, 0777);
        *c = '/';
    }
    mkdir(p, 0777);
}

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[SP];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return ENAMETOOLONG;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return errno ? errno : EIO;
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return errno ? errno : EIO;
    return 0;
}

/* mkdir -p; ein neuer Ordner bekommt die Rechte des Ordners, in dem er entsteht (wie die Konsole selbst) */
static int mkdir_like_parent(const char *path) {
    char tmp[SP];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return ENAMETOOLONG;
    for (char *p = tmp + 1;; p++) {
        if (*p != '/' && *p != 0) continue;
        const char save = *p;
        *p = 0;
        struct stat st;
        if (stat(tmp, &st) != 0) {
            char parent[SP];
            snprintf(parent, sizeof(parent), "%s", tmp);
            char *sl = strrchr(parent, '/');
            mode_t mode = 0755;
            struct stat ps;
            if (sl && sl != parent) { *sl = 0; if (stat(parent, &ps) == 0) mode = ps.st_mode & 0777; }
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return errno ? errno : EIO;
            (void)chmod(tmp, mode);
        }
        *p = save;
        if (!save) break;
    }
    return 0;
}

/* Ordner, den dieser Code angelegt hat, mit Inhalt löschen: Verknüpfungen werden entfernt, nie betreten */
static void rm_tree(char *path, size_t len, int depth) {
    if (depth > SX_DEPTH + 4) return;
    for (int pass = 0; pass < 3; pass++) {
        DIR *d = opendir(path);
        if (!d) break;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            const size_t n = strlen(e->d_name);
            if (len + 1 + n >= SP) continue;
            path[len] = '/';
            memcpy(path + len + 1, e->d_name, n + 1);
            struct stat st;
            if (lstat(path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) rm_tree(path, len + 1 + n, depth + 1);
                else unlink(path);
            }
            path[len] = 0;
        }
        closedir(d);
        if (rmdir(path) == 0) return;
    }
}

static void remove_tree(const char *dir) {
    char p[SP];
    if (snprintf(p, sizeof(p), "%s", dir) >= (int)sizeof(p)) return;
    rm_tree(p, strlen(p), 0);
}

/* ── Fortschritt: Abschnitte eines Vorgangs auf done/total des Aufrufers abgebildet ── */

typedef struct {
    saves_prog_fn fn;
    void    *ctx;
    uint64_t scale;          /* total des Aufrufers */
    unsigned a, b;           /* aktueller Abschnitt: von a bis b Promille */
    uint64_t ptotal, pdone;
    int      stop;
} prog_t;

static int prog_report(prog_t *p) {
    if (!p || !p->fn) return 0;
    uint64_t done = p->ptotal ? (p->pdone < p->ptotal ? p->pdone : p->ptotal) : 0;
    uint64_t pm = p->a + (p->ptotal ? (uint64_t)(p->b - p->a) * done / p->ptotal : 0);
    if (p->fn(p->ctx, pm * p->scale / 1000, p->scale)) p->stop = 1;
    return p->stop;
}
static void prog_phase(prog_t *p, unsigned a, unsigned b, uint64_t total) {
    if (!p) return;
    p->a = a;
    p->b = b;
    p->ptotal = total;
    p->pdone = 0;
    prog_report(p);
}
static int prog_work(prog_t *p, uint64_t n) {
    if (!p) return 0;
    p->pdone += n;
    return prog_report(p);
}
static int prog_stopped(prog_t *p) { return p && (p->stop || prog_report(p)); }
static int prog_tick(void *ctx, uint64_t n) { return prog_work(ctx, n); }

/* src in eine NEUE Datei dst kopieren (nie überschreiben); expect != -1: die Größe muss stimmen (sonst EAGAIN). 0 oder errno */
static int copy_file(prog_t *p, const char *src, const char *dst, uint64_t expect, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return errno ? errno : EIO;
    unlink(dst);
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) { int e = errno ? errno : EIO; close(in); return e; }
    uint8_t *buf = malloc(BUF);
    if (!buf) { close(in); close(out); unlink(dst); return ENOMEM; }
    uint64_t done = 0;
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, BUF);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = errno ? errno : EIO; break; }
        if (n == 0) break;
        if (write_all(out, buf, (size_t)n) != 0) { rc = errno ? errno : EIO; break; }
        done += (uint64_t)n;
        if (prog_work(p, (uint64_t)n)) { rc = ECANCELED; break; }
    }
    free(buf);
    close(in);
    if (rc == 0 && fsync(out) != 0) rc = errno ? errno : EIO;
    if (close(out) != 0 && rc == 0) rc = errno ? errno : EIO;
    if (rc == 0 && expect != (uint64_t)-1 && done != expect) rc = EAGAIN;
    if (rc == 0) (void)chmod(dst, mode);
    if (rc != 0) unlink(dst);
    return rc;
}

/* bis zu len Byte lesen, bis zum Dateiende; die Zahl, oder -1 */
static ssize_t read_upto(int fd, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        const ssize_t n = read(fd, buf + got, len - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* sind die beiden Dateien Byte für Byte gleich? */
static int same_file(const char *x, const char *y) {
    const int fx = open(x, O_RDONLY), fy = open(y, O_RDONLY);
    uint8_t *bx = malloc(2 * (size_t)BUF);
    int same = fx >= 0 && fy >= 0 && bx != NULL;
    while (same) {
        const ssize_t nx = read_upto(fx, bx, BUF), ny = read_upto(fy, bx + BUF, BUF);
        if (nx < 0 || nx != ny || memcmp(bx, bx + BUF, (size_t)nx) != 0) same = 0;
        else if (nx < (ssize_t)BUF) break;
    }
    free(bx);
    if (fx >= 0) close(fx);
    if (fy >= 0) close(fy);
    return same;
}

/* ── Benutzer ── */

static int read_username(const char *home, const char *uid, char *out, size_t n) {
    char p[640];
    out[0] = 0;
    if (path_fmt(p, sizeof(p), "%s/%s/username.dat", home, uid)) return -1;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) { out[0] = 0; return -1; }
    out[r] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return out[0] ? 0 : -1;
}

static int cmp_users(const void *a, const void *b) {
    return strcmp(((const save_user_t *)a)->uid, ((const save_user_t *)b)->uid);
}

int saves_scan_users(save_user_t **out, int *count) {
    *out = NULL;
    *count = 0;
    const char *home = plat_saves_dir();
    DIR *d = home ? opendir(home) : NULL;
    if (!d) return -1;
    save_user_t *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char name[64];
        if (!saves_name_ok(e->d_name, sizeof(((save_user_t *)0)->uid))) continue;
        if (read_username(home, e->d_name, name, sizeof(name)) < 0) continue;
        if (n == cap) {
            save_user_t *p = realloc(arr, (size_t)(cap = cap ? cap * 2 : 8) * sizeof(*p));
            if (!p) { free(arr); closedir(d); return -1; }
            arr = p;
        }
        save_user_t *u = &arr[n++];
        memset(u, 0, sizeof(*u));
        scpy(u->uid, sizeof(u->uid), e->d_name);
        scpy(u->username, sizeof(u->username), name);
    }
    closedir(d);
    if (n > 1) qsort(arr, (size_t)n, sizeof(*arr), cmp_users);
    *out = arr;
    *count = n;
    return 0;
}

/* ── Spielnamen ── */

static int utf8_enc(unsigned cp, char u[4]) {
    if (cp < 0x80) { u[0] = (char)cp; return 1; }
    if (cp < 0x800) { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* JSON-Zeichenkette ab dem Zeichen nach dem öffnenden " lesen (\" \\ \/ \n \t \uXXXX, auch Ersatzpaare) -> UTF-8 */
static int json_str_utf8(const char *p, char *out, size_t n) {
    size_t i = 0;
    while (*p && *p != '"') {
        unsigned cp;
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) &&
                isxdigit((unsigned char)p[4])) {
                char h[5] = {p[1], p[2], p[3], p[4], 0};
                cp = (unsigned)strtoul(h, NULL, 16);
                p += 5;
                if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u' && isxdigit((unsigned char)p[2]) &&
                    isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5])) {
                    char l[5] = {p[2], p[3], p[4], p[5], 0};
                    unsigned lo = (unsigned)strtoul(l, NULL, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
            } else {
                cp = *p == 'n' ? '\n' : *p == 't' ? '\t' : (unsigned char)*p;
                p++;
            }
        } else {
            cp = (unsigned char)*p++;
            if (cp >= 0x80) {
                if (i + 1 < n) out[i++] = (char)cp;
                continue;
            }
        }
        char u[4];
        int ul = utf8_enc(cp, u);
        if (i + (size_t)ul >= n) break;
        memcpy(out + i, u, (size_t)ul);
        i += (size_t)ul;
    }
    out[i] = 0;
    return i ? 0 : -1;
}

/* Spielname aus param.json: localizedParameters -> <defaultLanguage> -> titleName (sonst der erste titleName) */
static int title_from_json(const char *j, char *out, size_t n) {
    const char *lp = strstr(j, "\"localizedParameters\""), *q = NULL;
    if (lp) {
        const char *dl = strstr(lp, "\"defaultLanguage\"");
        if (dl && (dl = strchr(dl + 17, ':')) && (dl = strchr(dl, '"'))) {
            char lang[24];
            const char *e = strchr(dl + 1, '"');
            if (e && (size_t)(e - dl - 1) < sizeof(lang)) {
                memcpy(lang, dl + 1, (size_t)(e - dl - 1));
                lang[e - dl - 1] = 0;
                char key[40];
                snprintf(key, sizeof(key), "\"%s\"", lang);
                const char *k = strstr(e, key);
                if (k) q = strstr(k, "\"titleName\"");
            }
        }
        if (!q) q = strstr(lp, "\"titleName\"");
    }
    if (!q) q = strstr(j, "\"titleName\"");
    if (!q || !(q = strchr(q + 11, ':')) || !(q = strchr(q, '"'))) return -1;
    return json_str_utf8(q + 1, out, n);
}

/* Spielname des installierten Titels: <Ordner über /user/home>/appmeta/<id>/param.json oder param.sfo, sonst
 * /system_data/priv/appmeta */
static void title_name_of(const char *home, const char *title_id, char *name, size_t n) {
    char meta[700], sys[] = "/system_data/priv/appmeta";
    scpy(meta, sizeof(meta) - 8, home);
    size_t l = strlen(meta);
    while (l > 1 && meta[l - 1] == '/') meta[--l] = 0;
    char *sl = strrchr(meta, '/');
    if (sl) *sl = 0;   /* "/user/home" -> "/user" */
    snprintf(meta + strlen(meta), sizeof(meta) - strlen(meta), "/appmeta");
    const char *dirs[2] = {meta, sys};
    name[0] = 0;
    for (int i = 0; i < 2; i++) {
        char p[900];
        uint8_t *buf = malloc(65537);
        if (!buf) return;
        if (!path_fmt(p, sizeof(p), "%s/%s/param.json", dirs[i], title_id)) {
            int fd = open(p, O_RDONLY);
            ssize_t r = fd >= 0 ? read(fd, buf, 65536) : -1;
            if (fd >= 0) close(fd);
            if (r > 0) {
                buf[r] = 0;
                if (title_from_json((const char *)buf, name, n) == 0) { free(buf); return; }
            }
        }
        if (!path_fmt(p, sizeof(p), "%s/%s/param.sfo", dirs[i], title_id)) {
            int fd = open(p, O_RDONLY);
            ssize_t r = fd >= 0 ? read(fd, buf, 16384) : -1;
            if (fd >= 0) close(fd);
            char tid[32] = "";
            if (r > 0 && lib_parse_param_sfo(buf, (size_t)r, tid, sizeof(tid), name, n) == 0 && name[0]) { free(buf); return; }
        }
        free(buf);
        name[0] = 0;
    }
}

/* ── Apps erkennen (wie das Cooling Center, title_kind) ── */

int saves_title_is_app(const char *id) {
    static char memo_id[512][16];
    static char memo_kind[512];   /* 'g' oder 'a' */
    static int nmemo;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    if (!id || !id[0]) return 0;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nmemo; i++)
        if (!strcmp(memo_id[i], id)) { const int r = memo_kind[i] == 'a'; pthread_mutex_unlock(&lk); return r; }
    pthread_mutex_unlock(&lk);
    char kind = 0;
    if (!strncmp(id, "NPXS", 4)) kind = 'a';   /* Systemanwendungen */
    /* Beschreibungen der installierten Titel: <Ordner über /user/home>/appmeta, /system_data/priv/appmeta, erweiterte Speicher */
    char roots[4][300];
    int nr = 0;
    const char *home = plat_saves_dir(), *sys = plat_sys_data_dir();
    if (home) {
        scpy(roots[nr], sizeof(roots[nr]) - 10, home);
        char *sl = strrchr(roots[nr], '/');
        if (sl && sl != roots[nr]) { *sl = 0; snprintf(sl, sizeof(roots[nr]) - (size_t)(sl - roots[nr]), "/appmeta"); nr++; }
    }
    if (sys && !path_fmt(roots[nr], sizeof(roots[nr]), "%s/priv/appmeta", sys)) nr++;
#ifndef USBFMT_HOST
    scpy(roots[nr++], sizeof(roots[0]), "/mnt/ext0/user/appmeta");
    scpy(roots[nr++], sizeof(roots[0]), "/mnt/ext1/user/appmeta");
#endif
    for (int r = 0; r < nr && !kind; r++) {
        for (int k = 0; k < 2 && !kind; k++) {
            char path[600];
            if (path_fmt(path, sizeof(path), "%s/%s/%s", roots[r], id, k == 0 ? "param.json" : "param.sfo")) continue;
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            char *buf = malloc(96 * 1024);
            ssize_t n = buf ? read(fd, buf, 96 * 1024 - 1) : -1;
            close(fd);
            if (buf && n > 0) {
                buf[n] = 0;
                if (k == 0) {
                    cJSON *j = cJSON_Parse(buf);
                    cJSON *ct = j ? cJSON_GetObjectItem(j, "applicationCategoryType") : NULL;
                    if (cJSON_IsNumber(ct)) kind = ct->valuedouble == 0 ? 'g' : 'a';
                    cJSON_Delete(j);
                } else {
                    char cat[16];
                    if (savesfo_get_str((const uint8_t *)buf, (size_t)n, "CATEGORY", cat, sizeof(cat)) > 0) kind = cat[0] == 'g' ? 'g' : 'a';
                }
            }
            free(buf);
        }
    }
    if (!kind) kind = 'g';   /* ohne Beschreibung: ein Spiel, wie im Cooling Center */
    pthread_mutex_lock(&lk);
    if (nmemo < 512) { scpy(memo_id[nmemo], sizeof(memo_id[0]), id); memo_kind[nmemo++] = kind; }
    pthread_mutex_unlock(&lk);
    return kind == 'a';
}

static int (*g_game_check)(char *id, size_t n);
void saves_set_game_check(int (*fn)(char *id, size_t n)) { g_game_check = fn; }

/* ── Spielstände auflisten ── */

static int cmp_saves(const void *a, const void *b) {
    return strcmp(((const save_info_t *)a)->key, ((const save_info_t *)b)->key);
}

int saves_scan(save_info_t **out, int *count) {
    *out = NULL;
    *count = 0;
    const char *home = plat_saves_dir();
    DIR *hd = home ? opendir(home) : NULL;
    if (!hd) return -1;
    save_info_t *arr = NULL;
    int n = 0, cap = 0, full = 0;
    char last_tid[32] = "", last_name[256] = "";
    struct dirent *ue;
    while (!full && (ue = readdir(hd))) {
        if (!saves_name_ok(ue->d_name, sizeof(arr->uid))) continue;
        char username[64] = "", pdir[640];
        read_username(home, ue->d_name, username, sizeof(username));
        for (int pass = 0; pass < 2 && !full; pass++) {   /* 0: PS5 (savedata_prospero), 1: PS4 (savedata) */
            const int ps4 = pass == 1;
            if (path_fmt(pdir, sizeof(pdir), "%s/%s/%s", home, ue->d_name, ps4 ? SAVE_SUBDIR_PS4 : SAVE_SUBDIR)) continue;
            DIR *pd = opendir(pdir);
            if (!pd) continue;
            struct dirent *te;
            while (!full && (te = readdir(pd))) {
                char tdir[900];
                if (!saves_name_ok(te->d_name, sizeof(arr->title_id)) || path_fmt(tdir, sizeof(tdir), "%s/%s", pdir, te->d_name) ||
                    saves_title_is_app(te->d_name))   /* Apps bleiben unberührt */
                    continue;
                DIR *sd = opendir(tdir);
                if (!sd) continue;
                struct dirent *se;
                while ((se = readdir(sd))) {
                    size_t nl = strlen(se->d_name);
                    char fpath[1200];
                    struct stat st;
                    if (!saves_name_ok(se->d_name, sizeof(arr->save_dir))) continue;
                    if (strncmp(se->d_name, "sdimg_", 6) || !slot_ok(se->d_name + 6)) continue;   /* nur Abbilder, keine Systemkopien */
                    char kpath[1200] = "";
                    if (ps4) {   /* PS4: nur sdimg_<Name> mit <Name>.bin (96 Byte) daneben */
                        struct stat ks;
                        if (nl <= 6 || path_fmt(kpath, sizeof(kpath), "%s/%s.bin", tdir, se->d_name + 6) ||
                            stat(kpath, &ks) != 0 || !S_ISREG(ks.st_mode) || ks.st_size != 96)
                            continue;
                    }
                    if (path_fmt(fpath, sizeof(fpath), "%s/%s", tdir, se->d_name) || stat(fpath, &st) != 0 ||
                        !S_ISREG(st.st_mode) || (uint64_t)st.st_size < IMG_MIN)
                        continue;
                    if (n >= SAVES_MAX) { full = 1; break; }
                    if (n == cap) {
                        save_info_t *p = realloc(arr, (size_t)(cap = cap ? cap * 2 : 32) * sizeof(*p));
                        if (!p) { free(arr); closedir(sd); closedir(pd); closedir(hd); return -1; }
                        arr = p;
                    }
                    save_info_t *s = &arr[n++];
                    memset(s, 0, sizeof(*s));
                    if (path_fmt(s->path, sizeof(s->path), "%s", fpath)) { n--; continue; }
                    scpy(s->title_id, sizeof(s->title_id), te->d_name);
                    scpy(s->save_dir, sizeof(s->save_dir), se->d_name);
                    scpy(s->uid, sizeof(s->uid), ue->d_name);
                    scpy(s->username, sizeof(s->username), username);
                    snprintf(s->key, sizeof(s->key), "%s/%s/%s%s", s->uid, s->title_id, s->save_dir, ps4 ? "#PS4" : "");
                    s->size = (uint64_t)st.st_size;
                    s->mtime = st.st_mtime;
                    scpy(s->platform, sizeof(s->platform), ps4 ? "PS4" : "PS5");
                    if (ps4) scpy(s->key_path, sizeof(s->key_path), kpath);
                    if (strcmp(last_tid, s->title_id) != 0) {
                        title_name_of(home, s->title_id, last_name, sizeof(last_name));
                        scpy(last_tid, sizeof(last_tid), s->title_id);
                    }
                    scpy(s->title_name, sizeof(s->title_name), last_name[0] ? last_name : s->title_id);
                }
                closedir(sd);
            }
            closedir(pd);
        }
    }
    closedir(hd);
    if (n > 1) qsort(arr, (size_t)n, sizeof(*arr), cmp_saves);
    *out = arr;
    *count = n;
    return 0;
}

int saves_find(const char *key, save_info_t *out) {
    save_info_t *all = NULL;
    int n = 0, found = -1;
    if (!key || !key[0] || saves_scan(&all, &n) < 0) return -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i].key, key)) { *out = all[i]; found = 0; break; }
    free(all);
    return found;
}

/* ── Arbeitsordner aufräumen: Reste eines abgebrochenen Laufs. Nie unter einem Einhängeordner löschen. ── */

static int work_clean(const char *work, char *err, size_t errsz) {
    char mnt[SP], p[SP];
    static const char *const mnts[] = {"mnt", "donor_mnt"};
    for (int i = 0; i < 2; i++) {
        if (path_fmt(mnt, sizeof(mnt), "%s/%s", work, mnts[i])) continue;
        if (rmdir(mnt) != 0 && errno != ENOENT) {
            char e2[300];
            if (savemnt_umount_path(mnt, e2, sizeof(e2)) != 0)
                return efail(err, errsz, T("Von einem früheren Versuch ist noch ein Spielstand eingehängt, und die Konsole lässt ihn nicht los. %s Die PS5 neu starten, dann geht es wieder."), e2);
            if (rmdir(mnt) != 0 && errno != ENOENT)
                return efail(err, errsz, T("Im Arbeitsordner liegt noch ein Rest eines früheren Versuchs (%s). Die PS5 neu starten, dann geht es wieder."), strerror(errno));
        }
    }
    static const char *const dirs[] = {"", "/donor"};
    for (int i = 0; i < 2; i++) {
        char d[SP];
        if (path_fmt(d, sizeof(d), "%s%s", work, dirs[i])) continue;
        DIR *wd = opendir(d);
        if (!wd) continue;
        struct dirent *we;
        while ((we = readdir(wd)) != NULL) {
            struct stat sw;
            if (strncmp(we->d_name, "sdimg_", 6) || path_fmt(p, sizeof(p), "%s/%s", d, we->d_name)) continue;
            if (lstat(p, &sw) == 0 && S_ISREG(sw.st_mode)) unlink(p);
        }
        closedir(wd);
    }
    if (!path_fmt(p, sizeof(p), "%s/donor", work)) rmdir(p);
    if (!path_fmt(p, sizeof(p), "%s/stage", work)) remove_tree(p);
    if (!path_fmt(p, sizeof(p), "%s/icon0.png", work)) unlink(p);
    if (!path_fmt(p, sizeof(p), "%s/key.bin", work)) unlink(p);
    return 0;
}

/* ════════════════════════ Auslesen (wie savexport.c) ════════════════════════ */

typedef struct {
    char     rel[SX_REL_MAX];
    uint64_t size;
    uint32_t uid, gid;
    uint16_t mode;
    time_t   mtime;
    int      dir;
} ent_t;

typedef struct {
    ent_t   *v;
    unsigned n, cap;
    uint64_t bytes;
    unsigned skipped;
} list_t;

static int list_add(list_t *l, const char *rel, const struct stat *st, int dir) {
    if (l->n >= SX_MAX_ENT) return E2BIG;
    if (l->n == l->cap) {
        const unsigned cap = l->cap ? l->cap * 2 : 128;
        ent_t *nv = realloc(l->v, (size_t)cap * sizeof(*nv));
        if (!nv) return ENOMEM;
        l->v = nv;
        l->cap = cap;
    }
    ent_t *e = &l->v[l->n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->rel, sizeof(e->rel), "%s", rel);
    e->size = dir ? 0 : (uint64_t)st->st_size;
    e->uid = (uint32_t)st->st_uid;
    e->gid = (uint32_t)st->st_gid;
    e->mode = (uint16_t)(st->st_mode & 07777);
    e->mtime = st->st_mtime;
    e->dir = dir;
    if (!dir) l->bytes += e->size;
    return 0;
}

/* root/rel durchgehen. 0 oder errno; bad = der Name, der nicht mitgenommen werden kann */
static int walk(const char *root, const char *rel, int depth, list_t *l, char *bad, size_t bad_len) {
    if (depth > SX_DEPTH) { snprintf(bad, bad_len, "%s", rel); return ENAMETOOLONG; }
    char dir[SP];
    if (snprintf(dir, sizeof(dir), rel[0] ? "%s/%s" : "%s", root, rel) >= (int)sizeof(dir)) return ENAMETOOLONG;
    DIR *d = opendir(dir);
    if (!d) { snprintf(bad, bad_len, "%s", rel[0] ? rel : "/"); return errno ? errno : EIO; }
    int rc = 0;
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[SX_REL_MAX + 8];
        if (snprintf(child, sizeof(child), rel[0] ? "%s/%s" : "%s%s", rel, e->d_name) >= (int)SX_REL_MAX) {
            snprintf(bad, bad_len, "%.180s...", child);
            rc = ENAMETOOLONG;
            break;
        }
        char full[SP];
        if (snprintf(full, sizeof(full), "%s/%s", root, child) >= (int)sizeof(full)) { rc = ENAMETOOLONG; break; }
        struct stat st;
        if (lstat(full, &st) != 0) { snprintf(bad, bad_len, "%s", child); rc = errno ? errno : EIO; break; }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { l->skipped++; continue; }
        if (!chives_path_safe(child) || !savezip_name_ok(child)) { snprintf(bad, bad_len, "%s", child); rc = EINVAL; break; }
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size > SX_ZIP_MAX) { snprintf(bad, bad_len, "%s", child); rc = EFBIG; break; }
        rc = list_add(l, child, &st, S_ISDIR(st.st_mode));
        if (rc != 0) { snprintf(bad, bad_len, "%s", child); break; }
        if (S_ISDIR(st.st_mode)) rc = walk(root, child, depth + 1, l, bad, bad_len);
    }
    closedir(d);
    return rc;
}

static int cmp_ent(const void *a, const void *b) {
    return strcmp(((const ent_t *)a)->rel, ((const ent_t *)b)->rel);
}

/* ausgelesener Spielstand im Zwischenordner */
typedef struct {
    char     stage[SP];
    list_t   list;
    char    *json;      /* chives.json (PS5), sonst NULL */
    int      jlen;
    int      ps4;
    char     note[600];
    time_t   img_mtime;  /* Änderungszeit des Abbilds (für den Vergleich "seit der Sicherung geändert") */
} xout_t;

static void xout_free(xout_t *o) {
    if (o->stage[0]) remove_tree(o->stage);
    o->stage[0] = 0;
    free(o->list.v);
    free(o->json);
    memset(o, 0, sizeof(*o));
}

/* Spielstand auslesen: Abbild kopieren, Kopie einhängen, Dateien in den Zwischenordner, aushängen; param.sfo das echte.
 * p: Abschnitte 0..600 Promille. 0, ECANCELED oder -1 (err) */
static int export_stage(const save_info_t *s, prog_t *p, xout_t *o, char *err, size_t errsz) {
    memset(o, 0, sizeof(*o));
    const char *work = saves_workdir();
    const char *imgname = s->save_dir;
    if (strncmp(imgname, "sdimg_", 6) || !slot_ok(imgname + 6)) return efail(err, errsz, "%s", T("Die Abbild-Datei des Spielstands gibt es nicht (mehr)."));
    char img[SP], mnt[SP];
    if (path_fmt(img, sizeof(img), "%s/%s", work, imgname) || path_fmt(mnt, sizeof(mnt), "%s/mnt", work) ||
        path_fmt(o->stage, sizeof(o->stage), "%s/stage", work))
        return efail(err, errsz, "%s", T("Pfad zu lang"));
    int rc = -1, mounted = 0, stuck = 0, have_img = 0;
    uint8_t sealed[SAVEMNT_SEALED_BYTES];
    memset(sealed, 0, sizeof(sealed));
    uint8_t *sfo = NULL;
    uint32_t sfo_len = 0;
    char pdiag[480] = "";
    chives_t geo;
    chives_init(&geo);
    savemnt_t mt;
    memset(&mt, 0, sizeof(mt));

    struct stat st0;
    if (stat(s->path, &st0) != 0 || !S_ISREG(st0.st_mode)) { efail(err, errsz, "%s", T("Die Abbild-Datei des Spielstands gibt es nicht (mehr).")); goto done; }
    o->img_mtime = st0.st_mtime;
    {
        uint8_t head[0x370];
        int fd = open(s->path, O_RDONLY);
        ssize_t n = fd >= 0 ? pread(fd, head, sizeof(head), 0) : -1;
        const int e = errno;
        if (fd >= 0) close(fd);
        if (n < 0x40) { efail(err, errsz, T("Die Abbild-Datei ließ sich nicht lesen (%s)."), strerror(n < 0 ? e : EIO)); goto done; }
        if (savemnt_header_kind(head, (size_t)n) == 1) o->ps4 = 1;   /* PS4: kein eigener Aufbau, der Schlüssel steht in <Name>.bin */
        else if (chives_from_header(head, (size_t)n, (uint64_t)st0.st_size, &geo) != 0) {
            efail(err, errsz, "%s", T("Das ist kein Spielstand, den sich übertragen lässt (nur PS4- und PS5-Spielstände)."));
            goto done;
        }
    }
    if (o->ps4) {
        char bin[SP];
        if (path_fmt(bin, sizeof(bin), "%.*s%s.bin", (int)(strlen(s->path) - strlen(imgname)), s->path, imgname + 6)) {
            efail(err, errsz, "%s", T("Pfad zu lang"));
            goto done;
        }
        int bfd = open(bin, O_RDONLY);
        ssize_t bn = bfd >= 0 ? read(bfd, sealed, sizeof(sealed)) : -1;
        const int be = errno;
        if (bfd >= 0) close(bfd);
        if (bn != (ssize_t)sizeof(sealed)) {
            efail(err, errsz, T("Zum Spielstand gehört die Schlüsseldatei (%s.bin) nicht oder sie ist nicht lesbar (%s)."), imgname + 6, strerror(bn < 0 ? be : EIO));
            goto done;
        }
    }
    /* Platz: die Kopie des Abbilds, dann die Dateien daraus */
    {
        uint64_t avail = 0;
        const uint64_t need = 2 * (uint64_t)st0.st_size + MIN_FREE;
        if (free_space(work, &avail) == 0 && avail < need) {
            efail(err, errsz, T("Auf der internen Platte ist zu wenig Platz: gebraucht werden etwa %llu MB, frei sind %llu MB."),
                  (unsigned long long)(need >> 20), (unsigned long long)(avail >> 20));
            goto done;
        }
    }
    if (work_clean(work, err, errsz) != 0) goto done;

    /* 1. die Kopie des Abbilds - das Original wird nie eingehängt und nie geschrieben */
    prog_phase(p, 0, 300, (uint64_t)st0.st_size);
    {
        int e = copy_file(p, s->path, img, (uint64_t)st0.st_size, 0755);
        if (e == ECANCELED) { rc = ECANCELED; goto done; }
        if (e == EAGAIN) { efail(err, errsz, "%s", T("Der Spielstand hat sich beim Kopieren geändert. Läuft ein Spiel? Es noch einmal versuchen, wenn keines läuft.")); goto done; }
        if (e != 0) { efail(err, errsz, T("Das Abbild ließ sich nicht kopieren: %s"), strerror(e)); goto done; }
        have_img = 1;
        struct stat st1;
        if (stat(s->path, &st1) != 0 || st1.st_size != st0.st_size || st1.st_mtime != st0.st_mtime) {
            efail(err, errsz, "%s", T("Der Spielstand hat sich beim Kopieren geändert. Läuft ein Spiel? Es noch einmal versuchen, wenn keines läuft."));
            goto done;
        }
    }
    if (prog_stopped(p)) { rc = ECANCELED; goto done; }

    /* das echte param.sfo aus dem Kopf des Abbilds, VOR dem Einhängen (wie die Werkzeuge der Konsole) */
    sfo = malloc(SAVEMNT_PARAM_BYTES);
    if (!sfo) { efail(err, errsz, "%s", T("Kein Speicher")); goto done; }
    {
        char e2[480];
        const int pr = o->ps4 ? -1 : savemnt_param_read(img, 0, sfo, SAVEMNT_PARAM_BYTES, &sfo_len, e2, sizeof(e2));
        if (pr == 0 && savesfo_valid(sfo, sfo_len)) {
            /* gut */
        } else {
            sfo_len = 0;
            if (pr == -2) scpy(pdiag, sizeof(pdiag), e2);
            else if (pr == 0) scpy(pdiag, sizeof(pdiag), T("Die Metadaten (param.sfo) im Kopf des Abbilds sind nicht lesbar."));
        }
    }

    /* 2. eingehängt: die Dateien heraus */
    {
        char e2[400];
        if (mkdir(mnt, 0755) != 0 && errno != EEXIST) { efail(err, errsz, T("Der Einhängeordner ließ sich nicht anlegen: %s"), strerror(errno)); goto done; }
        if ((o->ps4 ? savemnt_open_sealed(img, sealed, mnt, &mt, e2, sizeof(e2)) : savemnt_open(img, mnt, &mt, e2, sizeof(e2))) != 0) {
            efail(err, errsz, "%s", e2);
            goto done;
        }
        mounted = 1;
    }
    {
        char bad[SX_REL_MAX + 8] = "";
        int e = walk(mnt, "", 0, &o->list, bad, sizeof(bad));
        if (e == E2BIG) efail(err, errsz, T("Der Spielstand hat mehr als %u Dateien und Ordner: so viele lassen sich nicht übertragen."), SX_MAX_ENT);
        else if (e == EINVAL) efail(err, errsz, T("Ein Name im Spielstand lässt sich nicht in eine ZIP-Datei übernehmen: %s"), bad);
        else if (e == EFBIG) efail(err, errsz, T("Eine Datei im Spielstand ist zu groß für eine ZIP-Datei (%s)."), bad);
        else if (e != 0) efail(err, errsz, T("Der Spielstand ließ sich nicht durchsehen (%s): %s"), bad, strerror(e));
        if (e != 0) goto done;
        if (o->list.bytes > SX_ZIP_MAX) { efail(err, errsz, T("Der Spielstand ist mit %llu MB zu groß für eine ZIP-Datei."), (unsigned long long)(o->list.bytes >> 20)); goto done; }
        if (o->list.n > 1) qsort(o->list.v, o->list.n, sizeof(*o->list.v), cmp_ent);
        if (o->list.skipped)
            snprintf(o->note, sizeof(o->note), T("%u Einträge, die keine Dateien sind (Verknüpfungen), wurden nicht übernommen."), o->list.skipped);
    }
    {
        uint64_t avail = 0;
        if (free_space(work, &avail) == 0 && avail < 2 * o->list.bytes + MIN_FREE) {
            efail(err, errsz, T("Auf der internen Platte ist zu wenig Platz für die Dateien des Spielstands (gebraucht werden etwa %llu MB, frei sind %llu MB)."),
                  (unsigned long long)((2 * o->list.bytes + MIN_FREE) >> 20), (unsigned long long)(avail >> 20));
            goto done;
        }
    }
    if (mkdir_p(o->stage, 0755) != 0) { efail(err, errsz, "%s", T("Der Zwischenordner ließ sich nicht anlegen.")); goto done; }
    prog_phase(p, 300, 500, o->list.bytes);
    for (unsigned i = 0; i < o->list.n; i++) {
        const ent_t *e = &o->list.v[i];
        char src[SP], dst[SP];
        if (path_fmt(src, sizeof(src), "%s/%s", mnt, e->rel) || path_fmt(dst, sizeof(dst), "%s/%s", o->stage, e->rel)) {
            efail(err, errsz, "%s", T("Pfad zu lang"));
            goto done;
        }
        if (e->dir) {
            if (mkdir(dst, 0755) != 0 && errno != EEXIST) { efail(err, errsz, T("Ein Ordner des Spielstands ließ sich nicht anlegen (%s): %s"), e->rel, strerror(errno)); goto done; }
            continue;
        }
        int ce = copy_file(p, src, dst, e->size, 0644);   /* lesbar auch nach dem Zurückgeben der Kennung des Dienstes */
        if (ce == ECANCELED) { rc = ECANCELED; goto done; }
        if (ce == EAGAIN) { efail(err, errsz, T("Eine Datei des Spielstands hat sich beim Lesen geändert (%s)."), e->rel); goto done; }
        if (ce != 0) { efail(err, errsz, T("Eine Datei des Spielstands ließ sich nicht lesen (%s): %s"), e->rel, strerror(ce)); goto done; }
    }

    /* 3. loslassen */
    {
        char e2[400];
        const int ce = savemnt_close(&mt, e2, sizeof(e2));
        mounted = 0;
        if (ce != 0) {
            stuck = 1;   /* das Abbild bleibt, wo es ist: es jetzt zu löschen, zöge es der Konsole unter den Füßen weg */
            efail(err, errsz, T("%s Der Spielstand wurde nicht verändert; bitte die PS5 neu starten, bevor es noch einmal versucht wird."), e2);
            goto done;
        }
    }
    rmdir(mnt);
    unlink(img);
    have_img = 0;
    if (prog_stopped(p)) { rc = ECANCELED; goto done; }

    /* das echte param.sfo statt des Blocks Nullen im Dateisystem */
    if (sfo_len) {
        char dir[SP], path[SP];
        if (path_fmt(dir, sizeof(dir), "%s/sce_sys", o->stage) || path_fmt(path, sizeof(path), "%s/sce_sys/param.sfo", o->stage)) {
            efail(err, errsz, "%s", T("Pfad zu lang"));
            goto done;
        }
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) { efail(err, errsz, "%s", T("Der Ordner sce_sys ließ sich nicht anlegen.")); goto done; }
        unlink(path);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 || write_all(fd, sfo, sfo_len) != 0) {
            const int e = errno ? errno : EIO;
            if (fd >= 0) close(fd);
            efail(err, errsz, T("Die Metadaten ließen sich nicht schreiben: %s"), strerror(e));
            goto done;
        }
        close(fd);
        int found = -1;
        for (unsigned i = 0; i < o->list.n; i++) if (!strcmp(o->list.v[i].rel, "sce_sys/param.sfo")) found = (int)i;
        if (found >= 0) {
            o->list.bytes = o->list.bytes - o->list.v[found].size + sfo_len;
            o->list.v[found].size = sfo_len;
        } else {
            struct stat fake;
            memset(&fake, 0, sizeof(fake));
            fake.st_size = sfo_len;
            fake.st_mode = S_IFREG | 0644;
            fake.st_mtime = time(NULL);
            int has_dir = 0;
            for (unsigned i = 0; i < o->list.n; i++) if (!strcmp(o->list.v[i].rel, "sce_sys")) has_dir = 1;
            if (!has_dir) {
                struct stat fd2 = fake;
                fd2.st_mode = S_IFDIR | 0755;
                if (list_add(&o->list, "sce_sys", &fd2, 1) != 0) { efail(err, errsz, "%s", T("Zu viele Dateien.")); goto done; }
            }
            if (list_add(&o->list, "sce_sys/param.sfo", &fake, 0) != 0) { efail(err, errsz, "%s", T("Zu viele Dateien.")); goto done; }
            if (o->list.n > 1) qsort(o->list.v, o->list.n, sizeof(*o->list.v), cmp_ent);
        }
    } else {
        /* kein Blob im Kopf: das param.sfo des Dateisystems ist das echte - es muss da und stimmig sein */
        char path[SP];
        if (path_fmt(path, sizeof(path), "%s/sce_sys/param.sfo", o->stage)) { efail(err, errsz, "%s", T("Pfad zu lang")); goto done; }
        int fd = open(path, O_RDONLY);
        ssize_t n = fd >= 0 ? read(fd, sfo, SAVEMNT_PARAM_BYTES) : -1;
        if (fd >= 0) close(fd);
        if (n <= 0 || !savesfo_valid(sfo, (size_t)n)) {
            if (pdiag[0]) efail(err, errsz, T("Dem Spielstand fehlen die Metadaten (param.sfo): ohne sie lässt er sich später nicht einspielen. Antwort der Konsole: %s"), pdiag);
            else efail(err, errsz, "%s", T("Dem Spielstand fehlen die Metadaten (param.sfo): ohne sie lässt er sich später nicht einspielen."));
            goto done;
        }
        sfo_len = (uint32_t)n;
        if (pdiag[0]) {
            const size_t on = strlen(o->note);
            if (on) snprintf(o->note + on, sizeof(o->note) - on, " ");
            const size_t on2 = strlen(o->note);
            snprintf(o->note + on2, sizeof(o->note) - on2, T("Die Metadaten stammen aus dem Spielstand selbst: Die Konsole gab die aus dem Kopf des Abbilds nicht heraus (%.300s)."), pdiag);
        }
    }

    /* die ZIP reist (auf eine andere PS5, zu einem Freund): das Konto des Benutzers wird darin nicht weitergegeben. ACCOUNT_ID kommt als 0
     * hinein, das Einspielen schreibt ohnehin das Konto des Zielbenutzers. PARAMS bleibt: das Einspielen braucht es, wenn die andere PS5
     * keinen Spielstand dieses Benutzers hat, aus dem es den Besitzerwert nehmen kann (wie das Cooling Center seit 1.88.0) */
    {
        char path[SP];
        uint8_t *pb = malloc(SAVEMNT_PARAM_BYTES);
        if (pb && !path_fmt(path, sizeof(path), "%s/sce_sys/param.sfo", o->stage)) {
            const int fd = open(path, O_RDWR);
            const ssize_t n = fd >= 0 ? read(fd, pb, SAVEMNT_PARAM_BYTES) : -1;
            if (n > 0 && savesfo_valid(pb, (size_t)n) && savesfo_patch_owner(pb, (size_t)n, 0, 0, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0 &&
                write_all(fd, pb, (size_t)n) == 0 && fsync(fd) == 0) {
                if (savesfo_valid(sfo, sfo_len)) (void)savesfo_patch_owner(sfo, sfo_len, 0, 0, 0);
            }
            if (fd >= 0) close(fd);
        }
        free(pb);
    }

    /* die Beschreibung (PS5; ein PS4-Spielstand reist nur mit seinen Dateien - wie bei Garlic SaveMgr) */
    if (!o->ps4) {
        char tid[24] = "", slot[120] = "";
        savesfo_get_str(sfo, sfo_len, "TITLE_ID", tid, sizeof(tid));
        savesfo_get_str(sfo, sfo_len, "SAVEDATA_DIRECTORY", slot, sizeof(slot));
        scpy(geo.title_id, sizeof(geo.title_id), tid[0] ? tid : s->title_id);
        scpy(geo.save_dir, sizeof(geo.save_dir), slot[0] ? slot : imgname + 6);
        scpy(geo.image_name, sizeof(geo.image_name), imgname);
        for (unsigned i = 0; i < o->list.n; i++)
            if (chives_own_add(&geo, o->list.v[i].rel, o->list.v[i].uid, o->list.v[i].gid, o->list.v[i].mode) != 0) break;
        o->json = malloc(CHIVES_MAX_JSON);
        if (!o->json) { efail(err, errsz, "%s", T("Kein Speicher")); goto done; }
        o->jlen = chives_write(&geo, o->json, CHIVES_MAX_JSON);
        if (o->jlen < 0) { efail(err, errsz, "%s", T("Die Beschreibung des Abbilds (chives.json) ließ sich nicht erzeugen.")); goto done; }
    }
    rc = 0;
done:
    if (mounted) {
        char e2[300];
        if (savemnt_close(&mt, e2, sizeof(e2)) != 0) {
            stuck = 1;
            if (rc == ECANCELED) rc = -1;
            efail(err, errsz, T("%s Bitte die PS5 neu starten, bevor es noch einmal versucht wird."), e2);
        }
    }
    if (!stuck) {
        if (have_img) unlink(img);
        rmdir(mnt);
    }
    free(sfo);
    chives_free(&geo);
    if (rc != 0) {
        if (rc != ECANCELED && !err[0]) efail(err, errsz, "%s", T("Der Spielstand ließ sich nicht auslesen."));
        if (!stuck) xout_free(o);
        else { free(o->list.v); free(o->json); o->list.v = NULL; o->json = NULL; }
    }
    return rc;
}

/* ausgelesenen Spielstand in die ZIP (Dateien, dann chives.json); p: Abschnitt a..b. 0, ECANCELED oder -1 */
static int export_zip(szw_t *z, const char *prefix, xout_t *o, prog_t *p, unsigned a, unsigned b, char *err, size_t errsz) {
    prog_phase(p, a, b, o->list.bytes);
    for (unsigned i = 0; i < o->list.n; i++) {
        const ent_t *e = &o->list.v[i];
        if (e->dir) continue;   /* Ordner entstehen aus den Pfaden der Dateien (und der Besitzerliste) wieder */
        char src[SP], name[SZ_MAX_NAME + 1];
        if (path_fmt(src, sizeof(src), "%s/%s", o->stage, e->rel) || path_fmt(name, sizeof(name), "%s%s", prefix, e->rel))
            return efail(err, errsz, "%s", T("Pfad zu lang"));
        const int zrc = szw_add_file(z, name, src, e->mtime, e->mode ? e->mode : 0644, prog_tick, p);
        if (zrc == SZ_E_STOP) return ECANCELED;
        if (zrc == SZ_E_TOO_BIG) return efail(err, errsz, "%s", T("Der Spielstand ist zu groß für eine ZIP-Datei."));
        if (zrc == SZ_E_CHANGED) return efail(err, errsz, "%s", T("Eine Datei hat sich beim Verpacken geändert."));
        if (zrc != SZ_OK) return efail(err, errsz, T("Die ZIP-Datei ließ sich nicht schreiben: %s"), strerror(errno ? errno : EIO));
    }
    if (o->json) {
        char name[SZ_MAX_NAME + 1];
        if (path_fmt(name, sizeof(name), "%schives.json", prefix)) return efail(err, errsz, "%s", T("Pfad zu lang"));
        if (szw_add_mem(z, name, o->json, (size_t)o->jlen, time(NULL), 0644) != SZ_OK)
            return efail(err, errsz, T("Die ZIP-Datei ließ sich nicht schreiben: %s"), strerror(errno ? errno : EIO));
    }
    return 0;
}

/* ── Herunterladen: chunked HTTP ── */

typedef struct {
    int  sock;
    int  started, broken;
    char head[768];
} http_sink_t;

static int sock_send(int sock, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(sock, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int http_sink(void *ctx, const void *buf, size_t len) {
    http_sink_t *h = ctx;
    if (h->broken) return -1;
    if (!len) return 0;   /* Länge 0 beendete den Strom */
    if (!h->started) {
        h->started = 1;
        if (sock_send(h->sock, h->head, strlen(h->head))) { h->broken = 1; return -1; }
    }
    char hdr[24];
    int hl = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    if (sock_send(h->sock, hdr, (size_t)hl) || sock_send(h->sock, buf, len) || sock_send(h->sock, "\r\n", 2)) {
        h->broken = 1;
        return -1;
    }
    return 0;
}

int saves_download_fits(const save_info_t *list, int n) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += list[i].size;
    return total <= SX_ZIP_MAX;
}

int saves_download(int sock, const save_info_t *list, int n, char *err, size_t errsz) {
    if (n < 1) { snprintf(err, errsz, "%s", T("Kein Spielstand ausgewählt")); return -1; }
    if (op_enter(err, errsz)) return -1;
    int rc = -1;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += list[i].size;
    http_sink_t h;
    memset(&h, 0, sizeof(h));
    h.sock = sock;
    szw_t *z = NULL;
    if (total > SX_ZIP_MAX) {
        snprintf(err, errsz, "%s", T("Zusammen zu groß für eine ZIP-Datei (höchstens 3,5 GB) – weniger Spielstände wählen"));
        goto done;
    }
    if (n == 1)
        snprintf(h.head, sizeof(h.head),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                 "Content-Disposition: attachment; filename=\"%s_%s.zip\"\r\nTransfer-Encoding: chunked\r\n"
                 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                 list[0].title_id, list[0].save_dir);
    else
        snprintf(h.head, sizeof(h.head),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                 "Content-Disposition: attachment; filename=\"Spielstaende_%d.zip\"\r\nTransfer-Encoding: chunked\r\n"
                 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", n);
    struct timeval tv = {60, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    z = szw_open_sink(http_sink, &h);
    if (!z) { snprintf(err, errsz, "%s", T("Kein Speicher")); goto done; }
    for (int i = 0; i < n; i++) {
        char prefix[400] = "", e2[600] = "";
        if (n > 1 && path_fmt(prefix, sizeof(prefix), "%s_%s_%s/", list[i].uid, list[i].title_id, list[i].save_dir)) {
            snprintf(err, errsz, "%s", T("Pfad zu lang"));
            rc = h.started ? -2 : -1;
            goto done;
        }
        xout_t o;
        int r = export_stage(&list[i], NULL, &o, e2, sizeof(e2));
        if (r == 0) {
            if (n == 1) {
                char c[64];
                snprintf(c, sizeof(c), "usbfmt-mtime:%lld", (long long)o.img_mtime);
                szw_comment(z, c);
            }
            r = export_zip(z, prefix, &o, NULL, 0, 0, e2, sizeof(e2));
            xout_free(&o);
        }
        if (r != 0) {
            if (!h.started) { snprintf(err, errsz, "%s", e2[0] ? e2 : T("Spielstand konnte nicht gelesen werden")); rc = -1; }
            else rc = -2;
            goto done;
        }
    }
    {
        const int fr = szw_finish(z);
        z = NULL;
        if (fr != SZ_OK || h.broken) { rc = h.started ? -2 : -1; if (rc == -1) snprintf(err, errsz, "%s", T("Spielstand konnte nicht gelesen werden")); goto done; }
    }
    if (h.started && sock_send(sock, "0\r\n\r\n", 5)) { rc = -2; goto done; }
    rc = 0;
done:
    if (z) szw_abort(z);
    op_leave();
    return rc;
}

/* ── Sichern in eine Datei ── */

#define BACKUP_KEEP 3   /* je Spielstand und Ort: die neueste Sicherung und die zwei davor */

static int is_version_name(const char *name, const char *prefix) {
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl) != 0 || name[pl] != '.' || strlen(name) != pl + 1 + 15 + 4 || strcmp(name + pl + 16, ".zip") != 0)
        return 0;
    for (int i = 0; i < 15; i++)
        if (i == 8 ? name[pl + 1 + i] != '-' : !isdigit((unsigned char)name[pl + 1 + i])) return 0;
    return 1;
}

static int cmp_str_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static int g_rot_err;   /* Fehler beim Beiseitelegen der bisherigen Sicherung */
static void rotate_backups(const char *dir, const char *prefix, const char *final_path, char *rotated, size_t rotsz) {
    struct stat st;
    g_rot_err = 0;
    if (rotated && rotsz) rotated[0] = 0;
    if (stat(final_path, &st) == 0 && S_ISREG(st.st_mode)) {
        struct tm tmv;
        char stamp[24], old[1400];
        time_t t = st.st_mtime;
        gmtime_r(&t, &tmv);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        if (path_fmt(old, sizeof(old), "%s/%s.%s.zip", dir, prefix, stamp)) g_rot_err = ENAMETOOLONG;
        else if (rename(final_path, old) != 0) g_rot_err = errno ? errno : EIO;
        else if (rotated && rotsz) scpy(rotated, rotsz, old);
    }
    DIR *d = opendir(dir);
    if (!d) return;
    char (*names)[320] = malloc(sizeof(*names) * 256);
    int n = 0;
    struct dirent *e;
    while (names && (e = readdir(d)) && n < 256)
        if (is_version_name(e->d_name, prefix) && strlen(e->d_name) < sizeof(*names)) scpy(names[n++], sizeof(*names), e->d_name);
    closedir(d);
    if (names && n > BACKUP_KEEP - 1) {
        qsort(names, (size_t)n, sizeof(*names), cmp_str_desc);
        for (int i = BACKUP_KEEP - 1; i < n; i++) {
            char p[1400];
            if (!path_fmt(p, sizeof(p), "%s/%s", dir, names[i])) unlink(p);
        }
    }
    free(names);
}

/* Dateiname für Ordner: Buchstaben, Ziffern, "_" und "-"; alles andere "_" */
static void safe_name(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; *src && i + 1 < n; src++) {
        unsigned char c = (unsigned char)*src;
        dst[i++] = (isalnum(c) && c < 0x80) || c == '_' || c == '-' ? (char)c : '_';
    }
    dst[i] = 0;
    if (!i) scpy(dst, n, "Benutzer");
}

#define UNDO_TAG  "vorher-"
#define UNDO_KEEP 3   /* je Spielstand: die drei neuesten Sicherungen "Stand vor dem Einspielen" */

/* die ZIP-Datei, aus der gerade eingespielt wird: das Aufräumen der Stände davor lässt sie stehen, auch wenn sie selbst ein solcher
 * Stand ist (sonst wäre der Wiederherstellungspunkt weg, falls das Einspielen danach scheitert) */
static const char *g_undo_keep;

/* Hinweis der letzten Sicherung (etwa: Metadaten aus dem Spielstand selbst, Verknüpfungen nicht übernommen) */
static char g_last_note[600];
const char *saves_last_note(void) { return g_last_note; }

/* Sicherungen "<prefix>.vorher-<JJJJMMTT-HHMMSS>.zip" über die neuesten UNDO_KEEP hinaus löschen */
static void prune_undo(const char *dir, const char *prefix) {
    struct stat ks;
    const int have_keep = g_undo_keep && stat(g_undo_keep, &ks) == 0;
    DIR *d = opendir(dir);
    if (!d) return;
    char (*names)[320] = malloc(sizeof(*names) * 256);
    int n = 0;
    struct dirent *e;
    const size_t pl = strlen(prefix), tl = strlen(UNDO_TAG);
    while (names && (e = readdir(d)) && n < 256) {
        const size_t l = strlen(e->d_name);
        if (l == pl + 1 + tl + 15 + 4 && !strncmp(e->d_name, prefix, pl) && e->d_name[pl] == '.' && !strncmp(e->d_name + pl + 1, UNDO_TAG, tl) &&
            !strcmp(e->d_name + l - 4, ".zip"))
            scpy(names[n++], sizeof(*names), e->d_name);
    }
    closedir(d);
    if (names && n > UNDO_KEEP) {
        qsort(names, (size_t)n, sizeof(*names), cmp_str_desc);
        for (int i = UNDO_KEEP; i < n; i++) {
            char p[1400];
            struct stat ps;
            if (path_fmt(p, sizeof(p), "%s/%s", dir, names[i])) continue;
            if (have_keep && stat(p, &ps) == 0 && ps.st_dev == ks.st_dev && ps.st_ino == ks.st_ino) continue;
            unlink(p);
        }
    }
    free(names);
}

/* eine Sicherung schreiben (ohne die Sperre: der Aufrufer hält sie). undo: als Stand vor dem Einspielen unter eigenem Namen
 * (<Titel>_<Name>.vorher-<Zeit>.zip) - die gewöhnliche Sicherung, aus der vielleicht gerade eingespielt wird, bleibt unberührt */
static int backup_one(const save_info_t *s, const char *root, int undo, char *out, size_t outsz, char *rotated, size_t rotsz, saves_prog_fn prog,
                      void *pctx, char *err, size_t errsz) {
    int rc = -1, have_stage = 0;
    char user[64], dir[1000], tmp[1300], final_path[1300], stamp[24] = "00000000-000000";
    szw_t *z = NULL;
    xout_t o;
    memset(&o, 0, sizeof(o));
    prog_t p = {prog, pctx, s->size ? s->size : 1, 0, 0, 0, 0, 0};
    err[0] = 0;
    safe_name(user, 40, s->username[0] ? s->username : s->uid);
    {
        time_t now = time(NULL);
        struct tm tmv;
        if (gmtime_r(&now, &tmv)) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    }
    if (path_fmt(dir, sizeof(dir), "%s/%s_%s", root, user, s->uid) ||
        (undo ? path_fmt(final_path, sizeof(final_path), "%s/%s_%s." UNDO_TAG "%s.zip", dir, s->title_id, s->save_dir, stamp)
              : path_fmt(final_path, sizeof(final_path), "%s/%s_%s.zip", dir, s->title_id, s->save_dir)) ||
        path_fmt(tmp, sizeof(tmp), "%s/.%s_%s.zip.tmp", dir, s->title_id, s->save_dir)) {
        snprintf(err, errsz, "%s", T("Pfad zu lang"));
        tmp[0] = 0;
        goto done;
    }
    mkdirs(root);
    mkdirs(dir);
    {
        struct statvfs sv;
        if (statvfs(root, &sv) == 0 && (uint64_t)sv.f_bavail * sv.f_frsize < s->size + FREE_MARGIN) {
            snprintf(err, errsz, "%s", T("Am Ziel ist nicht mehr genug Platz frei."));
            goto done;
        }
    }
    g_last_note[0] = 0;
    const int er = export_stage(s, &p, &o, err, errsz);
    if (er == 0) scpy(g_last_note, sizeof(g_last_note), o.note);
    if (er == ECANCELED) { snprintf(err, errsz, "%s", T("Abgebrochen.")); goto done; }
    if (er != 0) goto done;
    have_stage = 1;
    unlink(tmp);
    z = szw_create(tmp);
    if (!z) { snprintf(err, errsz, T("Zieldatei konnte nicht angelegt werden: %s"), strerror(errno)); goto done; }
    {
        char c[64];   /* die Zeit des Abbilds: die Liste der Sicherungen vergleicht sie mit dem Spielstand auf der PS5 */
        snprintf(c, sizeof(c), "usbfmt-mtime:%lld", (long long)o.img_mtime);
        szw_comment(z, c);
    }
    const int zr = export_zip(z, "", &o, &p, 600, 1000, err, errsz);
    if (zr == ECANCELED) { snprintf(err, errsz, "%s", T("Abgebrochen.")); goto done; }
    if (zr != 0) goto done;
    {
        const int fr = szw_finish(z);
        z = NULL;
        if (fr != SZ_OK) { snprintf(err, errsz, "%s", T("Schreibfehler: Datenträger voll oder nicht beschreibbar")); goto done; }
    }
    {
        char prefix[600];
        if (!undo && !path_fmt(prefix, sizeof(prefix), "%s_%s", s->title_id, s->save_dir)) rotate_backups(dir, prefix, final_path, rotated, rotsz);
        struct stat fs;
        if (!undo && lstat(final_path, &fs) == 0) {   /* die bisherige ließ sich nicht beiseitelegen: nicht ersetzen */
            snprintf(err, errsz, T("Die bisherige Sicherung ließ sich nicht als Vorversion beiseitelegen: %s"), strerror(g_rot_err ? g_rot_err : EIO));
            goto done;
        }
    }
    if (rename(tmp, final_path) != 0) {
        const int re = errno ? errno : EIO;
        if (rotated && rotated[0]) { (void)rename(rotated, final_path); rotated[0] = 0; }   /* die bisherige wieder unter ihrem Namen */
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(re));
        goto done;
    }
    tmp[0] = 0;
    {   /* auch der Name im Ordner bleibt: ein Stick, der gleich nach "fertig" gezogen wird, muss die Datei unter ihrem Namen haben */
        const int dfd = open(dir, O_RDONLY);
        if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
    }
    if (undo) {
        char prefix[600];
        if (!path_fmt(prefix, sizeof(prefix), "%s_%s", s->title_id, s->save_dir)) prune_undo(dir, prefix);
    }
    scpy(out, outsz, final_path);
    rc = 0;
done:
    if (z) szw_abort(z);
    if (tmp[0]) unlink(tmp);
    if (have_stage) xout_free(&o);
    return rc;
}

int saves_backup_file(const save_info_t *s, const char *root, char *out, size_t outsz, char *rotated, size_t rotsz, saves_prog_fn prog,
                      void *pctx, char *err, size_t errsz) {
    if (rotated && rotsz) rotated[0] = 0;
    if (op_enter(err, errsz)) return -1;
    const int rc = backup_one(s, root, 0, out, outsz, rotated, rotsz, prog, pctx, err, errsz);
    op_leave();
    return rc;
}

int saves_backup_revert(const char *path, const char *rotated) {
    if (!path || !path[0]) return 0;
    if (unlink(path) != 0 && errno != ENOENT) return -1;   /* etwa ein abgezogenes Laufwerk: der Aufrufer sagt es */
    if (rotated && rotated[0]) (void)rename(rotated, path);   /* die bisherige Sicherung wieder unter ihrem Namen */
    return 0;
}

/* Spielstände (Schlüssel) um alle übrigen Spielstände derselben Spiele erweitern - wie im PS5 Cooling Center: ein Spiel legt
 * mal SaveData, mal sce_sdmemory, mal beides an, und das soll niemand wissen müssen. Liefert die Zahl in out (höchstens max),
 * je Spiel (in der Reihenfolge der Wahl) alle seine Spielstände nach Namen. -1, wenn ein Schlüssel nicht gefunden wird. */
int saves_expand(const save_info_t *all, int na, char *const *keys, int nk, const save_info_t **out, int max) {
    int n = 0;
    for (int i = 0; i < nk; i++) {
        const save_info_t *hit = NULL;
        for (int k = 0; k < na && !hit; k++)
            if (!strcmp(all[k].key, keys[i])) hit = &all[k];
        if (!hit) return -1;
        for (int k = 0; k < na; k++) {   /* saves_scan liefert nach Schlüssel sortiert: die Spielstände eines Spiels stehen beisammen */
            const save_info_t *s = &all[k];
            if (strcmp(s->uid, hit->uid) || strcmp(s->title_id, hit->title_id) || strcmp(s->platform, hit->platform)) continue;
            int dup = 0;
            for (int m = 0; m < n && !dup; m++) dup = out[m] == s;
            if (dup) continue;
            if (n == max) return -2;   /* mehr als ein Auftrag fasst: gesagt, nicht abgeschnitten */
            out[n++] = s;
        }
    }
    return n;
}

/* ════════════════════════ Einspielen (wie saveimport.c) ════════════════════════ */

typedef struct {
    char     title_id[16];
    char     slot[100];
    char     main_title[256];
    char     sub_title[256];
    char     detail[1024];
    char     prefix[200];           /* Ordner des Spielstands in der ZIP ("" = Wurzel) */
    uint64_t image_size, block_count;
    uint32_t block_size;
    uint32_t user_param;
    unsigned files, dirs;
    uint64_t bytes;
    int      ps4;
    int      converted;             /* Copy-on-write-Abbild: als klassisches Abbild seiner Datenblöcke angelegt */
    uint64_t src_system_blocks;
    int      importable;
    char     why[400];
} iinfo_t;

static void why_set(iinfo_t *info, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void why_set(iinfo_t *info, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(info->why, sizeof(info->why), fmt, ap);
    va_end(ap);
    info->importable = 0;
}

/* Einträge anderer Werkzeuge neben den Dateien (Abbild, Schlüssel, Größe): keine Dateien des Spielstands */
static int is_foreign_sidecar(const char *rel) {
    return !strcmp(rel, "PS5IMAGE") || !strcmp(rel, "PS5KEY") || !strcmp(rel, "PS5PARAM_SIZE");
}

static int sfo_set_num(uint8_t *sfo, size_t len, const char *key, uint64_t v) {
    size_t off, used;
    uint8_t b[8];
    if (savesfo_find(sfo, len, key, &off, &used, NULL) != 0 || used < 1 || used > sizeof(b)) return -1;
    for (size_t i = 0; i < sizeof(b); i++) b[i] = (uint8_t)(v >> (8 * i));
    return savesfo_set_raw(sfo, len, key, 0, b, used);
}

static int under(const char *name, const char *prefix) { return !strncmp(name, prefix, strlen(prefix)); }

/* einen Spielstand der ZIP (Ordner prefix) prüfen: info, geo und das param.sfo (sfo_out, free()) */
static void inspect_root(szr_t *z, const char *prefix, iinfo_t *info, chives_t *geo, uint8_t **sfo_out, size_t *sfo_len_out) {
    memset(info, 0, sizeof(*info));
    chives_init(geo);
    *sfo_out = NULL;
    *sfo_len_out = 0;
    scpy(info->prefix, sizeof(info->prefix), prefix);
    const size_t pl = strlen(prefix);
    const unsigned n = szr_count(z);
    int side = -1, sfo_i = -1;
    for (unsigned i = 0; i < n; i++) {
        const szr_entry_t *e = szr_at(z, i);
        if (e->is_dir || !under(e->name, prefix)) continue;
        if (chives_is_sidecar(e->name + pl)) side = (int)i;
        if (!strcmp(e->name + pl, "sce_sys/param.sfo")) sfo_i = (int)i;
    }
    if (sfo_i < 0) { why_set(info, "%s", T("In der ZIP-Datei fehlt sce_sys/param.sfo.")); return; }
    uint8_t *sfo = NULL;
    size_t sl = 0;
    if (szr_read(z, (unsigned)sfo_i, &sfo, &sl, SI_MAX_SFO) != SZ_OK || !savesfo_valid(sfo, sl)) {
        free(sfo);
        why_set(info, "%s", T("Das param.sfo der ZIP-Datei ist leer oder nicht lesbar. Bei Spielständen neuerer Firmware liegt es nur im Abbildkopf, den die App nicht erzeugen kann."));
        return;
    }
    char tid[24] = "", slot[120] = "";
    savesfo_get_str(sfo, sl, "TITLE_ID", tid, sizeof(tid));
    savesfo_get_str(sfo, sl, "SAVEDATA_DIRECTORY", slot, sizeof(slot));
    /* jede Datei des Spielstands zählen */
    for (unsigned i = 0; i < n; i++) {
        const szr_entry_t *e = szr_at(z, i);
        if (!under(e->name, prefix)) continue;
        if (e->is_dir) { info->dirs++; continue; }
        if ((int)i == side || chives_is_sidecar(e->name + pl) || is_foreign_sidecar(e->name + pl)) continue;
        if (e->method != 0 && e->usize > SI_MEM_MAX) {   /* gepackt wird über den Speicher entpackt: so große werden nicht angenommen */
            free(sfo);
            why_set(info, T("Eine Datei in der ZIP-Datei ist gepackt und größer als %u MB (%.100s): So große gepackte Dateien liest der Formatierer nicht."),
                    SI_MEM_MAX >> 20, e->name);
            return;
        }
        info->files++;
        info->bytes += e->usize;
    }
    if (info->files == 0) { free(sfo); why_set(info, "%s", T("Die ZIP-Datei enthält keine Dateien des Spielstands.")); return; }
    if (info->files > SI_MAX_FILES) { free(sfo); why_set(info, T("Die ZIP-Datei enthält mehr als %u Dateien."), SI_MAX_FILES); return; }
    if (!slot_ok(slot)) { free(sfo); why_set(info, "%s", T("Der Name des Spielstands im param.sfo (SAVEDATA_DIRECTORY) ist ungültig.")); return; }

    if (side < 0) {
        /* ohne Beschreibung: ein PS4-Spielstand (CUSA) - die Größe wird geschätzt: Dateien + ein Viertel + 4 MB, mind. 32 MB, in 32-KiB-Schritten */
        if (!cusa_ok(tid)) {
            free(sfo);
            why_set(info, "%s", T("In dieser ZIP-Datei fehlt chives.json: Ohne die Beschreibung des Abbilds (Größe, Aufbau, Besitzer) lässt sich der PS5-Spielstand nicht einspielen. Die ZIP-Datei muss von dieser App, dem PS5 Cooling Center oder Garlic SaveMgr stammen."));
            return;
        }
        info->ps4 = 1;
        uint64_t sz = info->bytes + info->bytes / 4 + (4ull << 20);
        if (sz < (32ull << 20)) sz = 32ull << 20;
        sz = (sz + 32767) / 32768 * 32768;
        info->block_size = 32768;
        info->image_size = sz;
        info->block_count = sz / 32768;
        geo->version = 1;
        geo->block_size = 32768;
        geo->block_count = info->block_count;
        geo->image_size = sz;
    } else {
        uint8_t *js = NULL;
        size_t jl = 0;
        if (szr_read(z, (unsigned)side, &js, &jl, CHIVES_MAX_JSON) != SZ_OK) { free(sfo); why_set(info, "%s", T("Die chives.json der ZIP-Datei ist nicht lesbar.")); return; }
        const int pr = chives_parse((const char *)js, jl, geo);
        free(js);
        if (pr != 0) { free(sfo); why_set(info, "%s", T("Die chives.json der ZIP-Datei ist keine gültige Beschreibung eines PS5-Spielstands.")); return; }
        info->image_size = geo->image_size;
        info->block_count = geo->block_count;
        info->block_size = geo->block_size;
        const int cow = geo->has_fs_ext && geo->fs_ext != 0;
        if (!geo->has_fs_ext || geo->backup_header_count != 1 || geo->mode_raw != 0x000d || geo->image_size != geo->block_count * (uint64_t)geo->block_size ||
            (cow && geo->fs_ext >= geo->block_count)) {
            free(sfo);
            why_set(info, T("Dieser Spielstand hat ein Abbildformat, das die App nicht anlegen kann (Überschreibbereich fs_ext=%u, mode=%u, Sicherungsköpfe=%u). Es wurde nichts verändert."),
                    (unsigned)geo->fs_ext, (unsigned)geo->mode_raw, (unsigned)geo->backup_header_count);
            return;
        }
        if (info->bytes > geo->image_size) { free(sfo); why_set(info, "%s", T("Die Dateien sind größer als das Abbild, das die chives.json beschreibt.")); return; }
        char fmt[16] = "";
        savesfo_get_str(sfo, sl, "FORMAT", fmt, sizeof(fmt));
        uint8_t b8[8], s8[8];
        const int nb = savesfo_get_raw(sfo, sl, "SAVEDATA_BLOCKS", b8, sizeof(b8));
        const int ns = savesfo_get_raw(sfo, sl, "SYSTEM_BLOCKS", s8, sizeof(s8));
        if (strcmp(fmt, "ppr")) { free(sfo); why_set(info, T("Das ist kein PS5-Spielstand im Format ppr (FORMAT=%.12s)."), fmt); return; }
        if (!ppsa_ok(tid)) { free(sfo); why_set(info, T("Die Titel-Kennung im param.sfo (%.20s) ist keine PS5-Kennung."), tid); return; }
        const uint64_t want_data = cow ? geo->block_count - geo->fs_ext : geo->block_count, want_sys = cow ? geo->fs_ext : 0;
        if (nb < 4 || le_num(b8, (size_t)nb) != want_data || ns < 4 || le_num(s8, (size_t)ns) != want_sys) {
            free(sfo);
            why_set(info, "%s", T("Die Blockzahlen im param.sfo passen nicht zur chives.json: Dieser Spielstand hat ein Abbildformat, das die App nicht anlegen kann."));
            return;
        }
        if (cow) {   /* als klassisches Abbild der Datenblöcke; etwas mehr nur, wenn die Dateien nicht passten */
            uint64_t blocks = want_data;
            const uint64_t file_blocks = (info->bytes + geo->block_size - 1) / geo->block_size;
            const uint64_t need = file_blocks + file_blocks / 8 + 16;
            if (need > blocks) blocks = need;
            info->converted = 1;
            info->src_system_blocks = geo->fs_ext;
            geo->block_count = blocks;
            geo->image_size = blocks * (uint64_t)geo->block_size;
            geo->fs_ext = 0;
            info->block_count = blocks;
            info->image_size = geo->image_size;
        }
        if (geo->title_id[0] && strcmp(geo->title_id, tid)) {
            free(sfo);
            why_set(info, T("Die Titel-Kennung der chives.json (%.12s) weicht vom param.sfo ab (%.12s)."), geo->title_id, tid);
            return;
        }
    }
    scpy(info->title_id, sizeof(info->title_id), tid);
    scpy(info->slot, sizeof(info->slot), slot);
    savesfo_get_str(sfo, sl, "MAINTITLE", info->main_title, sizeof(info->main_title));
    savesfo_get_str(sfo, sl, "SUBTITLE", info->sub_title, sizeof(info->sub_title));
    savesfo_get_str(sfo, sl, "DETAIL", info->detail, sizeof(info->detail));
    uint32_t lp = 0;
    if (savesfo_get_u32(sfo, sl, "SAVEDATA_LIST_PARAM", &lp) == 0) info->user_param = lp;
    *sfo_out = sfo;
    *sfo_len_out = sl;
    info->importable = 1;
}

/* Spielstände (Ordner) in der ZIP: je ein Ordner mit sce_sys/param.sfo; keiner in einem anderen, keine Datei außerhalb */
static int find_roots(szr_t *z, char (*roots)[200], int cap, int *nr, char *err, size_t errsz) {
    *nr = 0;
    const unsigned n = szr_count(z);
    for (unsigned i = 0; i < n; i++) {
        const szr_entry_t *e = szr_at(z, i);
        const size_t l = strlen(e->name);
        if (e->is_dir || l < 17 || strcmp(e->name + l - 17, "sce_sys/param.sfo")) continue;
        if (l > 17 && e->name[l - 18] != '/') continue;
        if (l - 17 >= 200) return efail(err, errsz, "%s", T("Der Ordnername in der ZIP-Datei ist zu lang."));
        if (*nr == cap) return efail(err, errsz, T("Zu viele Spielstände in einer ZIP-Datei (höchstens %d)"), ZIP_MAXSAVES);
        memcpy(roots[*nr], e->name, l - 17);
        roots[*nr][l - 17] = 0;
        (*nr)++;
    }
    if (!*nr) return efail(err, errsz, "%s", T("In der ZIP-Datei ist kein Spielstand (sce_sys/param.sfo fehlt)."));
    for (int a = 0; a < *nr; a++)
        for (int b = 0; b < *nr; b++)
            if (a != b && under(roots[b], roots[a]))
                return efail(err, errsz, "%s", T("Die ZIP-Datei enthält ineinander liegende Spielstände – so lässt sie sich nicht einspielen."));
    for (unsigned i = 0; i < n; i++) {
        const szr_entry_t *e = szr_at(z, i);
        int in = 0;
        for (int r = 0; r < *nr && !in; r++) in = under(e->name, roots[r]) || (e->is_dir && !strncmp(roots[r], e->name, strlen(e->name)));
        if (!in) return efail(err, errsz, T("Die ZIP-Datei enthält Dateien außerhalb des Spielstand-Ordners (%.100s)."), e->name);
    }
    return 0;
}

/* ── Dateien in das neue Abbild ── */

typedef struct {
    const chives_t *geo;
    szr_t       *z;
    const uint8_t *sfo;   /* das angepasste param.sfo */
    size_t       sfo_len;
    char         mnt[SP];
    const char  *prefix;
    size_t       pl;
    prog_t      *p;
    int          chmod_failed;
    int          ps4;     /* ohne Beschreibung: die Rechte kommen aus den ZIP-Einträgen */
} fill_t;

static int in_root(const fill_t *f, const szr_entry_t *e) { return under(e->name, f->prefix); }

static void own_apply(fill_t *f, const char *rel) {
    for (unsigned i = 0; i < f->geo->nown; i++) {
        const chives_own_t *o = &f->geo->own[i];
        if (strcmp(o->path, rel)) continue;
        char p[SP];
        if (snprintf(p, sizeof(p), "%s/%s", f->mnt, rel) >= (int)sizeof(p)) return;
        if (chmod(p, o->mode) != 0) f->chmod_failed++;
        if (chown(p, (uid_t)o->uid, (gid_t)o->gid) != 0) { /* der Besitzer der Quelle ist meist 0:0; lehnt die Konsole ab, bleibt der, den die Datei hat */ }
        return;
    }
}

static int fill(fill_t *f, char *err, size_t errsz) {
    const unsigned n = szr_count(f->z);
    for (unsigned i = 0; i < n; i++) {
        const szr_entry_t *e = szr_at(f->z, i);
        if (prog_stopped(f->p)) return ECANCELED;
        if (e->is_dir || !in_root(f, e)) continue;
        const char *rel = e->name + f->pl;
        if (chives_is_sidecar(rel) || is_foreign_sidecar(rel)) continue;
        char p[SP];
        if (snprintf(p, sizeof(p), "%s/%s", f->mnt, rel) >= (int)sizeof(p)) return efail(err, errsz, "%s", T("Pfad zu lang"));
        {
            char dir[SP];
            snprintf(dir, sizeof(dir), "%s", p);
            char *sl = strrchr(dir, '/');
            if (sl) {
                *sl = 0;
                if (strlen(dir) > strlen(f->mnt)) {
                    const int me = mkdir_like_parent(dir);
                    if (me) return efail(err, errsz, T("Ein Ordner des Spielstands ließ sich nicht anlegen (%s): %s"), rel, strerror(me));
                }
            }
        }
        const int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) return efail(err, errsz, T("Eine Datei ließ sich im neuen Abbild nicht anlegen (%s): %s"), rel, strerror(errno ? errno : EIO));
        int rc;
        if (!strcmp(rel, "sce_sys/param.sfo")) {
            rc = write_all(fd, f->sfo, f->sfo_len) == 0 ? SZ_OK : SZ_E_IO;
            if (rc == SZ_OK) prog_work(f->p, f->sfo_len);
        } else {
            rc = szr_extract(f->z, i, fd, SI_MEM_MAX, prog_tick, f->p);
        }
        const int wrote_errno = errno;
        if (rc == SZ_OK && fsync(fd) != 0) rc = SZ_E_IO;
        if (close(fd) != 0 && rc == SZ_OK) rc = SZ_E_IO;
        if (rc == SZ_E_STOP) return ECANCELED;
        if (rc == SZ_E_CRC) return efail(err, errsz, T("Eine Datei in der ZIP-Datei ist beschädigt (%s): Die Prüfsumme stimmt nicht."), rel);
        if (rc == SZ_E_TOO_BIG) return efail(err, errsz, T("Eine Datei in der ZIP-Datei ist zu groß (%s)."), rel);
        if (rc != SZ_OK) return efail(err, errsz, T("Eine Datei ließ sich nicht in das neue Abbild schreiben (%s): %s"), rel, strerror(wrote_errno ? wrote_errno : EIO));
        if (f->ps4) {
            const unsigned mode = (e->attr >> 16) & 07777;
            if (mode && chmod(p, mode) != 0) f->chmod_failed++;
        }
    }
    /* Ordner aus der Besitzerliste, die keine Datei mitbrachte, dann Rechte und Besitzer von allem darin */
    for (unsigned i = 0; i < f->geo->nown; i++) {
        const chives_own_t *o = &f->geo->own[i];
        char p[SP];
        struct stat st;
        if (snprintf(p, sizeof(p), "%s/%s", f->mnt, o->path) >= (int)sizeof(p)) continue;
        if (stat(p, &st) != 0) {
            int in_zip = 0;
            for (unsigned k = 0; k < n && !in_zip; k++) {
                const szr_entry_t *e = szr_at(f->z, k);
                if (!e->is_dir && in_root(f, e) && !strcmp(e->name + f->pl, o->path)) in_zip = 1;
            }
            if (!in_zip) (void)mkdir_like_parent(p);   /* ein leerer Ordner der Quelle */
        }
        own_apply(f, o->path);
    }
    return 0;
}

/* jede Datei der ZIP aus dem eingehängten Abbild zurücklesen und vergleichen. 0, ECANCELED oder -1 */
static int verify_files(fill_t *f, char *err, size_t errsz) {
    const unsigned n = szr_count(f->z);
    uint8_t *buf = malloc(BUF);
    if (!buf) return efail(err, errsz, "%s", T("Kein Speicher"));
    int rc = 0;
    for (unsigned i = 0; i < n && rc == 0; i++) {
        const szr_entry_t *e = szr_at(f->z, i);
        if (e->is_dir || !in_root(f, e)) continue;
        const char *rel = e->name + f->pl;
        if (chives_is_sidecar(rel) || is_foreign_sidecar(rel)) continue;
        if (prog_stopped(f->p)) { rc = ECANCELED; break; }
        char p[SP];
        if (snprintf(p, sizeof(p), "%s/%s", f->mnt, rel) >= (int)sizeof(p)) { rc = efail(err, errsz, "%s", T("Pfad zu lang")); break; }
        const int fd = open(p, O_RDONLY);
        if (fd < 0) { rc = efail(err, errsz, T("Beim Prüfen fehlt eine Datei im neuen Abbild (%s)."), rel); break; }
        uint32_t crc = 0;
        uint64_t total = 0;
        for (;;) {
            ssize_t k = read(fd, buf, BUF);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { if (k < 0) rc = efail(err, errsz, T("Beim Prüfen ließ sich eine Datei nicht lesen (%s): %s"), rel, strerror(errno ? errno : EIO)); break; }
            crc = savezip_crc32(crc, buf, (size_t)k);
            total += (uint64_t)k;
            prog_work(f->p, (uint64_t)k);
        }
        close(fd);
        if (rc) break;
        if (!strcmp(rel, "sce_sys/param.sfo")) {
            if (total != f->sfo_len || crc != savezip_crc32(0, f->sfo, f->sfo_len)) rc = efail(err, errsz, "%s", T("Beim Prüfen weicht das param.sfo im neuen Abbild vom geschriebenen ab."));
        } else if (total != e->usize || crc != e->crc) {
            rc = efail(err, errsz, T("Beim Prüfen weicht eine Datei im neuen Abbild von der ZIP-Datei ab (%s)."), rel);
        }
    }
    free(buf);
    return rc;
}

/* Der Besitzer eines Spielstands (PS4 wie PS5) ist mehr als ACCOUNT_ID und Benutzer-ID: das 1024-Byte-Feld PARAMS trägt die
 * Benutzer-ID und einen 32-Byte-Wert, der in allen Spielständen dieses Benutzers gleich ist (an der Konsole gesehen: mit dem
 * Wert des Benutzers der anderen Konsole nahm das Spiel die Daten nicht an). Er lässt sich nicht erfinden, also kommt er aus
 * einem Spielstand desselben Benutzers auf dieser Konsole: derselbe Titel und Name zuerst (das ganze Feld), dann ein anderer
 * Name des Titels, dann ein anderer Titel derselben Art (nur Benutzer-ID und Wert, Bytes 4 bis 40), zuletzt einer der anderen
 * Art. Jeder Kandidat ist eine KOPIE, eingehängt, gelesen und wieder losgelassen. Fehlt ein Spender, scheitert das Einspielen
 * nicht: der Hinweis sagt, wie weit der Besitzer stimmt. 0, ECANCELED oder -1 (ein Spender ließ sich nicht lösen). */
typedef struct { char title[16]; char slot[100]; int rank; uint64_t size; int ps4; } donor_t;

static int cmp_donor(const void *x, const void *y) {
    const donor_t *a = x, *b = y;
    if (a->rank != b->rank) return a->rank - b->rank;
    return a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
}

#define DONOR_CAND 192   /* behaltene Kandidaten: der Titel des eingehenden Spielstands zuerst */

/* die Abbilder eines Titel-Ordners, die Spender sein können (der Ordner einer App wird nie geöffnet: ihre Daten bleiben unberührt) */
static void donor_scan_title(const iinfo_t *info, donor_t *cand, unsigned *nc, int dps4, const char *root, const char *title) {
    if (*nc >= DONOR_CAND || saves_title_is_app(title)) return;
    char tdir[SP];
    if (snprintf(tdir, sizeof(tdir), "%s/%s", root, title) >= (int)sizeof(tdir)) return;
    DIR *sd = opendir(tdir);
    while (sd && *nc < DONOR_CAND) {
        struct dirent *se = readdir(sd);
        if (!se) break;
        if (strncmp(se->d_name, "sdimg_", 6) || !slot_ok(se->d_name + 6)) continue;
        char bin[SP], img[SP];
        struct stat st, ist;
        if (dps4 && (snprintf(bin, sizeof(bin), "%s/%s.bin", tdir, se->d_name + 6) >= (int)sizeof(bin) || stat(bin, &st) != 0)) continue;   /* PS4: nur mit Schlüssel */
        if (snprintf(img, sizeof(img), "%s/%s", tdir, se->d_name) >= (int)sizeof(img) || stat(img, &ist) != 0 || !S_ISREG(ist.st_mode) ||
            (uint64_t)ist.st_size > DONOR_MAX)
            continue;
        donor_t *d = &cand[(*nc)++];
        d->size = (uint64_t)ist.st_size;
        d->ps4 = dps4;
        scpy(d->title, sizeof(d->title), title);
        scpy(d->slot, sizeof(d->slot), se->d_name + 6);
        d->rank = dps4 != info->ps4 ? 3 : strcmp(title, info->title_id) ? 2 : strcmp(se->d_name + 6, info->slot) ? 1 : 0;
    }
    if (sd) closedir(sd);
}

/* das PARAMS-Feld eines Spender-param.sfo, wenn es zu Konto und Benutzer passt und einen Besitzerwert trägt: 1 = übernommen */
static int donor_params(const uint8_t *buf, size_t n, uint64_t account_id, uint32_t user_id, uint8_t params[1024]) {
    uint8_t aid[8], pr[1024];
    if (!savesfo_valid(buf, n) || savesfo_get_raw(buf, n, "ACCOUNT_ID", aid, 8) != 8 || le_num(aid, 8) != account_id ||
        savesfo_get_raw(buf, n, "PARAMS", pr, sizeof(pr)) != 1024 || (uint32_t)le_num(pr + 4, 4) != user_id)
        return 0;
    int nz = 0;
    for (int k = 8; k < 40; k++) nz |= pr[k];
    if (!nz) return 0;
    memcpy(params, pr, sizeof(pr));
    return 1;
}

static int owner_donor(const char *home, const char *uid, const char *work, uint64_t account_id, uint32_t user_id, const iinfo_t *info,
                       uint8_t *sfo, size_t sfo_len, prog_t *p, char *note, size_t note_len, char *err, size_t errsz) {
    note[0] = 0;
    /* der Bereich der eigenen Art zuerst (PS4 bei einem PS4-Spielstand): sonst füllten viele Spielstände der anderen Art die Liste,
     * bevor die besten Spender (derselbe Titel) drankämen */
    typedef struct { const char *dir; int ps4; } area_t;
    static const area_t k_ps5_first[2] = { { SAVE_SUBDIR, 0 }, { SAVE_SUBDIR_PS4, 1 } },
                        k_ps4_first[2] = { { SAVE_SUBDIR_PS4, 1 }, { SAVE_SUBDIR, 0 } };
    const area_t *k_areas = info->ps4 ? k_ps4_first : k_ps5_first;
    donor_t *cand = calloc(DONOR_CAND, sizeof(*cand));   /* auf dem Heap: das läuft in einem Auftrags-Faden mit kleinem Stack */
    if (!cand) return efail(err, errsz, "%s", T("Kein Speicher"));
    unsigned nc = 0;
    for (int ai = 0; ai < 2; ai++) {
        const int dps4 = k_areas[ai].ps4;
        char root[SP];
        if (snprintf(root, sizeof(root), "%s/%s/%s", home, uid, k_areas[ai].dir) >= (int)sizeof(root)) continue;
        if (dps4 == info->ps4) donor_scan_title(info, cand, &nc, dps4, root, info->title_id);   /* der eigene Titel zuerst: die Grenze schneidet nie die besten */
        DIR *d = opendir(root);
        while (d) {
            struct dirent *te = readdir(d);
            if (!te) break;
            if (dps4 ? !cusa_ok(te->d_name) : !ppsa_ok(te->d_name)) continue;
            if (dps4 == info->ps4 && !strcmp(te->d_name, info->title_id)) continue;   /* schon oben */
            donor_scan_title(info, cand, &nc, dps4, root, te->d_name);
        }
        if (d) closedir(d);
    }
    if (nc > 1) qsort(cand, nc, sizeof(cand[0]), cmp_donor);   /* der beste Rang zuerst, darin das kleinste Abbild */
    uint8_t params[1024];
    int got = -1, rc = 0;
    unsigned tries = 0;
    char ddir[SP], dmnt[SP];
    if (snprintf(ddir, sizeof(ddir), "%s/donor", work) >= (int)sizeof(ddir) || snprintf(dmnt, sizeof(dmnt), "%s/donor_mnt", work) >= (int)sizeof(dmnt)) {
        free(cand);
        return 0;
    }
    for (int rank = 0; rank <= 3 && got < 0 && tries < 8 && rc == 0; rank++) {
        for (unsigned i = 0; i < nc && got < 0 && tries < 8; i++) {
            if (cand[i].rank != rank) continue;
            if (prog_stopped(p)) { rc = ECANCELED; break; }
            tries++;
            char src[SP], bin[SP], dimg[SP];
            uint8_t sealed[SAVEMNT_SEALED_BYTES];
            memset(sealed, 0, sizeof(sealed));
            const char *adir = cand[i].ps4 ? SAVE_SUBDIR_PS4 : SAVE_SUBDIR;
            if (snprintf(src, sizeof(src), "%s/%s/%s/%s/sdimg_%s", home, uid, adir, cand[i].title, cand[i].slot) >= (int)sizeof(src) ||
                snprintf(bin, sizeof(bin), "%s/%s/%s/%s/%s.bin", home, uid, adir, cand[i].title, cand[i].slot) >= (int)sizeof(bin) ||
                snprintf(dimg, sizeof(dimg), "%s/sdimg_%s", ddir, cand[i].slot) >= (int)sizeof(dimg))
                continue;
            if (cand[i].ps4) {
                const int bfd = open(bin, O_RDONLY);
                const ssize_t bn = bfd >= 0 ? read(bfd, sealed, sizeof(sealed)) : -1;
                if (bfd >= 0) close(bfd);
                if (bn != (ssize_t)sizeof(sealed)) continue;
            }
            mkdir(ddir, 0755);
            {
                uint64_t room = 0;   /* die Kopie des Spenders braucht eigenen Platz: zu wenig heißt kein Spender, keine halbe Kopie */
                if (free_space(work, &room) != 0 || room < cand[i].size + MIN_FREE) continue;
            }
            if (copy_file(NULL, src, dimg, (uint64_t)-1, 0755) != 0) { unlink(dimg); continue; }
            char e2[300];
            int valid = 0;
            if (!cand[i].ps4) {
                /* neuere PS5-Spielstände tragen das echte param.sfo nur im Kopf des Abbilds (das im Dateisystem ist leer): dort zuerst
                 * nachsehen, das spart auch das Einhängen */
                uint8_t *hb = malloc(SAVEMNT_PARAM_BYTES);
                uint32_t hn = 0;
                char e3[480];
                if (hb && savemnt_param_read(dimg, 0, hb, SAVEMNT_PARAM_BYTES, &hn, e3, sizeof(e3)) == 0)
                    valid = donor_params(hb, hn, account_id, user_id, params);
                free(hb);
            }
            mkdir(dmnt, 0755);
            savemnt_t dm;
            if (!valid && (cand[i].ps4 ? savemnt_open_sealed(dimg, sealed, dmnt, &dm, e2, sizeof(e2)) : savemnt_open(dimg, dmnt, &dm, e2, sizeof(e2))) == 0) {
                char sp[SP];
                uint8_t *buf = malloc(SI_MAX_SFO);
                if (buf && snprintf(sp, sizeof(sp), "%s/sce_sys/param.sfo", dmnt) < (int)sizeof(sp)) {
                    const int fd = open(sp, O_RDONLY);
                    const ssize_t n = fd >= 0 ? read(fd, buf, SI_MAX_SFO) : -1;
                    if (fd >= 0) close(fd);
                    if (n > 0) valid = donor_params(buf, (size_t)n, account_id, user_id, params);
                }
                free(buf);
                if (savemnt_close(&dm, e2, sizeof(e2)) != 0) {
                    efail(err, errsz, T("%s Es wurde nichts verändert; bitte die PS5 neu starten, bevor es noch einmal versucht wird."), e2);
                    free(cand);
                    return -1;
                }
            }
            rmdir(dmnt);
            unlink(dimg);
            if (valid) got = rank;
        }
    }
    rmdir(ddir);
    free(cand);
    if (rc == ECANCELED) return ECANCELED;
    /* passt das PARAMS-Feld des Spielstands nicht (kürzer als der Spenderwert), zählt das wie kein Spender: lieber der Hinweis als still nichts */
    if (got == 0 && savesfo_set_raw(sfo, sfo_len, "PARAMS", 0, params, 1024) != 0) got = -1;
    if (got > 0 && savesfo_set_raw(sfo, sfo_len, "PARAMS", 4, params + 4, 36) != 0) got = -1;
    if (got == 0) {
        /* der eigene Spielstand: ganz übernommen, kein Hinweis */
    } else if (got > 0) {
        snprintf(note, note_len, "%s", T("Die Besitzerangaben stammen aus einem anderen Spielstand dieses Benutzers und sind nicht vollständig geprüft: Meldet das Spiel beschädigte Daten, liegt es vermutlich daran."));
    } else {
        snprintf(note, note_len, "%s", T("Von diesem Benutzer war kein Spielstand zu finden, dessen Besitzerangaben sich übernehmen ließen. Der Spielstand ist eingespielt; das Spiel meldet vielleicht beschädigte Daten."));
    }
    savesfo_patch_owner(sfo, sfo_len, account_id, 1, user_id);   /* Konto und Benutzer noch einmal, über das, was das Feld brachte */
    return 0;
}

static void sync_dir_of(const char *file) {
    char d[SP];
    scpy(d, sizeof(d), file);
    char *sl = strrchr(d, '/');
    if (!sl || sl == d) return;
    *sl = 0;
    const int fd = open(d, O_RDONLY);
    if (fd >= 0) { (void)fsync(fd); close(fd); }
}

/* ── Aufzeichnung eines Tauschs (wie das Cooling Center seit 1.88.0, saveimport.c) ──
 * Vom ersten Umbenennen des Tauschs bis zu seinem Ende liegt eine kleine Aufzeichnung (Pfade, was vorher da war, die Zeile der Datenbank,
 * die Phase), jedes Mal ganz geschrieben und dauerhaft gemacht. Bricht ein Lauf in dieser Zeit ab (Strom, Absturz, neue Fassung des
 * Formatierers), bringt saves_import_recover beim nächsten Start (und vor dem nächsten Einspielen) zu Ende oder nimmt zurück, was sie
 * nennt: Phase "swap" und "rollback": die Dateien zurück wie vorher; Phase "db" (die Abbilder liegen, die Zeile fehlt noch): die Zeile
 * noch einmal schreiben (dieselben Werte) und die alten Dateien entfernen. Bei "rollback" nennt sie auch, was mit der Datenbank
 * geschieht: dbb = die Kopie von vorher kommt zurück, db_new = die Datenbank gab es vorher nicht (sie wird wieder entfernt). */
typedef struct {
    char fin[SP], ifin[SP], dbp[SP], dbb[SP];
    int ps4, replaced, bin_replaced, icon_existed, db_new;
    savedb_row_t row;
} jr_t;

static void journal_path(char *out, size_t n) {
    const char *d = plat_data_dir();
    if (!d || path_fmt(out, n, "%s/save_import.journal", d)) out[0] = 0;
}

static void jr_hex(cJSON *o, const char *key, uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%llx", (unsigned long long)v);
    cJSON_AddStringToObject(o, key, b);
}
static uint64_t jr_hex_get(const cJSON *o, const char *key) {
    const cJSON *i = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(i) && i->valuestring ? (uint64_t)strtoull(i->valuestring, NULL, 16) : 0;
}
static void jr_str_get(const cJSON *o, const char *key, char *out, size_t n) {
    const cJSON *i = cJSON_GetObjectItem(o, key);
    snprintf(out, n, "%s", cJSON_IsString(i) && i->valuestring ? i->valuestring : "");
}
static int jr_int_get(const cJSON *o, const char *key) {
    const cJSON *i = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(i) ? (int)i->valuedouble : 0;
}

/* ganz und dauerhaft (Zwischendatei, fsync, umbenennen, fsync des Ordners). 0 oder errno */
static int journal_write(const char *path, const jr_t *j, const char *phase) {
    if (!path || !path[0]) return 0;
    cJSON *o = cJSON_CreateObject();
    if (!o) return ENOMEM;
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "phase", phase);
    cJSON_AddStringToObject(o, "fin", j->fin);
    cJSON_AddStringToObject(o, "ifin", j->ifin);
    cJSON_AddStringToObject(o, "dbp", j->dbp);
    cJSON_AddStringToObject(o, "dbb", j->dbb);
    cJSON_AddNumberToObject(o, "db_new", j->db_new);
    cJSON_AddNumberToObject(o, "ps4", j->ps4);
    cJSON_AddNumberToObject(o, "replaced", j->replaced);
    cJSON_AddNumberToObject(o, "bin_replaced", j->bin_replaced);
    cJSON_AddNumberToObject(o, "icon_existed", j->icon_existed);
    cJSON_AddStringToObject(o, "title_id", j->row.title_id);
    cJSON_AddStringToObject(o, "dir_name", j->row.dir_name);
    cJSON_AddStringToObject(o, "main_title", j->row.main_title);
    cJSON_AddStringToObject(o, "sub_title", j->row.sub_title);
    cJSON_AddStringToObject(o, "detail", j->row.detail);
    cJSON_AddStringToObject(o, "mtime", j->row.mtime);
    jr_hex(o, "account_id", j->row.account_id);
    jr_hex(o, "user_id", j->row.user_id);
    jr_hex(o, "user_param", j->row.user_param);
    jr_hex(o, "blocks", j->row.blocks);
    jr_hex(o, "free_blocks", j->row.free_blocks);
    jr_hex(o, "size_kib", j->row.size_kib);
    jr_hex(o, "system_blocks", j->row.system_blocks);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return ENOMEM;
    char tmp[SP], dir[SP];
    if (path_fmt(tmp, sizeof(tmp), "%s.tmp", path) || path_fmt(dir, sizeof(dir), "%s", path)) { free(txt); return ENAMETOOLONG; }
    char *sl = strrchr(dir, '/');
    if (sl && sl != dir) { *sl = 0; (void)mkdir_like_parent(dir); }
    int rc = 0;
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) rc = errno ? errno : EIO;
    else {
        if (write_all(fd, txt, strlen(txt)) != 0 || fsync(fd) != 0) rc = errno ? errno : EIO;
        if (close(fd) != 0 && rc == 0) rc = errno ? errno : EIO;
    }
    free(txt);
    if (rc == 0 && rename(tmp, path) != 0) rc = errno ? errno : EIO;
    if (rc != 0) { unlink(tmp); return rc; }
    if (sl && sl != dir) {
        const int dfd = open(dir, O_RDONLY);
        if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
    }
    return 0;
}

/* 0, oder -1 wenn sie nicht lesbar ist oder etwas nennt, das dieser Code nie anfasst */
static int journal_read(const char *path, jr_t *j, char *phase, size_t phase_len) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 65536) return -1;
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) return -1;
    const int fd = open(path, O_RDONLY);
    const ssize_t n = fd >= 0 ? read(fd, buf, (size_t)st.st_size) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) { free(buf); return -1; }
    buf[n] = 0;
    cJSON *o = cJSON_Parse(buf);
    free(buf);
    if (!o) return -1;
    memset(j, 0, sizeof(*j));
    if (jr_int_get(o, "v") != 1) { cJSON_Delete(o); return -1; }
    jr_str_get(o, "phase", phase, phase_len);
    jr_str_get(o, "fin", j->fin, sizeof(j->fin));
    jr_str_get(o, "ifin", j->ifin, sizeof(j->ifin));
    jr_str_get(o, "dbp", j->dbp, sizeof(j->dbp));
    jr_str_get(o, "dbb", j->dbb, sizeof(j->dbb));
    j->db_new = jr_int_get(o, "db_new") != 0;
    j->ps4 = jr_int_get(o, "ps4") != 0;
    j->replaced = jr_int_get(o, "replaced") != 0;
    j->bin_replaced = jr_int_get(o, "bin_replaced") != 0;
    j->icon_existed = jr_int_get(o, "icon_existed") != 0;
    jr_str_get(o, "title_id", j->row.title_id, sizeof(j->row.title_id));
    jr_str_get(o, "dir_name", j->row.dir_name, sizeof(j->row.dir_name));
    jr_str_get(o, "main_title", j->row.main_title, sizeof(j->row.main_title));
    jr_str_get(o, "sub_title", j->row.sub_title, sizeof(j->row.sub_title));
    jr_str_get(o, "detail", j->row.detail, sizeof(j->row.detail));
    jr_str_get(o, "mtime", j->row.mtime, sizeof(j->row.mtime));
    j->row.account_id = jr_hex_get(o, "account_id");
    j->row.user_id = (uint32_t)jr_hex_get(o, "user_id");
    j->row.user_param = (uint32_t)jr_hex_get(o, "user_param");
    j->row.blocks = jr_hex_get(o, "blocks");
    j->row.free_blocks = jr_hex_get(o, "free_blocks");
    j->row.size_kib = jr_hex_get(o, "size_kib");
    j->row.system_blocks = jr_hex_get(o, "system_blocks");
    cJSON_Delete(o);
    /* geglaubt wird nur, was dieser Code anlegt: ein Abbild "sdimg_<Name>" dieses Namens, sein Bild, die Spielstand-Datenbank */
    const char *base = strrchr(j->fin, '/');
    char want[128];
    const size_t fl = strlen(j->fin), il = strlen(j->ifin), dl = strlen(j->dbp);
    if (!base || j->fin[0] != '/' || strstr(j->fin, "/..") || !slot_ok(j->row.dir_name) ||
        snprintf(want, sizeof(want), "sdimg_%s", j->row.dir_name) >= (int)sizeof(want) || strcmp(base + 1, want))
        return -1;
    if (j->ifin[0] != '/' || strstr(j->ifin, "/..") || il < 11 || strcmp(j->ifin + il - 10, "_icon0.png")) return -1;
    if (j->dbp[0] != '/' || strstr(j->dbp, "/..") || dl < 12 || strcmp(j->dbp + dl - 11, "savedata.db")) return -1;
    if (fl + 14 >= sizeof(j->fin) || il + 14 >= sizeof(j->ifin)) return -1;
    if (j->dbb[0]) {
        const size_t bl = strlen(j->dbb);
        if (j->dbb[0] != '/' || strstr(j->dbb, "/..") || !strstr(j->dbb, "/save_db_backup/savedata-") || bl < 4 || strcmp(j->dbb + bl - 3, ".db"))
            return -1;
    }
    return 0;
}

int saves_import_recover(char *report, size_t report_len) {
    if (report_len) report[0] = 0;
    char path[SP];
    struct stat st;
    journal_path(path, sizeof(path));
    if (!path[0] || lstat(path, &st) != 0) return 0;
    jr_t *j = calloc(1, sizeof(*j));
    if (!j) { snprintf(report, report_len, "%s", T("Kein Speicher")); return -1; }
    char phase[16] = "";
    if (journal_read(path, j, phase, sizeof(phase)) != 0) {
        free(j);
        unlink(path);
        snprintf(report, report_len, "%s", T("Die Aufzeichnung eines unterbrochenen Einspielens war nicht lesbar und wurde entfernt."));
        return 1;
    }
    {   /* ein laufendes Spiel kann den Spielstand gerade benutzen: dann nichts umbenennen - das nächste Einspielen versucht es wieder */
        char gid[32] = "";
        if (g_game_check && g_game_check(gid, sizeof(gid))) {
            snprintf(report, report_len, T("Ein unterbrochenes Einspielen von %.15s (%.60s) wird erst zu Ende gebracht, wenn kein Spiel läuft (gerade: %.15s)."),
                     j->row.title_id, j->row.dir_name, gid);
            free(j);
            return -1;
        }
    }
    char neu[SP], alt[SP], ineu[SP], ialt[SP], dir[SP], bfin[SP], bneu[SP], balt[SP];
    path_fmt(neu, sizeof(neu), "%s.usbfmt-neu", j->fin);
    path_fmt(alt, sizeof(alt), "%s.usbfmt-alt", j->fin);
    path_fmt(ineu, sizeof(ineu), "%s.usbfmt-neu", j->ifin);
    path_fmt(ialt, sizeof(ialt), "%s.usbfmt-alt", j->ifin);
    scpy(dir, sizeof(dir), j->fin);
    *strrchr(dir, '/') = 0;
    if (path_fmt(bfin, sizeof(bfin), "%s/%s.bin", dir, j->row.dir_name)) bfin[0] = 0;
    path_fmt(bneu, sizeof(bneu), "%s.usbfmt-neu", bfin);
    path_fmt(balt, sizeof(balt), "%s.usbfmt-alt", bfin);
    /* Lag das neue Abbild schon an seinem Platz und hat ein Spiel es seit dem Abbruch benutzt (jünger als die Aufzeichnung),
     * steckt darin womöglich neuer Fortschritt: dann nicht zurücknehmen, sondern zu Ende führen (der Rest ist die Zeile).
     * Ein PS4-Abbild braucht dazu seinen neuen Schlüssel: liegt der noch daneben, bleibt es beim Zurücknehmen. */
    if (!strcmp(phase, "swap") && lstat(neu, &st) != 0 && !(j->ps4 && bfin[0] && lstat(bneu, &st) == 0)) {
        struct stat fs, js;
        if (lstat(j->fin, &fs) == 0 && S_ISREG(fs.st_mode) && lstat(path, &js) == 0 && fs.st_mtime > js.st_mtime + 2) {
            if (lstat(ineu, &st) == 0) (void)rename(ineu, j->ifin);   /* das Bild gehört zum neuen Stand */
            scpy(phase, sizeof(phase), "db");
        }
    }
    if (!strcmp(phase, "db") && lstat(j->fin, &st) == 0 && lstat(neu, &st) != 0) {
        /* die Abbilder liegen: übrig ist die Zeile - dieselben Werte noch einmal, ob der erste Versuch durchkam, spielt keine Rolle */
        char e2[300] = "";
        int inserted = 0;
        int rc = savedb_upsert(j->dbp, &j->row, &inserted, e2, sizeof(e2));
        if (rc == 0) rc = savedb_check(j->dbp, e2, sizeof(e2));
        if (rc != 0) {
            snprintf(report, report_len, T("Ein unterbrochenes Einspielen von %.15s (%.60s) ist fast zu Ende: Nur der Eintrag in der Spielstand-Datenbank fehlt noch (%.200s). Das nächste Einspielen versucht es noch einmal."),
                     j->row.title_id, j->row.dir_name, e2);
            free(j);
            return -1;
        }
        unlink(alt);
        unlink(ialt);
        if (j->ps4 && bfin[0]) unlink(balt);
        unlink(neu);
        unlink(ineu);
        if (j->ps4 && bfin[0]) unlink(bneu);
        snprintf(report, report_len, T("Ein unterbrochenes Einspielen von %.15s (%.60s) wurde zu Ende geführt: Der Spielstand ist eingespielt."), j->row.title_id, j->row.dir_name);
    } else {
        /* die Dateien zurück wie vorher: das alte Abbild, wo es beiseitegelegt war, nichts, wo vorher nichts war (Schlüssel und Bild ebenso) */
        struct stat s2;
        int bad = 0;   /* ein Zurücklegen misslang: dann bleibt die Aufzeichnung, und nichts wird als erledigt gemeldet */
        if (j->ps4 && bfin[0]) {   /* erst der Schlüssel, dann das Abbild (wie beim Zurücknehmen im Lauf) */
            if (lstat(balt, &s2) == 0) { if (rename(balt, bfin) != 0) bad = errno ? errno : EIO; }
            else if (!j->bin_replaced) unlink(bfin);
        }
        if (lstat(alt, &s2) == 0) { if (rename(alt, j->fin) != 0) bad = errno ? errno : EIO; }
        else if (!j->replaced) unlink(j->fin);
        if (lstat(ialt, &s2) == 0) { if (rename(ialt, j->ifin) != 0) bad = errno ? errno : EIO; }
        else if (!j->icon_existed) unlink(j->ifin);
        if (!bad && !strcmp(phase, "rollback") && (j->dbb[0] || j->db_new)) {
            /* die Zeile war schon geschrieben: die Datenbank wie vorher (die Kopie zurück bzw. die neu angelegte weg) */
            if (j->db_new) unlink(j->dbp);
            else {
                char dneu[SP];
                if (path_fmt(dneu, sizeof(dneu), "%s.usbfmt-neu", j->dbp) || copy_file(NULL, j->dbb, dneu, (uint64_t)-1, 0644) != 0 || rename(dneu, j->dbp) != 0)
                    bad = errno ? errno : EIO;
            }
        }
        if (bad) {
            snprintf(report, report_len, T("Ein unterbrochenes Einspielen von %.15s (%.60s) ließ sich nicht zurücknehmen (%s). Der nächste Start versucht es noch einmal; bis dahin bitte diesen Spielstand nicht benutzen."),
                     j->row.title_id, j->row.dir_name, strerror(bad));
            free(j);
            return -1;
        }
        unlink(neu);
        unlink(ineu);
        if (j->ps4 && bfin[0]) unlink(bneu);
        snprintf(report, report_len, T("Ein unterbrochenes Einspielen von %.15s (%.60s) wurde zurückgenommen: Der alte Spielstand liegt wieder an seinem Platz."), j->row.title_id, j->row.dir_name);
    }
    free(j);
    unlink(path);
    return 1;
}

/* Aufräumen beim Start und zu Beginn jedes Spielstand-Auftrags (wie das Cooling Center seit 1.88.0): von den Kopien der Spielstand-Datenbank
 * (save_db_backup/savedata-<uid>-...db) bleiben je Benutzer die zehn neuesten. Ein laufender Auftrag ist dann nie dabei (einer zugleich). */
typedef struct { char name[128]; char uid[9]; time_t t; } dbk_t;
static int cmp_dbk(const void *a, const void *b) {
    const dbk_t *x = a, *y = b;
    const int c = strcmp(x->uid, y->uid);
    if (c) return c;
    if (x->t != y->t) return x->t < y->t ? 1 : -1;   /* die neueste eines Benutzers zuerst */
    return strcmp(y->name, x->name);
}
void saves_housekeeping(void) {
    const char *data = plat_data_dir();
    char dir[SP];
    if (!data || path_fmt(dir, sizeof(dir), "%s/save_db_backup", data)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    dbk_t *v = calloc(1024, sizeof(*v));
    unsigned n = 0;
    struct dirent *e;
    while (v && n < 1024 && (e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        const size_t l = strlen(nm);
        char full[SP];
        struct stat st;
        if (l < 20 || l >= sizeof(v[0].name) || strncmp(nm, "savedata-", 9) || strcmp(nm + l - 3, ".db") || nm[17] != '-') continue;
        memcpy(v[n].uid, nm + 9, 8);
        v[n].uid[8] = 0;
        if (!uid_ok(v[n].uid) || path_fmt(full, sizeof(full), "%s/%s", dir, nm) || lstat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        scpy(v[n].name, sizeof(v[n].name), nm);
        v[n].t = st.st_mtime;
        n++;
    }
    closedir(d);
    if (n > 1) qsort(v, n, sizeof(*v), cmp_dbk);
    unsigned kept = 0;
    for (unsigned i = 0; i < n; i++) {
        kept = i && !strcmp(v[i].uid, v[i - 1].uid) ? kept + 1 : 1;
        if (kept <= 10) continue;
        char full[SP];
        if (!path_fmt(full, sizeof(full), "%s/%s", dir, v[i].name)) unlink(full);
    }
    free(v);
}

/* "*.usbfmt-neu" in einem Ordner entfernen (nur Dateien, keinen Verknüpfungen folgen): Anzahl */
static int unlink_neu_in(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t l = strlen(e->d_name);
        char full[SP];
        struct stat st;
        if (l <= 11 || strcmp(e->d_name + l - 11, ".usbfmt-neu") || path_fmt(full, sizeof(full), "%s/%s", dir, e->d_name)) continue;
        if (lstat(full, &st) == 0 && S_ISREG(st.st_mode) && unlink(full) == 0) n++;
    }
    closedir(d);
    return n;
}

int saves_cleanup_stale(void) {
    /* liegt eine Aufzeichnung, gehören die Reste zu ihr (saves_import_recover entscheidet) */
    char jp[SP];
    struct stat st;
    journal_path(jp, sizeof(jp));
    if (!jp[0] || lstat(jp, &st) == 0) return 0;
    const char *home = plat_saves_dir(), *sys = plat_sys_data_dir();
    int n = 0;
    DIR *ud = home ? opendir(home) : NULL;
    struct dirent *ue;
    while (ud && (ue = readdir(ud)) != NULL) {
        if (!uid_ok(ue->d_name)) continue;
        for (int ps4 = 0; ps4 < 2; ps4++) {
            /* Abbilder und Schlüssel: <home>/<uid>/<Bereich>/<Titel>/; Bilder: <home>/<uid>/<Bereich>_meta/user/<Titel>/ */
            for (int meta = 0; meta < 2; meta++) {
                char area[SP];
                if (path_fmt(area, sizeof(area), meta ? "%s/%s/%s/user" : "%s/%s/%s", home, ue->d_name,
                             meta ? (ps4 ? "savedata_meta" : "savedata_prospero_meta") : (ps4 ? SAVE_SUBDIR_PS4 : SAVE_SUBDIR)))
                    continue;
                DIR *td = opendir(area);
                struct dirent *te;
                while (td && (te = readdir(td)) != NULL) {
                    char tdir[SP];
                    if (!(ps4 ? cusa_ok(te->d_name) : ppsa_ok(te->d_name)) || path_fmt(tdir, sizeof(tdir), "%s/%s", area, te->d_name)) continue;
                    n += unlink_neu_in(tdir);
                }
                if (td) closedir(td);
            }
            char dbdir[SP];   /* die Spielstand-Datenbank: <sys>/<Bereich>/<uid>/db/user/ */
            if (sys && !path_fmt(dbdir, sizeof(dbdir), "%s/%s/%s/db/user", sys, ps4 ? SAVE_SUBDIR_PS4 : SAVE_SUBDIR, ue->d_name)) n += unlink_neu_in(dbdir);
        }
    }
    if (ud) closedir(ud);
    return n;
}

/* einen Spielstand (Ordner prefix der ZIP) für den Benutzer uid einspielen. Abschnitte: Promille a..b von p.
 * 0, ECANCELED oder -1 (err); note = Hinweise trotz Erfolg */
static int import_root(szr_t *z, const char *prefix, const char *uid, prog_t *p, unsigned a, unsigned b, char *note, size_t notesz,
                       char *err, size_t errsz) {
    const char *home = plat_saves_dir(), *sys = plat_sys_data_dir(), *work = saves_workdir(), *data = plat_data_dir();
    iinfo_t info;
    chives_t geo;
    uint8_t *sfo = NULL;
    size_t sfo_len = 0;
    int rc = -1, mounted = 0, stuck = 0, have_img = 0, ps4 = 0;
    char img[SP] = "", mnt[SP] = "", icon_tmp[SP] = "", bin_tmp[SP] = "", username[64] = "";
    uint8_t sealed[SAVEMNT_SEALED_BYTES];
    char owner_note[600] = "", recover_note[600] = "", jpath[SP] = "";
    int jw = 0;      /* die Aufzeichnung des Tauschs liegt */
    int rb_ok = 1;   /* jedes Zurücklegen gelang (sonst bleibt die Aufzeichnung für den nächsten Start) */
    jr_t *jrec = NULL;
    savemnt_t mt;
    memset(&mt, 0, sizeof(mt));
    memset(sealed, 0, sizeof(sealed));
    note[0] = 0;
    const unsigned span = b - a;
#define SEG(x, y) (a + span * (x) / 100), (a + span * (y) / 100)

    {   /* ein Tausch, den ein früherer Lauf nicht beendet hat, zuerst zu Ende bringen oder zurücknehmen; geht das noch nicht, beginnt nichts Neues */
        char rn[600];
        const int jr = saves_import_recover(rn, sizeof(rn));
        if (jr < 0) { efail(err, errsz, T("Von einem früheren Einspielen ist noch etwas offen. %s Es wurde nichts verändert."), rn); goto done; }
        if (jr > 0) scpy(recover_note, sizeof(recover_note), rn);
    }
    inspect_root(z, prefix, &info, &geo, &sfo, &sfo_len);
    if (!info.importable) { efail(err, errsz, "%s", info.why); goto done; }
    ps4 = info.ps4;
    if (!home || !sys) { efail(err, errsz, "%s", T("Die Spielstände der PS5 sind nicht erreichbar.")); goto done; }
    read_username(home, uid, username, sizeof(username));
    uint64_t account_id = 0;
    const int have_account = plat_account_id(username, &account_id) == 0;
    const uint32_t user_id = (uint32_t)strtoul(uid, NULL, 16);
    if (path_fmt(img, sizeof(img), "%s/sdimg_%s", work, info.slot) || path_fmt(mnt, sizeof(mnt), "%s/mnt", work) ||
        path_fmt(icon_tmp, sizeof(icon_tmp), "%s/icon0.png", work) || path_fmt(bin_tmp, sizeof(bin_tmp), "%s/key.bin", work)) {
        efail(err, errsz, "%s", T("Pfad zu lang"));
        goto done;
    }

    /* der Besitzer im param.sfo: Konto und Benutzer dieser Konsole */
    if (savesfo_patch_owner(sfo, sfo_len, account_id, 1, user_id) != 0) {
        efail(err, errsz, "%s", T("Im param.sfo lassen sich das Konto und der Benutzer nicht eintragen (ACCOUNT_ID fehlt oder ist zu kurz)."));
        goto done;
    }
    if (info.converted && (sfo_set_num(sfo, sfo_len, "SAVEDATA_BLOCKS", info.block_count) != 0 || sfo_set_num(sfo, sfo_len, "SYSTEM_BLOCKS", 0) != 0)) {
        efail(err, errsz, "%s", T("Im param.sfo lassen sich die Blockzahlen für das klassische Abbild nicht eintragen."));
        goto done;
    }

    /* Platz, Reste */
    {
        uint64_t avail = 0;
        const uint64_t need = 2 * info.image_size + MIN_FREE;
        if (free_space(work, &avail) != 0) { efail(err, errsz, "%s", T("Der freie Platz ließ sich nicht ermitteln.")); goto done; }
        if (avail < need) {
            efail(err, errsz, T("Auf der internen Platte ist zu wenig Platz: gebraucht werden etwa %llu MB, frei sind %llu MB."),
                  (unsigned long long)(need >> 20), (unsigned long long)(avail >> 20));
            goto done;
        }
        if (work_clean(work, err, errsz) != 0) goto done;
    }
    if (prog_stopped(p)) { rc = ECANCELED; goto done; }

    /* der Besitzer aus einem Spielstand desselben Benutzers */
    prog_phase(p, SEG(0, 10), 0);
    {
        const int orc = owner_donor(home, uid, work, account_id, user_id, &info, sfo, sfo_len, p, owner_note, sizeof(owner_note), err, errsz);
        if (orc == ECANCELED) { rc = ECANCELED; goto done; }
        if (orc != 0) { stuck = 1; goto done; }
    }
    if (!have_account) {
        const size_t on = strlen(owner_note);
        snprintf(owner_note + on, sizeof(owner_note) - on, "%s%s", on ? "\n" : "",
                 T("Das Konto des Benutzers war in der Registry der PS5 nicht zu finden; der Spielstand trägt das Konto 0."));
    }
    if (prog_stopped(p)) { rc = ECANCELED; goto done; }

    /* das neue Abbild */
    prog_phase(p, SEG(10, 15), 0);
    {
        char e2[400];
        if ((ps4 ? savemnt_create_ps4(img, info.image_size, sealed, e2, sizeof(e2)) : savemnt_create(img, info.image_size, info.block_size, e2, sizeof(e2))) != 0) {
            efail(err, errsz, "%s", e2);
            goto done;
        }
        have_img = 1;
        if (ps4) {   /* der gebundene Schlüssel des neuen Abbilds kommt nach <Name>.bin daneben */
            uint8_t head4[0x40];
            struct stat st4;
            int fd4 = open(img, O_RDONLY);
            const ssize_t hn4 = fd4 >= 0 ? pread(fd4, head4, sizeof(head4), 0) : -1;
            const int s4 = fd4 >= 0 && fstat(fd4, &st4) == 0;
            if (fd4 >= 0) close(fd4);
            unlink(bin_tmp);
            int bfd = open(bin_tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
            int bok = bfd >= 0 && write_all(bfd, sealed, sizeof(sealed)) == 0 && fsync(bfd) == 0;
            if (bfd >= 0 && close(bfd) != 0) bok = 0;
            if (hn4 < 0x40 || !s4 || savemnt_header_kind(head4, (size_t)hn4) != 1 || (uint64_t)st4.st_size != info.image_size || !bok) {
                efail(err, errsz, "%s", T("Das neue Abbild hat nicht den Aufbau, den die ZIP-Datei beschreibt (die Konsole hat es anders angelegt). Es wurde nichts verändert."));
                goto done;
            }
        } else {
            uint8_t head[0x370];
            int fd = open(img, O_RDONLY);
            ssize_t hn = fd >= 0 ? pread(fd, head, sizeof(head), 0) : -1;
            struct stat st;
            const int ok_stat = fd >= 0 && fstat(fd, &st) == 0;
            if (fd >= 0) close(fd);
            chives_t made;
            chives_init(&made);
            const int mr =hn >= 0x40 && ok_stat ? chives_from_header(head, (size_t)hn, (uint64_t)st.st_size, &made) : -1;
            const int bad = mr != 0 || made.block_size != geo.block_size || made.block_count != geo.block_count || (made.has_fs_ext && made.fs_ext != 0);
            if (mr == 0) chives_free(&made);
            if (bad) {
                efail(err, errsz, "%s", T("Das neue Abbild hat nicht den Aufbau, den die ZIP-Datei beschreibt (die Konsole hat es anders angelegt). Es wurde nichts verändert."));
                goto done;
            }
        }
    }
    if (prog_stopped(p)) { rc = ECANCELED; goto done; }

    /* eingehängt: die Dateien hinein */
    fill_t f;
    memset(&f, 0, sizeof(f));
    f.geo = &geo;
    f.z = z;
    f.sfo = sfo;
    f.sfo_len = sfo_len;
    f.prefix = info.prefix;
    f.pl = strlen(info.prefix);
    f.p = p;
    f.ps4 = ps4;
    scpy(f.mnt, sizeof(f.mnt), mnt);
    {
        char e2[400];
        if (mkdir(mnt, 0755) != 0 && errno != EEXIST) { efail(err, errsz, T("Der Einhängeordner ließ sich nicht anlegen: %s"), strerror(errno)); goto done; }
        if ((ps4 ? savemnt_open_sealed(img, sealed, mnt, &mt, e2, sizeof(e2)) : savemnt_open(img, mnt, &mt, e2, sizeof(e2))) != 0) { efail(err, errsz, "%s", e2); goto done; }
        mounted = 1;
    }
    uint64_t free_blocks = 0;
    prog_phase(p, SEG(15, 55), info.bytes);
    {
        const int fr = fill(&f, err, errsz);
        if (fr == ECANCELED) { rc = ECANCELED; goto done; }
        if (fr != 0) goto done;
        struct statvfs sv;
        if (statvfs(mnt, &sv) == 0 && sv.f_frsize && sv.f_blocks && (uint64_t)sv.f_blocks * (uint64_t)sv.f_frsize <= info.image_size * 2)
            free_blocks = (uint64_t)sv.f_bfree * (uint64_t)sv.f_frsize / info.block_size;
        else
            free_blocks = info.image_size > info.bytes ? (info.image_size - info.bytes) / info.block_size : 0;
        if (free_blocks > info.block_count) free_blocks = info.block_count;
        sync();
    }
    {
        char e2[400];
        const int ce = savemnt_close(&mt, e2, sizeof(e2));
        mounted = 0;
        if (ce != 0) { stuck = 1; efail(err, errsz, T("%s Auf der PS5 wurde nichts verändert; bitte die PS5 neu starten, bevor es noch einmal versucht wird."), e2); goto done; }
    }
    rmdir(mnt);
    if (prog_stopped(p)) { rc = ECANCELED; goto done; }

    /* ein zweites Mal eingehängt: jede Datei aus dem neuen Abbild gelesen und mit der ZIP verglichen */
    prog_phase(p, SEG(55, 85), info.bytes);
    {
        char e2[400];
        if (mkdir(mnt, 0755) != 0 && errno != EEXIST) { efail(err, errsz, T("Der Einhängeordner ließ sich nicht anlegen: %s"), strerror(errno)); goto done; }
        if ((ps4 ? savemnt_open_sealed(img, sealed, mnt, &mt, e2, sizeof(e2)) : savemnt_open(img, mnt, &mt, e2, sizeof(e2))) != 0) {
            efail(err, errsz, T("Beim Prüfen ließ sich das neue Abbild nicht einhängen. %s"), e2);
            goto done;
        }
        mounted = 1;
        const int vr = verify_files(&f, err, errsz);
        if (vr == ECANCELED) { rc = ECANCELED; goto done; }
        if (vr != 0) goto done;
        const int ce = savemnt_close(&mt, e2, sizeof(e2));
        mounted = 0;
        if (ce != 0) { stuck = 1; efail(err, errsz, T("%s Auf der PS5 wurde nichts verändert; bitte die PS5 neu starten, bevor es noch einmal versucht wird."), e2); goto done; }
    }
    rmdir(mnt);

    /* das Bild des Spielstands aus der ZIP */
    int have_icon = 0;
    {
        char want[400];
        snprintf(want, sizeof(want), "%ssce_sys/icon0.png", info.prefix);
        for (unsigned i = 0; i < szr_count(z); i++) {
            if (szr_at(z, i)->is_dir || strcmp(szr_at(z, i)->name, want)) continue;
            unlink(icon_tmp);
            const int fd = open(icon_tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd >= 0) {
                have_icon = szr_extract(z, i, fd, SI_MEM_MAX, NULL, NULL) == SZ_OK && fsync(fd) == 0;
                close(fd);
                if (!have_icon) unlink(icon_tmp);
            }
            break;
        }
    }
    if (prog_stopped(p)) { rc = ECANCELED; goto done; }

    /* die Datenbank muss schreibbar sein, bevor etwas auf der Konsole angefasst wird: ihr Ordner ist da (wird nie angelegt) */
    const char *adir = ps4 ? SAVE_SUBDIR_PS4 : SAVE_SUBDIR;
    const char *mdir = ps4 ? "savedata_meta" : "savedata_prospero_meta";
    char dbp[SP], dbdir[SP];
    {
        char e2[300];
        struct stat st;
        if (path_fmt(dbp, sizeof(dbp), "%s/%s/%s/db/user/savedata.db", sys, adir, uid) || path_fmt(dbdir, sizeof(dbdir), "%s/%s/%s/db/user", sys, adir, uid)) {
            efail(err, errsz, "%s", T("Pfad zu lang"));
            goto done;
        }
        if (stat(dbdir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            efail(err, errsz, "%s", T("Für diesen Benutzer gibt es auf der PS5 keinen Ordner für die Spielstand-Datenbank. Es wurde nichts verändert."));
            goto done;
        }
        if (stat(dbp, &st) == 0 && savedb_has_slot(dbp, info.title_id, info.slot, e2, sizeof(e2)) < 0) {
            efail(err, errsz, T("Die Spielstand-Datenbank dieses Benutzers lässt sich nicht lesen. %s Es wurde nichts verändert."), e2);
            goto done;
        }
        /* eine Datenbank, die nicht in Ordnung ist, fällt jetzt auf - nicht erst nach dem Tausch der Abbilder (dann gäbe es nur den Weg
         * zurück zu einer Kopie einer beschädigten Datei) */
        if (stat(dbp, &st) == 0 && savedb_check(dbp, e2, sizeof(e2)) != 0) {
            efail(err, errsz, T("Die Spielstand-Datenbank dieses Benutzers ist nicht in Ordnung. %s Es wurde nichts verändert."), e2);
            goto done;
        }
    }
    /* der letzte Blick auf das, was sich inzwischen geändert haben kann: ein Spiel, das während des Schreibens und Prüfens gestartet wurde,
     * hält das Abbild, das gleich getauscht wird */
    {
        char gid[32] = "";
        if (g_game_check && g_game_check(gid, sizeof(gid))) {
            efail(err, errsz, T("Währenddessen wurde ein Spiel gestartet (%s). Es wurde nichts eingespielt."), gid);
            goto done;
        }
    }

    /* ab hier wird die Konsole verändert - kein Abbrechen mehr */
    prog_phase(p, SEG(85, 100), 0);
    if (p) p->fn = NULL;
    {
        char dir[SP], fin[SP], neu[SP], alt[SP], idir[SP], ifin[SP], ineu[SP], ialt[SP], dbb[SP] = "", bfin[SP], bneu[SP], balt[SP];
        int image_in = 0, replaced = 0, icon_in = 0, icon_replaced = 0, db_existed = 0, bin_in = 0, bin_replaced = 0;
        if (path_fmt(dir, sizeof(dir), "%s/%s/%s/%s", home, uid, adir, info.title_id) || path_fmt(fin, sizeof(fin), "%s/sdimg_%s", dir, info.slot) ||
            path_fmt(neu, sizeof(neu), "%s.usbfmt-neu", fin) || path_fmt(alt, sizeof(alt), "%s.usbfmt-alt", fin) ||
            path_fmt(idir, sizeof(idir), "%s/%s/%s/user/%s", home, uid, mdir, info.title_id) ||
            path_fmt(bfin, sizeof(bfin), "%s/%s.bin", dir, info.slot) || path_fmt(bneu, sizeof(bneu), "%s.usbfmt-neu", bfin) ||
            path_fmt(balt, sizeof(balt), "%s.usbfmt-alt", bfin) || path_fmt(ifin, sizeof(ifin), "%s/%s_icon0.png", idir, info.slot) ||
            path_fmt(ineu, sizeof(ineu), "%s.usbfmt-neu", ifin) || path_fmt(ialt, sizeof(ialt), "%s.usbfmt-alt", ifin)) {
            efail(err, errsz, "%s", T("Pfad zu lang"));
            goto done;
        }
        struct stat st;
        int e = mkdir_like_parent(dir);
        if (e) { efail(err, errsz, T("Der Ordner des Spielstands ließ sich nicht anlegen: %s"), strerror(e)); goto done; }
        /* ein abgebrochenes früheres Einspielen: fehlt die Datei und ist nur die alte da, kommt die alte zurück (sie ist der Spielstand);
         * sind beide da, ist die alte ein Rest und wird vom nächsten Umbenennen übernommen. Die unfertigen neuen gehen. */
        if (ps4 && (lstat(alt, &st) == 0 || lstat(balt, &st) == 0) && (lstat(fin, &st) != 0 || lstat(bfin, &st) != 0)) {
            /* PS4: Abbild und Schlüssel gehören zusammen - fehlt eines, kommen beide aus den alten Dateien zurück (sonst passte ein
             * neues Abbild zu einem alten Schlüssel) */
            /* gab es vorher kein Abbild (nur den Schlüssel), ist ein Abbild ohne alte Kopie das neue: es geht, der alte Schlüssel kommt zurück */
            if (lstat(alt, &st) != 0 && lstat(balt, &st) == 0 && lstat(fin, &st) == 0 && lstat(bfin, &st) != 0) unlink(fin);
            if (lstat(alt, &st) == 0) (void)rename(alt, fin);
            if (lstat(balt, &st) == 0) (void)rename(balt, bfin);
        }
        if (lstat(alt, &st) == 0 && lstat(fin, &st) != 0) (void)rename(alt, fin);
        unlink(neu);
        if (lstat(ialt, &st) == 0 && lstat(ifin, &st) != 0) (void)rename(ialt, ifin);
        unlink(ineu);
        if (ps4) {
            if (lstat(balt, &st) == 0 && lstat(bfin, &st) != 0) (void)rename(balt, bfin);
            unlink(bneu);
        }
        replaced = lstat(fin, &st) == 0;
        char stamp[24] = "00000000-000000";
        time_t now = time(NULL);
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        if (gmtime_r(&now, &tmv)) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        savedb_row_t row;
        memset(&row, 0, sizeof(row));
        scpy(row.title_id, sizeof(row.title_id), info.title_id);
        scpy(row.dir_name, sizeof(row.dir_name), info.slot);
        scpy(row.main_title, sizeof(row.main_title), info.main_title);
        scpy(row.sub_title, sizeof(row.sub_title), info.sub_title);
        scpy(row.detail, sizeof(row.detail), info.detail);
        strftime(row.mtime, sizeof(row.mtime), "%Y-%m-%dT%H:%M:%S.00Z", &tmv);
        row.account_id = account_id;
        row.user_id = user_id;
        row.user_param = info.user_param;
        row.blocks = info.block_count;
        row.free_blocks = free_blocks;
        row.size_kib = info.image_size / 1024;
        row.system_blocks = 0;
        /* das Abbild: auf demselben Dateisystem verschoben (daraus kann nichts halb Geschriebenes werden), sonst kopiert und mit dem
         * geprüften verglichen */
        e = 0;
        if (rename(img, neu) == 0) (void)chmod(neu, 0644);
        else {
            e = copy_file(NULL, img, neu, (uint64_t)-1, 0644);
            if (!e && !same_file(img, neu)) { unlink(neu); e = EIO; }
        }
        if (e) { efail(err, errsz, T("Das Abbild ließ sich nicht an den Platz des Spielstands legen: %s"), strerror(e)); goto done; }
        if (ps4) {
            e = copy_file(NULL, bin_tmp, bneu, (uint64_t)-1, 0644);
            if (!e && !same_file(bin_tmp, bneu)) { unlink(bneu); e = EIO; }
            if (e) { unlink(neu); efail(err, errsz, T("Die Schlüsseldatei ließ sich nicht an den Platz des Spielstands kopieren: %s"), strerror(e)); goto done; }
            bin_replaced = lstat(bfin, &st) == 0;
        }
        /* die Aufzeichnung des Tauschs, vor dem ersten Umbenennen: ein Abbruch ab hier hinterlässt sie, der nächste Start bringt zu Ende
         * oder nimmt zurück, was sie nennt */
        /* Reste eines früheren Laufs neben den jetzigen Dateien weg: das nächste Umbenennen ersetzte sie ohnehin, und ein Zurücknehmen nach
         * einem Abbruch legte sonst einen alten Rest über den jetzigen Spielstand */
        if (lstat(fin, &st) == 0) unlink(alt);
        if (lstat(ifin, &st) == 0) unlink(ialt);
        if (ps4 && lstat(bfin, &st) == 0) unlink(balt);
        journal_path(jpath, sizeof(jpath));
        if (jpath[0]) {
            jrec = calloc(1, sizeof(*jrec));
            int je = jrec ? 0 : ENOMEM;
            if (jrec) {
                scpy(jrec->fin, sizeof(jrec->fin), fin);
                scpy(jrec->ifin, sizeof(jrec->ifin), ifin);
                scpy(jrec->dbp, sizeof(jrec->dbp), dbp);
                jrec->ps4 = ps4;
                jrec->replaced = replaced;
                jrec->bin_replaced = bin_replaced;
                jrec->icon_existed = lstat(ifin, &st) == 0;
                jrec->row = row;
                je = journal_write(jpath, jrec, "swap");
            }
            if (je) {
                unlink(neu);
                if (ps4) unlink(bneu);
                efail(err, errsz, T("Die Aufzeichnung für den Fall eines Abbruchs ließ sich nicht schreiben: %s"), strerror(je));
                goto done;
            }
            jw = 1;
        }
        /* Abbild und Schlüssel direkt nacheinander umbenennen: die Zeit, in der sie nicht zusammenpassen, so kurz wie möglich */
        if (replaced && rename(fin, alt) != 0) {
            const int re = errno ? errno : EIO;
            unlink(neu);
            if (ps4) unlink(bneu);
            efail(err, errsz, T("Der bisherige Spielstand ließ sich nicht beiseite legen: %s"), strerror(re));
            goto done;
        }
        if (ps4 && bin_replaced && rename(bfin, balt) != 0) {
            const int re = errno ? errno : EIO;
            if (replaced) rb_ok &= rename(alt, fin) == 0;
            unlink(neu);
            unlink(bneu);
            efail(err, errsz, T("Die bisherige Schlüsseldatei ließ sich nicht beiseite legen: %s"), strerror(re));
            goto done;
        }
        if (rename(neu, fin) != 0) {
            const int re = errno ? errno : EIO;
            if (ps4 && bin_replaced) rb_ok &= rename(balt, bfin) == 0;
            if (replaced) rb_ok &= rename(alt, fin) == 0;
            unlink(neu);
            if (ps4) unlink(bneu);
            efail(err, errsz, T("Der neue Spielstand ließ sich nicht an seinen Platz legen: %s"), strerror(re));
            goto done;
        }
        image_in = 1;
        if (ps4) {
            if (rename(bneu, bfin) != 0) {
                const int re = errno ? errno : EIO;
                if (bin_replaced) rb_ok &= rename(balt, bfin) == 0;   /* erst der Schlüssel, dann das Abbild (die Aufräum-Regel am Anfang verlässt sich darauf) */
                if (replaced) rb_ok &= rename(alt, fin) == 0; else unlink(fin);
                unlink(bneu);
                efail(err, errsz, T("Die Schlüsseldatei ließ sich nicht an ihren Platz legen: %s"), strerror(re));
                goto done;
            }
            bin_in = 1;
        }
        if (have_icon) {
            if (mkdir_like_parent(idir) == 0 && copy_file(NULL, icon_tmp, ineu, (uint64_t)-1, 0644) == 0) {
                icon_replaced = lstat(ifin, &st) == 0;
                if (icon_replaced && rename(ifin, ialt) != 0) unlink(ineu);
                else if (rename(ineu, ifin) != 0) { if (icon_replaced) (void)rename(ialt, ifin); unlink(ineu); }
                else icon_in = 1;
            }
            if (!icon_in) snprintf(note, notesz, "%s", T("Das Bild des Spielstands ließ sich nicht ablegen; der Spielstand selbst ist eingespielt."));
        }
        sync_dir_of(fin);   /* die Umbenennungen dauerhaft, bevor "db" sie als geschehen aufzeichnet */
        if (icon_in) sync_dir_of(ifin);
        if (jw && journal_write(jpath, jrec, "db") != 0) {   /* die Abbilder liegen: übrig ist die Zeile */
            /* ohne "db" hielte ein Abbruch den Tausch für unfertig: lieber gleich zurück (in der Datenbank steht noch nichts) */
            if (icon_in) { if (icon_replaced) rb_ok &= rename(ialt, ifin) == 0; else unlink(ifin); }
            if (bin_in) { if (bin_replaced) rb_ok &= rename(balt, bfin) == 0; else unlink(bfin); }
            if (image_in) { if (replaced) rb_ok &= rename(alt, fin) == 0; else unlink(fin); }
            efail(err, errsz, "%s", T("Die Aufzeichnung für den Fall eines Abbruchs ließ sich nicht schreiben. Der Spielstand wurde nicht eingespielt."));
            goto done;
        }

        /* die Zeile in der Datenbank, nach einer Kopie davon */
        db_existed = stat(dbp, &st) == 0;
        char e2[400] = "";
        int dbr = 0, db_written = 0;
        if (db_existed) {
            char bdir[SP];
            int named = 0;
            if (data && !path_fmt(bdir, sizeof(bdir), "%s/save_db_backup", data) && mkdir_p(bdir, 0755) == 0) {
                /* ein eigener Name je Kopie (Zeit, Spielstand, Zähler): eine vorhandene Kopie wird nie ersetzt */
                for (int k = 0; k < 100 && !named; k++) {
                    struct stat bs;
                    if (path_fmt(dbb, sizeof(dbb), k ? "%s/savedata-%s-%s-%s-%s-%d.db" : "%s/savedata-%s-%s-%s-%s.db", bdir, uid, ps4 ? "ps4" : "ps5", stamp,
                                 info.slot, k))
                        break;
                    if (stat(dbb, &bs) != 0) named = 1;
                }
            }
            if (!named) {
                dbb[0] = 0;
                dbr = -1;
                snprintf(e2, sizeof(e2), "%s", T("Es gibt keinen Platz für die Sicherung der Spielstand-Datenbank."));
            } else if (savedb_backup(dbp, dbb, e2, sizeof(e2)) != 0) {
                unlink(dbb);   /* keine halbe Kopie liegen lassen */
                dbb[0] = 0;
                dbr = -1;
            }
        }
        if (dbr == 0) {
            int inserted = 0;
            dbr = savedb_upsert(dbp, &row, &inserted, e2, sizeof(e2));
            if (dbr == 0) {
                db_written = 1;
                dbr = savedb_check(dbp, e2, sizeof(e2));
            }
        }
        if (dbr != 0) {   /* alles wieder wie vorher */
            if (jw) {   /* ein Abbruch ab hier nimmt die Dateien (und die Datenbank) zurück, er beendet den Tausch nicht */
                if (db_written && db_existed && dbb[0]) scpy(jrec->dbb, sizeof(jrec->dbb), dbb);
                jrec->db_new = db_written && !db_existed;
                if (journal_write(jpath, jrec, "rollback") != 0) {
                    unlink(jpath);   /* lieber keine Aufzeichnung als "db": die schriebe die neue Zeile über das alte Abbild */
                    jw = 0;
                }
            }
            int db_back = 1;
            /* die Kopie kommt nur zurück, wenn SQLite geschrieben hat und erst die Prüfung danach scheiterte. Hat SQLite selbst abgelehnt
             * (etwa gesperrt), hat es schon zurückgerollt - eine Datei über eine Datenbank zu legen, die die Konsole vielleicht offen hat,
             * richtete eigenen Schaden an (wie im Cooling Center) */
            if (db_written && db_existed && dbb[0]) {
                char dneu[SP];
                db_back = !path_fmt(dneu, sizeof(dneu), "%s.usbfmt-neu", dbp) && copy_file(NULL, dbb, dneu, (uint64_t)-1, 0644) == 0 && rename(dneu, dbp) == 0;
            } else if (db_written && !db_existed) {
                unlink(dbp);
            }
            if (icon_in) { if (icon_replaced) rb_ok &= rename(ialt, ifin) == 0; else unlink(ifin); }
            if (bin_in) { if (bin_replaced) rb_ok &= rename(balt, bfin) == 0; else unlink(bfin); }
            if (image_in) { if (replaced) rb_ok &= rename(alt, fin) == 0; else unlink(fin); }
            if (db_back) efail(err, errsz, T("%s Der Spielstand wurde nicht eingespielt; alles ist wieder so wie vorher."), e2);
            else efail(err, errsz, T("%s Der Spielstand wurde nicht eingespielt. Die Spielstand-Datenbank ließ sich nicht zurücklegen; ihre Kopie liegt in %s."), e2, dbb);
            goto done;
        }
        if (replaced) unlink(alt);
        if (icon_replaced) unlink(ialt);
        if (bin_in && bin_replaced) unlink(balt);
    }
    if (info.converted) {
        const size_t on = strlen(note);
        snprintf(note + on, notesz - on, T("%sDer Spielstand lag im neueren Abbildformat (mit %llu Systemblöcken) und wurde als klassisches Abbild mit %llu Blöcken angelegt. Ob das Spiel ihn annimmt, zeigt erst ein Start des Spiels."),
                 on ? "\n" : "", (unsigned long long)info.src_system_blocks, (unsigned long long)info.block_count);
    }
    if (f.chmod_failed && !note[0]) snprintf(note, notesz, T("Bei %d Dateien ließen sich die Zugriffsrechte der Quelle nicht übernehmen."), f.chmod_failed);
    if (owner_note[0]) {
        const size_t on = strlen(note);
        snprintf(note + on, notesz - on, "%s%.500s", on ? "\n" : "", owner_note);
    }
    if (recover_note[0]) {
        const size_t on = strlen(note);
        snprintf(note + on, notesz - on, "%s%.500s", on ? "\n" : "", recover_note);
    }
    rc = 0;
done:
#undef SEG
    /* zu Ende oder zurückgenommen: für den nächsten Start bleibt nichts zu tun. Misslang ein Zurücklegen, bleibt die Aufzeichnung
     * ("rollback"), damit der nächste Start es noch einmal versucht */
    if (jw && jpath[0]) {
        if (rb_ok) unlink(jpath);
        else (void)journal_write(jpath, jrec, "rollback");
    }
    free(jrec);
    if (mounted) {
        char e2[300];
        if (savemnt_close(&mt, e2, sizeof(e2)) != 0) {
            stuck = 1;
            if (rc == ECANCELED) rc = -1;
            efail(err, errsz, T("%s Bitte die PS5 neu starten, bevor es noch einmal versucht wird."), e2);
        }
    }
    if (!stuck) {
        if (have_img) unlink(img);
        if (mnt[0]) rmdir(mnt);
    }
    if (icon_tmp[0]) unlink(icon_tmp);
    if (bin_tmp[0]) unlink(bin_tmp);
    free(sfo);
    chives_free(&geo);
    if (rc != 0 && rc != ECANCELED && !err[0]) efail(err, errsz, "%s", T("Der Spielstand ließ sich nicht einspielen."));
    return rc;
}

int saves_zip_slots(const char *zip, char (*out)[160], int max) {
    int zerr = 0, nr = 0, n = -1;
    char (*roots)[200] = calloc(ZIP_MAXSAVES, sizeof(*roots));
    szr_t *z = roots ? szr_open(zip, &zerr) : NULL;
    char e2[300];
    if (z && find_roots(z, roots, ZIP_MAXSAVES, &nr, e2, sizeof(e2)) == 0) {
        n = 0;
        for (int i = 0; i < nr && n < max; i++) {
            iinfo_t info;
            chives_t geo;
            uint8_t *sfo = NULL;
            size_t sl = 0;
            inspect_root(z, roots[i], &info, &geo, &sfo, &sl);
            free(sfo);
            chives_free(&geo);
            if (info.title_id[0] && info.slot[0]) snprintf(out[n++], 160, "%s/%s#%s", info.title_id, info.slot, info.ps4 ? "PS4" : "PS5");
        }
    }
    if (z) szr_close(z);
    free(roots);
    return n;
}

int saves_backup_uid(const char *path, char *uid, size_t n) {
    const char *f = path ? strrchr(path, '/') : NULL;
    if (!f || f == path) return -1;
    const char *d = f - 1;
    while (d > path && *d != '/') d--;
    if (*d == '/') d++;
    /* Ordner "<Benutzer>_<uid>": die uid nach dem letzten "_" */
    const char *us = NULL;
    for (const char *c = d; c < f; c++) if (*c == '_') us = c;
    if (!us || f - us - 1 != 8 || n < 9) return -1;
    char u[9];
    memcpy(u, us + 1, 8);
    u[8] = 0;
    if (!uid_ok(u)) return -1;
    scpy(uid, n, u);
    return 0;
}

static int user_exists(const char *home, const char *uid) {
    char p[700], name[64];
    struct stat st;
    return uid_ok(uid) && !path_fmt(p, sizeof(p), "%s/%s", home, uid) && stat(p, &st) == 0 && S_ISDIR(st.st_mode) &&
           read_username(home, uid, name, sizeof(name)) == 0;
}

/* Vor dem Einspielen den jetzigen Stand des Spiels sichern (wie das Cooling Center: einmal je Spiel und Auftrag, alle seiner
 * Spielstände) - als gewöhnliche Sicherung unter undo_root, also in der Liste "Wiederherstellen" zu finden. done hält die schon
 * gesicherten Spiele dieses Auftrags ("<uid>/<Titel>#<Konsole>\n..."). 0 = gesichert oder nichts zu sichern, sonst -1 (err). */
/* Rohkopie eines Spielstands (Abbild und bei PS4 der Schlüssel) als Stand davor, wenn er sich nicht auslesen lässt - etwa ein
 * beschädigter, der gerade ersetzt werden soll (wie das Cooling Center, dessen Stand davor immer eine Rohkopie ist). Nur auf
 * dieser PS5 brauchbar. <undo_root>/<Benutzer>_<uid>/roh/<Titel>_<Name>.<Zeit>.img (+ .bin). 0 oder -1 */
static int undo_raw(const save_info_t *s, const char *undo_root, char *err, size_t errsz) {
    char user[64], dir[1000], img[1300], bin[1300], stamp[24] = "00000000-000000";
    time_t now = time(NULL);
    struct tm tmv;
    if (gmtime_r(&now, &tmv)) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    safe_name(user, 40, s->username[0] ? s->username : s->uid);
    if (path_fmt(dir, sizeof(dir), "%s/%s_%s/roh", undo_root, user, s->uid) ||
        path_fmt(img, sizeof(img), "%s/%s_%s.%s.img", dir, s->title_id, s->save_dir, stamp) ||
        path_fmt(bin, sizeof(bin), "%s/%s_%s.%s.bin", dir, s->title_id, s->save_dir, stamp))
        return efail(err, errsz, "%s", T("Pfad zu lang"));
    mkdirs(dir);
    struct statvfs sv;
    if (statvfs(dir, &sv) == 0 && (uint64_t)sv.f_bavail * sv.f_frsize < s->size + MIN_FREE)
        return efail(err, errsz, "%s", T("Am Ziel ist nicht mehr genug Platz frei."));
    int e = copy_file(NULL, s->path, img, s->size, 0644);
    if (!e && s->key_path[0]) e = copy_file(NULL, s->key_path, bin, 96, 0644);
    if (e) {
        unlink(img);
        unlink(bin);
        return efail(err, errsz, T("Schreibfehler: %s"), strerror(e));
    }
    /* je Spielstand bleiben die drei neuesten Rohkopien (<Titel>_<Name>.<Zeit>.img, Zeit 15 Zeichen) */
    char prefix[600];
    DIR *d = path_fmt(prefix, sizeof(prefix), "%s_%s.", s->title_id, s->save_dir) ? NULL : opendir(dir);
    char (*names)[320] = d ? malloc(sizeof(*names) * 128) : NULL;
    int nn = 0;
    struct dirent *de;
    const size_t pl = strlen(prefix);
    while (names && (de = readdir(d)) && nn < 128) {
        const size_t l = strlen(de->d_name);
        if (l == pl + 15 + 4 && !strncmp(de->d_name, prefix, pl) && !strcmp(de->d_name + l - 4, ".img")) scpy(names[nn++], sizeof(*names), de->d_name);
    }
    if (d) closedir(d);
    if (names && nn > 3) {
        qsort(names, (size_t)nn, sizeof(*names), cmp_str_desc);
        for (int i = 3; i < nn; i++) {
            char p1[1400], p2[1400];
            if (path_fmt(p1, sizeof(p1), "%s/%s", dir, names[i])) continue;
            unlink(p1);
            if (!path_fmt(p2, sizeof(p2), "%s", p1) && strlen(p2) > 4) { memcpy(p2 + strlen(p2) - 4, ".bin", 5); unlink(p2); }
        }
    }
    free(names);
    return 0;
}

static int undo_backup(const char *uid, const char *title, int ps4, const char *undo_root, char *done, size_t donesz, prog_t *p,
                       int *saved, int *raw, char *err, size_t errsz) {
    char key[64];
    snprintf(key, sizeof(key), "%s/%s#%s\n", uid, title, ps4 ? "PS4" : "PS5");
    if (!undo_root || !undo_root[0] || (done && strstr(done, key))) return 0;
    save_info_t *all = NULL;
    int na = 0, rc = 0;
    if (saves_scan(&all, &na) < 0) return efail(err, errsz, "%s", T("Die Spielstände der PS5 sind nicht erreichbar."));
    for (int k = 0; k < na && rc == 0; k++) {
        const save_info_t *s = &all[k];
        if (strcmp(s->uid, uid) || strcmp(s->title_id, title) || strcmp(s->platform, ps4 ? "PS4" : "PS5")) continue;
        char out[1300], e2[600] = "";
        if (prog_stopped(p)) { rc = ECANCELED; break; }
        if (backup_one(s, undo_root, 1, out, sizeof(out), NULL, 0, NULL, NULL, e2, sizeof(e2)) == 0) {
            if (saved) (*saved)++;
        } else if (prog_stopped(p)) {
            rc = ECANCELED;
        } else {
            char e3[300] = "";
            if (undo_raw(s, undo_root, e3, sizeof(e3)) == 0) {   /* ließ sich nicht auslesen: dann wenigstens roh */
                if (raw) (*raw)++;
            } else {
                char both[1000];
                snprintf(both, sizeof(both), "%s %s", e2, e3);
                efail(err, errsz, T("Der jetzige Stand ließ sich nicht sichern, deshalb wurde nichts eingespielt. %s"), both);
                rc = -1;
            }
        }
    }
    free(all);
    if (rc == 0 && done && strlen(done) + strlen(key) < donesz) snprintf(done + strlen(done), donesz - strlen(done), "%s", key);
    return rc;
}

int saves_target_uid(const char *uid, const char *fallback_uid, char *out, size_t n, char *err, size_t errsz) {
    const char *home = plat_saves_dir();
    out[0] = 0;
    if (!home) return efail(err, errsz, "%s", T("Die Spielstände der PS5 sind nicht erreichbar."));
    if (uid && uid[0]) {
        if (!user_exists(home, uid)) return efail(err, errsz, T("Der Benutzer %s ist auf dieser PS5 nicht vorhanden"), uid);
        scpy(out, n, uid);
    } else if (fallback_uid && fallback_uid[0] && user_exists(home, fallback_uid)) {
        scpy(out, n, fallback_uid);
    } else {
        save_user_t *users = NULL;
        int nu = 0;
        if (saves_scan_users(&users, &nu) == 0 && nu == 1) scpy(out, n, users[0].uid);
        free(users);
        if (!out[0]) return efail(err, errsz, "%s", T("Bitte wählen, für welchen Benutzer der Spielstand eingespielt wird."));
    }
    return 0;
}

/* die Hinweise eines Spielstands (je Zeile einer) an die gesammelten hängen; ein schon vorhandener kommt nicht noch einmal dazu
 * (sonst stünde etwa der Besitzer-Hinweis je Speicherplatz einmal da) */
static void note_merge(char *note, size_t notesz, char *add) {
    for (char *piece = add; piece && *piece;) {
        char *nl = strchr(piece, '\n');
        if (nl) *nl = 0;
        if (*piece && !strstr(note, piece)) {
            const size_t on = strlen(note);
            if (on + 1 < notesz) snprintf(note + on, notesz - on, "%s%s", on ? " " : "", piece);
        }
        piece = nl ? nl + 1 : NULL;
    }
}

int saves_restore_file(const char *zip_path, const char *uid, const char *fallback_uid, const char *undo_root, char *undo_done,
                       size_t undo_donesz, int *count, saves_prog_fn prog, void *pctx, char *note, size_t notesz, char *err, size_t errsz) {
    if (count) *count = 0;
    if (note && notesz) note[0] = 0;
    err[0] = 0;
    const char *home = plat_saves_dir();
    if (!home) { snprintf(err, errsz, "%s", T("Die Spielstände der PS5 sind nicht erreichbar.")); return -1; }
    char target[16] = "";
    if (saves_target_uid(uid, fallback_uid, target, sizeof(target), err, errsz) != 0) return -1;
    if (op_enter(err, errsz)) return -1;
    {   /* zuerst, was ein abgebrochener früherer Tausch offen ließ (vor jeder Prüfung dieser ZIP) */
        char rn[600];
        const int jr = saves_import_recover(rn, sizeof(rn));
        if (jr < 0) {
            snprintf(err, errsz, T("Von einem früheren Einspielen ist noch etwas offen. %s Es wurde nichts verändert."), rn);
            op_leave();
            return -1;
        }
        if (jr > 0 && note && notesz) snprintf(note, notesz, "%s", rn);
    }
    (void)saves_cleanup_stale();   /* unter der Sperre: kein anderes Einspielen legt gerade "*.usbfmt-neu" an */
    int rc = -1, zerr = 0, nr = 0;
    char (*roots)[200] = calloc(ZIP_MAXSAVES, sizeof(*roots));
    char (*seen)[160] = calloc(ZIP_MAXSAVES, sizeof(*seen));
    szr_t *z = roots ? szr_open(zip_path, &zerr) : NULL;
    prog_t p = {prog, pctx, 1000, 0, 0, 0, 0, 0};
    if (!roots || !seen) { snprintf(err, errsz, "%s", T("Kein Speicher")); goto done; }
    if (!z) {
        snprintf(err, errsz, "%s", zerr == SZ_E_IO ? T("Die ZIP-Datei ließ sich nicht öffnen.") : zerr == SZ_E_NAME ? T("Die ZIP-Datei enthält einen unsicheren Dateinamen.") :
                 T("Das ist keine lesbare ZIP-Datei (oder eine, die sich nicht lesen lässt: verschlüsselt oder ZIP64)."));
        goto done;
    }
    if (find_roots(z, roots, ZIP_MAXSAVES, &nr, err, errsz)) goto done;
    /* erst alle Spielstände der ZIP prüfen, dann einspielen: eine fehlerhafte ZIP ändert nichts */
    for (int i = 0; i < nr; i++) {
        iinfo_t info;
        chives_t geo;
        uint8_t *sfo = NULL;
        size_t sl = 0;
        inspect_root(z, roots[i], &info, &geo, &sfo, &sl);
        free(sfo);
        chives_free(&geo);
        if (!info.importable) { snprintf(err, errsz, "%s", info.why); goto done; }
        if (saves_title_is_app(info.title_id)) {
            snprintf(err, errsz, "%s", T("Das ist eine App, kein Spiel. Bei Apps (zum Beispiel YouTube) ändert der Formatierer nichts."));
            goto done;
        }
        /* zweimal derselbe Spielstand würde sich gegenseitig ersetzen: der letzte gewönne, gleich wie alt */
        snprintf(seen[i], sizeof(seen[i]), "%s/%s#%d", info.title_id, info.slot, info.ps4);
        for (int k = 0; k < i; k++)
            if (!strcmp(seen[k], seen[i])) {
                snprintf(err, errsz, T("Zwei Spielstände in der Auswahl sind derselbe (%.60s): Es kann nur einer davon eingespielt werden."), info.slot);
                goto done;
            }
    }
    /* der jetzige Stand jedes betroffenen Spiels, bevor etwas ersetzt wird */
    {
        int saved = 0, raw = 0;
        for (int i = 0; i < nr; i++) {
            iinfo_t info;
            chives_t geo;
            uint8_t *sfo = NULL;
            size_t sl = 0;
            inspect_root(z, roots[i], &info, &geo, &sfo, &sl);
            free(sfo);
            chives_free(&geo);
            g_undo_keep = zip_path;
            const int ur = undo_backup(target, info.title_id, info.ps4, undo_root, undo_done, undo_donesz, &p, &saved, &raw, err, errsz);
            g_undo_keep = NULL;
            if (ur == ECANCELED) { snprintf(err, errsz, "%s", T("Abgebrochen.")); goto done; }
            if (ur != 0) goto done;
        }
        if (saved && note && notesz) {
            const size_t on = strlen(note);
            snprintf(note + on, notesz - on, T("%sDer Stand davor ist gesichert (%d Spielstände, in der Liste „Wiederherstellen“)."), on ? " " : "", saved);
        }
        if (raw && note && notesz) {
            const size_t on = strlen(note);
            snprintf(note + on, notesz - on, T("%s%d Spielstände ließen sich nicht auslesen; von ihnen liegt eine Rohkopie im Ordner „roh“ der Sicherungen (nur auf dieser PS5 brauchbar)."),
                     on ? " " : "", raw);
        }
    }
    /* noch einmal, direkt vor dem Ersetzen (das Sichern des Stands davor dauert): ein Spiel könnte den Spielstand gerade benutzen */
    {
        char gid[32] = "";
        if (g_game_check && g_game_check(gid, sizeof(gid))) {
            snprintf(err, errsz, T("Währenddessen wurde ein Spiel gestartet (%s). Es wurde nichts eingespielt."), gid);
            goto done;
        }
    }
    for (int i = 0; i < nr; i++) {
        char n2[1200] = "", e2[600] = "";
        const unsigned a = (unsigned)(i * 1000 / nr), b = (unsigned)((i + 1) * 1000 / nr);
        const int r = import_root(z, roots[i], target, &p, a, b, n2, sizeof(n2), e2, sizeof(e2));
        if (r != 0) {
            if (r == ECANCELED) snprintf(e2, sizeof(e2), "%s", T("Abgebrochen."));
            if (i) snprintf(err, errsz, T("%d von %d Spielständen wiederhergestellt, dann Fehler: %s"), i, nr, e2);
            else snprintf(err, errsz, "%s", e2);
            if (count) *count = i;
            goto done;
        }
        p.fn = prog;   /* nach dem Ersetzen war Abbrechen gesperrt - für den nächsten Spielstand wieder erlaubt */
        if (n2[0] && note && notesz) note_merge(note, notesz, n2);
        if (count) *count = i + 1;
    }
    rc = 0;
done:
    if (z) szr_close(z);
    free(roots);
    free(seen);
    op_leave();
    return rc;
}

/* ── Sicherungen auf der PS5 auflisten ── */

static int cmp_backups(const void *a, const void *b) {
    const save_backup_t *x = a, *y = b;
    int c = strcmp(x->username, y->username);
    if (c) return c;
    c = strcmp(x->title_name, y->title_name);
    if (!c) c = strcmp(x->save_dir, y->save_dir);
    if (c) return c;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0;   /* gleiche Sicherung: neueste zuerst */
}

/* eine Sicherung lesen: genau ein Spielstand im Wurzelordner der ZIP */
static int backup_info(const char *fp, const struct stat *st, const char *home, save_backup_t *b) {
    int zerr = 0, rc = -1;
    szr_t *z = szr_open(fp, &zerr);
    if (!z) return -1;
    int sfo_i = -1, side = 0;
    int64_t newest = 0;
    for (unsigned i = 0; i < szr_count(z); i++) {
        const szr_entry_t *e = szr_at(z, i);
        if (e->is_dir) continue;
        if (!strcmp(e->name, "sce_sys/param.sfo")) sfo_i = (int)i;
        else if (chives_is_sidecar(e->name)) { side = 1; continue; }
        const time_t t = szr_mtime(e);
        if ((int64_t)t > newest) newest = (int64_t)t;
    }
    {   /* Sicherungen dieses Formatierers tragen die Zeit des Abbilds im Kommentar; sonst die jüngste Datei */
        const char *c = szr_comment(z);
        if (!strncmp(c, "usbfmt-mtime:", 13) && strspn(c + 13, "0123456789") == strlen(c + 13) && strlen(c + 13) > 0 && strlen(c + 13) < 12)
            newest = strtoll(c + 13, NULL, 10);
    }
    uint8_t *sfo = NULL;
    size_t sl = 0;
    if (sfo_i >= 0 && szr_read(z, (unsigned)sfo_i, &sfo, &sl, SI_MAX_SFO) == SZ_OK && savesfo_valid(sfo, sl)) {
        char tid[24] = "", slot[120] = "", mt[256] = "";
        savesfo_get_str(sfo, sl, "TITLE_ID", tid, sizeof(tid));
        savesfo_get_str(sfo, sl, "SAVEDATA_DIRECTORY", slot, sizeof(slot));
        savesfo_get_str(sfo, sl, "MAINTITLE", mt, sizeof(mt));
        if ((ppsa_ok(tid) && side) || cusa_ok(tid)) {
            if (slot_ok(slot) && !path_fmt(b->save_dir, sizeof(b->save_dir), "sdimg_%s", slot) && !path_fmt(b->path, sizeof(b->path), "%s", fp)) {
                scpy(b->title_id, sizeof(b->title_id), tid);
                scpy(b->platform, sizeof(b->platform), cusa_ok(tid) ? "PS4" : "PS5");
                if (home) title_name_of(home, tid, b->title_name, sizeof(b->title_name));
                if (!b->title_name[0]) scpy(b->title_name, sizeof(b->title_name), mt[0] ? mt : tid);
                b->size = (uint64_t)st->st_size;
                b->mtime = st->st_mtime;
                b->save_mtime = newest;
                if (saves_backup_uid(fp, b->uid, sizeof(b->uid)) == 0) {
                    if (!home || read_username(home, b->uid, b->username, sizeof(b->username)) != 0) {
                        /* Ordnername vor "_<uid>" */
                        const char *f = strrchr(fp, '/'), *d = f - 1;
                        while (d > fp && *d != '/') d--;
                        if (*d == '/') d++;
                        size_t l = (size_t)(f - d) > 9 ? (size_t)(f - d) - 9 : 0;
                        if (l >= sizeof(b->username)) l = sizeof(b->username) - 1;
                        memcpy(b->username, d, l);
                        b->username[l] = 0;
                    }
                }
                rc = 0;
            }
        }
    }
    free(sfo);
    szr_close(z);
    return rc;
}

int saves_list_backups(const char *root, save_backup_t **out, int *count) {
    *out = NULL;
    *count = 0;
    DIR *rd = opendir(root);
    if (!rd) return -1;
    const char *home = plat_saves_dir();
    save_backup_t *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ue;
    while ((ue = readdir(rd)) && n < SAVES_MAX) {
        char ud[1000];
        if (ue->d_name[0] == '.' || path_fmt(ud, sizeof(ud), "%s/%s", root, ue->d_name)) continue;
        DIR *dd = opendir(ud);
        struct dirent *fe;
        while (dd && (fe = readdir(dd)) && n < SAVES_MAX) {
            size_t l = strlen(fe->d_name);
            char fp[1300];
            struct stat st;
            if (fe->d_name[0] == '.' || l < 5 || strcmp(fe->d_name + l - 4, ".zip") != 0 ||
                path_fmt(fp, sizeof(fp), "%s/%s", ud, fe->d_name) || stat(fp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (n == cap) {
                save_backup_t *p = realloc(arr, (size_t)(cap = cap ? cap * 2 : 32) * sizeof(*p));
                if (!p) { free(arr); closedir(dd); closedir(rd); return -1; }
                arr = p;
            }
            memset(&arr[n], 0, sizeof(arr[n]));
            if (backup_info(fp, &st, home, &arr[n]) == 0) n++;
        }
        if (dd) closedir(dd);
    }
    closedir(rd);
    if (n > 1) qsort(arr, (size_t)n, sizeof(*arr), cmp_backups);
    *out = arr;
    *count = n;
    return 0;
}
