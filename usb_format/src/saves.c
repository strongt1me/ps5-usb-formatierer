/*
 * PS5-Spielstand-Sicherung, und Wiederherstellung.
 * Sichern: das verschlüsselte Container-Image + metadata.json als ZIP (nichts wird entschlüsselt).
 * Wiederherstellen: ZIP prüfen und den Container atomar zurückschreiben.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

#include "i18n.h"
#include "library.h"
#include "platform.h"
#include "saves.h"

#define SAVE_SUBDIR     "savedata_prospero"
#define SAVE_SUBDIR_PS4 "savedata"
#define IMG_MIN         0x1000ull
#define IMG_MAX         0xF0000000ull   /* ZIP ohne ZIP64: Größen passen in 32 Bit */
#define META_MAX        4096
#define SAVES_MAX       4096
#define CHUNK           65536
#define FREE_MARGIN     (1ull << 20)

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static pthread_rwlock_t g_op = PTHREAD_RWLOCK_INITIALIZER;

/* Lesen (Sichern, Herunterladen) darf gleichzeitig laufen - Browser fragen eine Datei gern doppelt an -, Zurückschreiben
 * nur allein und nie neben einem Lesen derselben Dateien */
static int op_read(char *err, size_t errsz) {
    if (pthread_rwlock_tryrdlock(&g_op) == 0) return 0;
    snprintf(err, errsz, "%s", T("Es läuft bereits ein Spielstand-Vorgang"));
    return -1;
}
static int op_enter(char *err, size_t errsz) {
    if (pthread_rwlock_trywrlock(&g_op) == 0) return 0;
    snprintf(err, errsz, "%s", T("Es läuft bereits ein Spielstand-Vorgang"));
    return -1;
}
static void op_leave(void) { pthread_rwlock_unlock(&g_op); }

/* ── Hilfen ──────────────────────────────────────────────────────────── */

/* Zeichenkette in einen Puffer fester Größe kopieren (abgeschnitten, immer mit Ende) */
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

int saves_name_ok(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= max || s[0] == '.') return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return 0;
    }
    return 1;
}

static int pread_full(int fd, void *buf, size_t n, off_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; off += r; n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static uint32_t rd16(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8; }
static uint32_t rd32(const uint8_t *b) { return rd16(b) | rd16(b + 2) << 16; }
static void wr16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *b, uint32_t v) { wr16(b, v & 0xFFFF); wr16(b + 2, v >> 16); }

static uint32_t g_crc_tbl[256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tbl[i] = c;
    }
}
static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len) {
    pthread_once(&g_crc_once, crc_init);
    const uint8_t *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = g_crc_tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* legt den Ordner bei Bedarf an */
const char *saves_workdir(void) {
    static char dir[300];
    const char *d = plat_data_dir();
    snprintf(dir, sizeof(dir), "%s/save_tmp", d ? d : "/tmp");
    if (d) mkdir(d, 0755);
    mkdir(dir, 0755);
    return dir;
}

static void mkdirs(const char *path) {
    char p[1300];
    scpy(p, sizeof(p), path);
    for (char *c = p + 1; *c; c++) {
        if (*c != '/') continue;
        *c = 0;
        mkdir(p, 0777);
        *c = '/';
    }
    mkdir(p, 0777);
}

/* freier Platz im Ordner path ≥ need? */
static int space_ok(const char *path, uint64_t need) {
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) return 1;   /* unbekannt: nicht blockieren, Schreibfehler werden gemeldet */
    return (uint64_t)sv.f_bavail * sv.f_frsize >= need + FREE_MARGIN;
}

/* ── Benutzer ────────────────────────────────────────── */

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
    out[strcspn(out, "\r\n")] = 0;   /* NUL-terminiert; Zeilenende abschneiden */
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

/* ── Spielstände auflisten ───────────────────────────────────────────── */

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
                if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {   /* Ersatzpaar */
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
            if (cp >= 0x80) {   /* UTF-8 unverändert durchreichen */
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

/* Spielname des installierten Titels: <Ordner über /user/home>/appmeta/<id>/param.json (neuere Firmware) oder param.sfo
 * (PS5: /user/appmeta), sonst /system_data/priv/appmeta */
static void title_name_of(const char *home, const char *title_id, char *name, size_t n) {
    char meta[700], sys[] = "/system_data/priv/appmeta";
    scpy(meta, sizeof(meta) - 8, home);
    size_t l = strlen(meta);
    while (l > 1 && meta[l - 1] == '/') meta[--l] = 0;
    char *sl = strrchr(meta, '/');
    if (sl) *sl = 0;   /* "/user/home" -> "/user" */
    strcat(meta, "/appmeta");
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
            if (!saves_name_ok(te->d_name, sizeof(arr->title_id)) || path_fmt(tdir, sizeof(tdir), "%s/%s", pdir, te->d_name))
                continue;
            DIR *sd = opendir(tdir);
            if (!sd) continue;
            struct dirent *se;
            while ((se = readdir(sd))) {
                size_t nl = strlen(se->d_name);
                char fpath[1200];
                struct stat st;
                if (!saves_name_ok(se->d_name, sizeof(arr->save_dir))) continue;
                if (nl > 4 && !strcmp(se->d_name + nl - 4, ".bin")) continue;   /* Schlüssel-Begleiter (PS4) */
                if (!strncmp(se->d_name, "sdimg_sce_bu_", 13)) continue;         /* zweite Systemkopie, die die PS5 selbst anlegt */
                char kpath[1200] = "";
                if (ps4) {   /* PS4: nur sdimg_<Name> mit <Name>.bin (96 Byte) daneben - ohne den Schlüssel taugt der Container nichts */
                    struct stat ks;
                    if (strncmp(se->d_name, "sdimg_", 6) || nl <= 6 || path_fmt(kpath, sizeof(kpath), "%s/%s.bin", tdir, se->d_name + 6) ||
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
        }   /* pass */
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

/* ── ZIP schreiben (gespeichert, ohne Deflate): als chunked HTTP an einen Socket oder in eine Datei ─ */

static int set_times_fd(int fd, const char *path, const struct timespec ts[2]);

typedef struct { char name[400]; uint32_t crc, size, off; uint16_t flags; } zcd_t;

typedef struct {
    int      sock, fd;   /* fd >= 0: in diese Datei schreiben, sonst als chunked HTTP an sock */
    int      broken, started;
    char     head[768];
    uint64_t off;
    zcd_t   *cd;
    int      cd_n, cd_cap;
    saves_prog_fn prog;   /* Fortschritt beim Schreiben des Containers */
    void    *pctx;
} zs_t;

static int sock_send(int sock, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(sock, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* ein Stück Ausgabe; bei HTTP ein Chunk (Länge 0 würde den Strom beenden und wird nie gesendet) */
static void zs_chunk(zs_t *z, const void *buf, size_t len) {
    if (z->broken || !len) return;
    if (z->fd >= 0) {
        if (write_full(z->fd, buf, len)) z->broken = 1;
        return;
    }
    char hdr[24];
    int hl = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    if (!z->started) {
        z->started = 1;
        if (sock_send(z->sock, z->head, strlen(z->head))) { z->broken = 1; return; }
    }
    if (sock_send(z->sock, hdr, (size_t)hl) || sock_send(z->sock, buf, len) || sock_send(z->sock, "\r\n", 2))
        z->broken = 1;
}

static void zs_data(zs_t *z, const void *buf, size_t len) {
    zs_chunk(z, buf, len);
    z->off += len;
}

/* flags: 0x0800 UTF-8, 0x0008 = Größen/CRC folgen hinter den Daten. Liefert den Eintrag im Zentralverzeichnis. */
static zcd_t *zs_local(zs_t *z, const char *name, uint16_t flags, uint32_t crc, uint32_t size) {
    uint8_t h[30 + 400];
    size_t nl = strlen(name);
    if (z->cd_n == z->cd_cap) {
        zcd_t *p = realloc(z->cd, (size_t)(z->cd_cap = z->cd_cap ? z->cd_cap * 2 : 16) * sizeof(*p));
        if (!p) { z->broken = 1; return NULL; }
        z->cd = p;
    }
    if (nl >= sizeof(z->cd[0].name)) { z->broken = 1; return NULL; }
    memset(h, 0, 30);
    wr32(h, 0x04034b50);
    wr16(h + 4, 20);
    wr16(h + 6, flags);
    wr16(h + 10, 0);        /* Uhrzeit */
    wr16(h + 12, 0x0021);   /* Datum 1980-01-01 */
    wr32(h + 14, crc);
    wr32(h + 18, size);
    wr32(h + 22, size);
    wr16(h + 26, (uint32_t)nl);
    memcpy(h + 30, name, nl);
    zcd_t *e = &z->cd[z->cd_n++];
    e->crc = crc;
    e->size = size;
    e->off = (uint32_t)z->off;
    e->flags = flags;
    scpy(e->name, sizeof(e->name), name);
    zs_data(z, h, 30 + nl);
    return e;
}

static void zs_finish(zs_t *z) {
    size_t cap = 22, pos = 0;
    for (int i = 0; i < z->cd_n; i++) cap += 46 + strlen(z->cd[i].name);
    uint8_t *buf = malloc(cap);
    if (!buf) { z->broken = 1; return; }
    uint32_t cd_off = (uint32_t)z->off;
    for (int i = 0; i < z->cd_n; i++) {
        size_t nl = strlen(z->cd[i].name);
        uint8_t *e = buf + pos;
        memset(e, 0, 46);
        wr32(e, 0x02014b50);
        wr16(e + 4, 20);
        wr16(e + 6, 20);
        wr16(e + 8, z->cd[i].flags);
        wr16(e + 12, 0);
        wr16(e + 14, 0x0021);
        wr32(e + 16, z->cd[i].crc);
        wr32(e + 20, z->cd[i].size);
        wr32(e + 24, z->cd[i].size);
        wr16(e + 28, (uint32_t)nl);
        wr32(e + 42, z->cd[i].off);
        memcpy(e + 46, z->cd[i].name, nl);
        pos += 46 + nl;
    }
    uint8_t *e = buf + pos;
    memset(e, 0, 22);
    wr32(e, 0x06054b50);
    wr16(e + 8, (uint32_t)z->cd_n);
    wr16(e + 10, (uint32_t)z->cd_n);
    wr32(e + 12, (uint32_t)pos);
    wr32(e + 16, cd_off);
    zs_chunk(z, buf, pos + 22);
    free(buf);
    if (z->fd < 0 && !z->broken && z->started && sock_send(z->sock, "0\r\n\r\n", 5)) z->broken = 1;
}

/* Text für JSON: " \ und Steuerzeichen maskieren, übrige Bytes (UTF-8) bleiben */
static size_t json_put(char *out, size_t n, const char *s) {
    size_t i = 0;
    for (; *s && i + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[i++] = '\\'; out[i++] = (char)c; }
        else if (c < 0x20) i += (size_t)snprintf(out + i, n - i, "\\u%04x", c);
        else out[i++] = (char)c;
    }
    out[i] = 0;
    return i;
}

/* Spielstand als zwei Einträge in den ZIP-Strom: <prefix>metadata.json und <prefix><Titel>_<Name>.img (Container in einem
 * Durchgang, CRC und Größe hinter den Daten). 0 = ok; -1 = Fehler vor dem ersten Byte dieses Spielstands (err gefüllt);
 * -2 = Fehler im Strom */
static int zs_save(zs_t *z, const save_info_t *s, const char *prefix, char *err, size_t errsz) {
    int fd = open(s->path, O_RDONLY), rc = -1;
    struct stat st;
    uint8_t *buf = NULL;
    if (fd < 0 || fstat(fd, &st) != 0) {
        snprintf(err, errsz, T("Spielstand konnte nicht geöffnet werden: %s"), strerror(errno));
        goto done;
    }
    if ((uint64_t)st.st_size < IMG_MIN || (uint64_t)st.st_size > IMG_MAX) {
        snprintf(err, errsz, "%s", T("Der Spielstand hat eine ungültige Größe"));
        goto done;
    }
    uint32_t imgsz = (uint32_t)st.st_size;
    char tid[96], sdir[800], uid[64], user[256], tname[1100], meta[2600], mname[420], iname[420];
    json_put(tid, sizeof(tid), s->title_id);
    json_put(sdir, sizeof(sdir), s->save_dir);
    json_put(uid, sizeof(uid), s->uid);
    json_put(user, sizeof(user), s->username);
    json_put(tname, sizeof(tname), s->title_name);
    int ml = snprintf(meta, sizeof(meta),
                      "{\"format\":1,\"platform\":\"%s\",\"title_id\":\"%s\",\"title_name\":\"%s\",\"save_dir\":\"%s\",\"uid\":\"%s\","
                      "\"username\":\"%s\",\"size\":%u,\"timestamp\":%lld,\"mtime\":\"%lld\"}",
                      s->platform[0] ? s->platform : "PS5", tid, tname, sdir, uid, user, imgsz, (long long)time(NULL),
                      (long long)st.st_mtime);
    char kname[420];
    uint8_t keybuf[96];
    int ps4 = s->key_path[0] != 0;
    if (ps4) {   /* PS4: der verschlüsselte Schlüssel (<Name>.bin, genau 96 Byte) gehört zum Container */
        int kfd = open(s->key_path, O_RDONLY);
        int kok = kfd >= 0 && pread_full(kfd, keybuf, sizeof(keybuf), 0) == 0;
        if (kfd >= 0) close(kfd);
        if (!kok) {
            snprintf(err, errsz, "%s", T("Die Schlüsseldatei des PS4-Spielstands konnte nicht gelesen werden"));
            goto done;
        }
    }
    if (ml <= 0 || ml >= (int)sizeof(meta) ||
        path_fmt(mname, sizeof(mname), "%smetadata.json", prefix) ||
        path_fmt(kname, sizeof(kname), "%s%s_%s.bin", prefix, s->title_id, s->save_dir) ||
        path_fmt(iname, sizeof(iname), "%s%s_%s.img", prefix, s->title_id, s->save_dir)) {
        snprintf(err, errsz, "%s", T("Der Spielstand hat eine ungültige Größe"));
        goto done;
    }
    buf = malloc(CHUNK);
    if (!buf) { snprintf(err, errsz, "%s", T("Kein Speicher")); goto done; }

    if (!zs_local(z, mname, 0x0800, crc32_update(0, meta, (size_t)ml), (uint32_t)ml)) goto stream;
    zs_data(z, meta, (size_t)ml);
    zcd_t *ie = zs_local(z, iname, 0x0808, 0, 0);   /* CRC und Größe stehen hinter den Daten */
    if (!ie) goto stream;
    size_t idx = (size_t)(ie - z->cd);
    z->cd[idx].size = imgsz;
    uint32_t crc = 0;
    for (uint64_t pos = 0; pos < imgsz && !z->broken;) {
        size_t want = imgsz - pos < CHUNK ? (size_t)(imgsz - pos) : CHUNK;
        if (pread_full(fd, buf, want, (off_t)pos)) goto stream;
        crc = crc32_update(crc, buf, want);
        zs_data(z, buf, want);
        pos += want;
        if (z->prog && z->prog(z->pctx, pos, imgsz)) {   /* abgebrochen: nichts davon bleibt liegen (Aufrufer löscht die .tmp) */
            snprintf(err, errsz, "%s", T("Abgebrochen."));
            rc = -1;
            goto done;
        }
    }
    z->cd[idx].crc = crc;
    uint8_t dd[16];
    wr32(dd, 0x08074b50);
    wr32(dd + 4, crc);
    wr32(dd + 8, imgsz);
    wr32(dd + 12, imgsz);
    zs_data(z, dd, sizeof(dd));
    if (ps4) {
        if (!zs_local(z, kname, 0x0800, crc32_update(0, keybuf, sizeof(keybuf)), sizeof(keybuf))) goto stream;
        zs_data(z, keybuf, sizeof(keybuf));
    }
    rc = z->broken ? -2 : 0;
    goto done;
stream:
    rc = -2;
done:
    free(buf);
    if (fd >= 0) close(fd);
    return rc;
}

static void zs_free(zs_t *z) { free(z->cd); z->cd = NULL; }

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

int saves_download(int sock, const save_info_t *list, int n, char *err, size_t errsz) {
    if (n < 1) { snprintf(err, errsz, "%s", T("Kein Spielstand ausgewählt")); return -1; }
    if (op_read(err, errsz)) return -1;
    int rc = -1;
    uint64_t total = 0;
    zs_t z;
    memset(&z, 0, sizeof(z));
    z.sock = sock;
    z.fd = -1;
    for (int i = 0; i < n; i++) total += list[i].size + 1024;
    if (total > IMG_MAX) {
        snprintf(err, errsz, "%s", T("Zusammen zu groß für eine ZIP-Datei (höchstens 3,7 GB) – weniger Spielstände wählen"));
        goto done;
    }
    if (n == 1)
        snprintf(z.head, sizeof(z.head),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                 "Content-Disposition: attachment; filename=\"%s_%s.zip\"\r\nTransfer-Encoding: chunked\r\n"
                 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                 list[0].title_id, list[0].save_dir);
    else
        snprintf(z.head, sizeof(z.head),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                 "Content-Disposition: attachment; filename=\"Spielstaende_%d.zip\"\r\nTransfer-Encoding: chunked\r\n"
                 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", n);
    struct timeval tv = {60, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    for (int i = 0; i < n; i++) {
        char prefix[400] = "", e2[256] = "";
        if (n > 1 && path_fmt(prefix, sizeof(prefix), "%s_%s_%s/", list[i].uid, list[i].title_id, list[i].save_dir)) {
            snprintf(err, errsz, "%s", T("Pfad zu lang"));
            goto done;
        }
        int r = zs_save(&z, &list[i], prefix, e2, sizeof(e2));
        if (r == -1) {   /* nichts davon gesendet: bei einem noch nicht begonnenen Strom darf der Aufrufer antworten */
            if (!z.started) { snprintf(err, errsz, "%s", e2); rc = -1; }
            else rc = -2;
            goto done;
        }
        if (r < 0) { rc = z.started ? -2 : -1; if (rc == -1) snprintf(err, errsz, "%s", T("Spielstand konnte nicht gelesen werden")); goto done; }
    }
    zs_finish(&z);
    rc = z.broken ? -2 : 0;
done:
    zs_free(&z);
    op_leave();
    return rc;
}

#define BACKUP_KEEP 3   /* je Spielstand und Ort: die neueste Sicherung und die zwei davor */

/* Vorversion: Name endet auf ".<JJJJMMTT-HHMMSS>.zip" hinter dem Präfix "<Titel>_<Name>" */
static int is_version_name(const char *name, const char *prefix) {
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl) != 0 || name[pl] != '.' || strlen(name) != pl + 1 + 15 + 4 || strcmp(name + pl + 16, ".zip") != 0)
        return 0;
    for (int i = 0; i < 15; i++)
        if (i == 8 ? name[pl + 1 + i] != '-' : !isdigit((unsigned char)name[pl + 1 + i])) return 0;
    return 1;
}

static int cmp_str_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

/* Die bisherige Sicherung (final_path) als Vorversion beiseitelegen und alte Vorversionen entfernen. Fehler hier lassen die
 * neue Sicherung nicht scheitern: im schlimmsten Fall wird die alte wie früher ersetzt. */
static void rotate_backups(const char *dir, const char *prefix, const char *final_path) {
    struct stat st;
    if (stat(final_path, &st) == 0 && S_ISREG(st.st_mode)) {
        struct tm tmv;
        char stamp[24], old[1400];
        time_t t = st.st_mtime;
        gmtime_r(&t, &tmv);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        if (!path_fmt(old, sizeof(old), "%s/%s.%s.zip", dir, prefix, stamp)) (void)rename(final_path, old);
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
        qsort(names, (size_t)n, sizeof(*names), cmp_str_desc);   /* neueste zuerst */
        for (int i = BACKUP_KEEP - 1; i < n; i++) {
            char p[1400];
            if (!path_fmt(p, sizeof(p), "%s/%s", dir, names[i])) unlink(p);
        }
    }
    free(names);
}

int saves_backup_file(const save_info_t *s, const char *root, char *out, size_t outsz, saves_prog_fn prog, void *pctx,
                      char *err, size_t errsz) {
    if (op_read(err, errsz)) return -1;
    int rc = -1, fd = -1;
    char user[64], dir[1000], tmp[1300], final_path[1300], e2[256] = "";
    zs_t z;
    memset(&z, 0, sizeof(z));
    z.fd = -1;
    z.prog = prog;
    z.pctx = pctx;
    safe_name(user, 40, s->username[0] ? s->username : s->uid);
    if (path_fmt(dir, sizeof(dir), "%s/%s_%s", root, user, s->uid) ||
        path_fmt(final_path, sizeof(final_path), "%s/%s_%s.zip", dir, s->title_id, s->save_dir) ||
        path_fmt(tmp, sizeof(tmp), "%s/.%s_%s.zip.tmp", dir, s->title_id, s->save_dir)) {
        snprintf(err, errsz, "%s", T("Pfad zu lang"));
        goto done;
    }
    if (!space_ok(root, s->size + 4096)) {
        snprintf(err, errsz, "%s", T("Am Ziel ist nicht mehr genug Platz frei."));
        goto done;
    }
    mkdirs(root);
    mkdirs(dir);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(err, errsz, T("Zieldatei konnte nicht angelegt werden: %s"), strerror(errno));
        goto done;
    }
    z.fd = fd;
    if (zs_save(&z, s, "", e2, sizeof(e2)) != 0 || (zs_finish(&z), z.broken) || fsync(fd) != 0) {
        snprintf(err, errsz, "%s", e2[0] ? e2 : T("Schreibfehler: Datenträger voll oder nicht beschreibbar"));
        goto done;
    }
    if (close(fd) != 0) { fd = -1; snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno)); goto done; }
    fd = -1;
    {
        char prefix[600];
        if (!path_fmt(prefix, sizeof(prefix), "%s_%s", s->title_id, s->save_dir)) rotate_backups(dir, prefix, final_path);
    }
    if (rename(tmp, final_path) != 0) {
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    tmp[0] = 0;
    scpy(out, outsz, final_path);
    rc = 0;
done:
    if (fd >= 0) close(fd);
    if (tmp[0]) unlink(tmp);
    zs_free(&z);
    op_leave();
    return rc;
}

/* ── ZIP lesen ───────────────────────────────────────────────────────── */

typedef struct { char name[400]; uint32_t crc, size; uint64_t data; } zent_t;

#define ZIP_MAXENT 200   /* bis zu 64 Spielstände (je zwei bis drei Einträge) in einer ZIP */
#define ZIP_MAXSAVES 64

/* Zentralverzeichnis lesen und jeden Eintrag prüfen: gespeichert (ohne Kompression), keine Verschlüsselung, kein
 * ZIP64, Daten innerhalb der Datei */
static int zip_dir_n(int fd, uint64_t fsz, zent_t *ents, int cap, int *n, char *err, size_t errsz) {
    *n = 0;
    if (fsz < 22) goto bad;
    size_t tail = fsz < 22 + 65535 ? (size_t)fsz : 22 + 65535;
    uint8_t *t = malloc(tail);
    if (!t) { snprintf(err, errsz, "%s", T("Kein Speicher")); return -1; }
    if (pread_full(fd, t, tail, (off_t)(fsz - tail))) { free(t); goto bad; }
    ssize_t e = -1;
    for (ssize_t i = (ssize_t)tail - 22; i >= 0; i--)
        if (rd32(t + i) == 0x06054b50 && (size_t)i + 22 + rd16(t + i + 20) == tail) { e = i; break; }
    if (e < 0) { free(t); goto bad; }
    uint32_t disk = rd16(t + e + 4), cdisk = rd16(t + e + 6), n1 = rd16(t + e + 8), n2 = rd16(t + e + 10);
    uint32_t cd_size = rd32(t + e + 12), cd_off = rd32(t + e + 16);
    uint64_t eocd_pos = fsz - tail + (uint64_t)e;
    free(t);
    if (disk || cdisk || n1 != n2 || !n1 || n1 > (uint32_t)cap || cd_size > 65536 || (uint64_t)cd_off + cd_size > eocd_pos)
        goto bad;
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    if (!cd) { snprintf(err, errsz, "%s", T("Kein Speicher")); return -1; }
    if (pread_full(fd, cd, cd_size, (off_t)cd_off)) { free(cd); goto bad; }
    size_t pos = 0;
    for (uint32_t i = 0; i < n1; i++) {
        if (pos + 46 > cd_size || rd32(cd + pos) != 0x02014b50) { free(cd); goto bad; }
        uint32_t flags = rd16(cd + pos + 8), method = rd16(cd + pos + 10), crc = rd32(cd + pos + 16);
        uint32_t csize = rd32(cd + pos + 20), usize = rd32(cd + pos + 24), nl = rd16(cd + pos + 28);
        uint32_t xl = rd16(cd + pos + 30), cl = rd16(cd + pos + 32), lho = rd32(cd + pos + 42);
        if (pos + 46 + nl + xl + cl > cd_size || nl >= sizeof(ents[0].name) || (flags & 1) || method != 0 ||
            csize != usize || csize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) { free(cd); goto bad; }
        zent_t *z = &ents[*n];
        memcpy(z->name, cd + pos + 46, nl);
        z->name[nl] = 0;
        z->crc = crc;
        z->size = usize;
        uint8_t lh[30];
        if (pread_full(fd, lh, sizeof(lh), (off_t)lho) || rd32(lh) != 0x04034b50) { free(cd); goto bad; }
        z->data = (uint64_t)lho + 30 + rd16(lh + 26) + rd16(lh + 28);
        if (z->data + usize > cd_off) { free(cd); goto bad; }
        (*n)++;
        pos += 46 + nl + xl + cl;
    }
    free(cd);
    return 0;
bad:
    snprintf(err, errsz, "%s", T("Keine gültige Spielstand-ZIP-Datei"));
    return -1;
}

static zent_t *zip_get(zent_t *ents, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(ents[i].name, name)) return &ents[i];
    return NULL;
}

/* Wert eines Schlüssels der obersten Ebene (nur Zeichenketten): String-Wert, \"-Maskierungen werden aufgelöst */
static int meta_get(const char *j, const char *key, char *out, size_t n) {
    const char *p = j;
    size_t kl = strlen(key);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p++ != '{') return -1;
    for (;;) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (*p != '"') return -1;
        p++;
        char k[64];
        size_t ki = 0;
        for (; *p && *p != '"'; p++) {
            if (*p == '\\' && p[1]) p++;
            if (ki + 1 < sizeof(k)) k[ki++] = *p;
        }
        if (*p != '"') return -1;
        k[ki] = 0;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p++ != ':') return -1;
        while (*p && isspace((unsigned char)*p)) p++;
        int match = ki == kl && !strcmp(k, key);
        if (*p == '"') {
            p++;
            size_t vi = 0;
            int cut = 0;
            for (; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1]) p++;
                if (vi + 1 < n) out[vi++] = *p;
                else cut = 1;
            }
            if (*p != '"') return -1;
            p++;
            out[vi] = 0;
            if (match) return cut ? -1 : 0;
        } else {
            while (*p && *p != ',' && *p != '}') p++;
            if (match) return -1;   /* kein String */
        }
        if (*p == '}' || !*p) return -1;
    }
}

/* ein zu schreibender Spielstand aus der ZIP (geprüft) */
typedef struct {
    char   tid[32], sdir[256], uid[16];
    int    ps4;                    /* PS4: zusätzlich die Schlüsseldatei key (96 Byte) nach kdest */
    zent_t *img, *key;
    char   dest[1300], tdir[900], d2[1300], tmp[1300];
    char   kdest[1300], ktmp[1300];
    time_t mtime;                  /* Änderungszeit des gesicherten Containers (0 = unbekannt) */
} rjob_t;

/* Eintrag "<prefix>metadata.json" prüfen und den Zielpfad bestimmen. 0 = ok */
static int rplan(int zfd, zent_t *ents, int ne, zent_t *me, rjob_t *r, char *err, size_t errsz) {
    char prefix[400], meta[META_MAX + 1], imgname[800];
    size_t pl = strlen(me->name) - strlen("metadata.json");
    memcpy(prefix, me->name, pl);
    prefix[pl] = 0;
    if (!me->size || me->size > META_MAX || strstr(prefix, "..") || prefix[0] == '/' || strchr(prefix, '\\')) {
        snprintf(err, errsz, "%s", T("metadata.json fehlt im ZIP oder ist ungültig"));
        return -1;
    }
    if (pread_full(zfd, meta, me->size, (off_t)me->data) || crc32_update(0, meta, me->size) != me->crc) {
        snprintf(err, errsz, "%s", T("metadata.json fehlt im ZIP oder ist ungültig"));
        return -1;
    }
    meta[me->size] = 0;
    if (meta_get(meta, "title_id", r->tid, sizeof(r->tid)) || meta_get(meta, "save_dir", r->sdir, sizeof(r->sdir)) ||
        meta_get(meta, "uid", r->uid, sizeof(r->uid)) || !saves_name_ok(r->tid, sizeof(r->tid)) ||
        !saves_name_ok(r->sdir, sizeof(r->sdir)) || !saves_name_ok(r->uid, sizeof(r->uid))) {
        snprintf(err, errsz, "%s", T("Die Angaben in metadata.json sind unvollständig oder ungültig"));
        return -1;
    }
    if (path_fmt(imgname, sizeof(imgname), "%s%s_%s.img", prefix, r->tid, r->sdir)) {
        snprintf(err, errsz, "%s", T("Pfad zu lang"));
        return -1;
    }
    char plat[8] = "PS5";
    if (meta_get(meta, "platform", plat, sizeof(plat)) || (strcmp(plat, "PS5") && strcmp(plat, "PS4"))) scpy(plat, sizeof(plat), "PS5");
    r->ps4 = !strcmp(plat, "PS4");
    char mts[24] = "";
    r->mtime = 0;
    if (meta_get(meta, "mtime", mts, sizeof(mts)) == 0 && strlen(mts) > 0 && strlen(mts) < 12 && strspn(mts, "0123456789") == strlen(mts))
        r->mtime = (time_t)strtoll(mts, NULL, 10);
    r->key = NULL;
    r->img = zip_get(ents, ne, imgname);
    if (!r->img) {
        snprintf(err, errsz, T("Container (%s) fehlt im ZIP"), imgname);
        return -1;
    }
    if (r->img->size < IMG_MIN || r->img->size > IMG_MAX) {
        snprintf(err, errsz, "%s", T("Der Spielstand hat eine ungültige Größe"));
        return -1;
    }
    uint8_t first;
    if (pread_full(zfd, &first, 1, (off_t)r->img->data)) {
        snprintf(err, errsz, "%s", T("Keine gültige Spielstand-ZIP-Datei"));
        return -1;
    }
    if ((first == 0x01) != r->ps4) {   /* Byte 0 des Containers: 1 = PS4, sonst PS5 - die Angabe in metadata.json muss dazu passen */
        snprintf(err, errsz, "%s", T("Konsole in metadata.json und Container passen nicht zusammen"));
        return -1;
    }
    if (r->ps4) {
        char kname[800];
        if (path_fmt(kname, sizeof(kname), "%s%s_%s.bin", prefix, r->tid, r->sdir) || strncmp(r->sdir, "sdimg_", 6) ||
            !(r->key = zip_get(ents, ne, kname)) || r->key->size != 96) {
            snprintf(err, errsz, "%s", T("Die Schlüsseldatei des PS4-Spielstands fehlt im ZIP"));
            return -1;
        }
    }
    const char *home = plat_saves_dir();
    char udir[700];
    struct stat st;
    if (!home || path_fmt(udir, sizeof(udir), "%s/%s", home, r->uid) || stat(udir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, errsz, T("Der Benutzer %s ist auf dieser PS5 nicht vorhanden"), r->uid);
        return -1;
    }
    if (path_fmt(r->d2, sizeof(r->d2), "%s/%s", udir, r->ps4 ? SAVE_SUBDIR_PS4 : SAVE_SUBDIR) || path_fmt(r->tdir, sizeof(r->tdir), "%s/%s", r->d2, r->tid) ||
        path_fmt(r->dest, sizeof(r->dest), "%s/%s", r->tdir, r->sdir) ||
        path_fmt(r->tmp, sizeof(r->tmp), "%s/.%s.restore.tmp", r->tdir, r->sdir)) {
        snprintf(err, errsz, "%s", T("Pfad zu lang"));
        return -1;
    }
    if (r->ps4 && (path_fmt(r->kdest, sizeof(r->kdest), "%s/%s.bin", r->tdir, r->sdir + 6) ||
                   path_fmt(r->ktmp, sizeof(r->ktmp), "%s/.%s.bin.restore.tmp", r->tdir, r->sdir + 6))) {
        snprintf(err, errsz, "%s", T("Pfad zu lang"));
        return -1;
    }
    if (!space_ok(udir, r->img->size)) {
        snprintf(err, errsz, "%s", T("Nicht genug freier Speicher auf der PS5"));
        return -1;
    }
    return 0;
}

/* Spielstand schreiben: erst daneben, nach der Prüfsumme umbenennen. 0 = ok */
static int rexec(int zfd, rjob_t *r, saves_prog_fn prog, void *pctx, uint64_t base, uint64_t total, char *err, size_t errsz) {
    struct stat st;
    mode_t mode = 0644;
    uid_t own_u = (uid_t)-1;
    gid_t own_g = (gid_t)-1;
    int rc = -1, dfd = -1;
    uint8_t *buf = NULL;
    /* Zeiten: die Datei behält ihre Änderungszeit (die Datenbank der PS5 führt sie mit); gibt es sie noch nicht, gilt die
     * Zeit aus der Sicherung */
    struct timespec ts[2], kts[2];
    int have_ts = 0, have_kts = 0;
    mkdir(r->d2, 0777);
    mkdir(r->tdir, 0777);
    if (stat(r->dest, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            snprintf(err, errsz, "%s", T("Ziel ist keine Datei"));
            return -1;
        }
        mode = st.st_mode & 0777;
        own_u = st.st_uid;
        own_g = st.st_gid;
        ts[0] = st.st_atim;
        ts[1] = st.st_mtim;
        have_ts = 1;
    } else if (r->mtime > 0) {
        ts[0].tv_sec = ts[1].tv_sec = r->mtime;
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
        have_ts = 1;
    }
    if (r->ps4) {
        struct stat ks0;
        if (stat(r->kdest, &ks0) == 0 && S_ISREG(ks0.st_mode)) { kts[0] = ks0.st_atim; kts[1] = ks0.st_mtim; have_kts = 1; }
        else if (have_ts) { kts[0] = ts[0]; kts[1] = ts[1]; have_kts = 1; }
    }
    unlink(r->tmp);
    dfd = open(r->tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (dfd < 0) {
        snprintf(err, errsz, T("Zieldatei konnte nicht angelegt werden: %s"), strerror(errno));
        return -1;
    }
    buf = malloc(CHUNK);
    if (!buf) { snprintf(err, errsz, "%s", T("Kein Speicher")); goto done; }
    uint32_t crc = 0;
    int bad = 0;
    for (uint64_t pos = 0; pos < r->img->size && !bad;) {
        size_t want = r->img->size - pos < CHUNK ? (size_t)(r->img->size - pos) : CHUNK;
        if (pread_full(zfd, buf, want, (off_t)(r->img->data + pos)) || write_full(dfd, buf, want)) bad = 1;
        else {
            crc = crc32_update(crc, buf, want);
            pos += want;
            if (prog && prog(pctx, base + pos, total)) { snprintf(err, errsz, "%s", T("Abgebrochen.")); goto done; }
        }
    }
    if (bad) {
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    if (crc != r->img->crc) {
        snprintf(err, errsz, "%s", T("Prüfsumme des Spielstands stimmt nicht – die ZIP-Datei ist beschädigt"));
        goto done;
    }
    fchmod(dfd, mode);
    if (own_u != (uid_t)-1) (void)!fchown(dfd, own_u, own_g);
    if (have_ts) set_times_fd(dfd, NULL, ts);
    if (fsync(dfd) != 0 || close(dfd) != 0) {
        dfd = -1;
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    dfd = -1;
    if (r->ps4) {   /* Schlüssel daneben schreiben und prüfen, dann beide umbenennen (Schlüssel zuerst: er ist 96 Byte klein) */
        uint8_t kb[96];
        int kfd = -1;
        mode_t kmode = mode;
        uid_t ku = own_u;
        gid_t kg = own_g;
        struct stat ks;
        if (stat(r->kdest, &ks) == 0 && S_ISREG(ks.st_mode)) { kmode = ks.st_mode & 0777; ku = ks.st_uid; kg = ks.st_gid; }
        unlink(r->ktmp);
        if (pread_full(zfd, kb, sizeof(kb), (off_t)r->key->data) || crc32_update(0, kb, sizeof(kb)) != r->key->crc ||
            (kfd = open(r->ktmp, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0 || write_full(kfd, kb, sizeof(kb)) ||
            (ku != (uid_t)-1 && fchown(kfd, ku, kg) != 0 && 0) || fchmod(kfd, kmode) != 0 || (have_kts && set_times_fd(kfd, NULL, kts) != 0 && 0) ||
            fsync(kfd) != 0 || close(kfd) != 0) {
            if (kfd >= 0) close(kfd);
            unlink(r->ktmp);
            snprintf(err, errsz, "%s", T("Die Schlüsseldatei des PS4-Spielstands konnte nicht geschrieben werden"));
            goto done;
        }
        if (rename(r->ktmp, r->kdest) != 0) {
            unlink(r->ktmp);
            snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
            goto done;
        }
    }
    if (rename(r->tmp, r->dest) != 0) {
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    r->tmp[0] = 0;
    rc = 0;
done:
    free(buf);
    if (dfd >= 0) close(dfd);
    if (r->tmp[0]) unlink(r->tmp);
    return rc;
}

int saves_restore_file(const char *zip_path, int *count, saves_prog_fn prog, void *pctx, char *err, size_t errsz) {
    if (count) *count = 0;
    if (op_enter(err, errsz)) return -1;
    int rc = -1, zfd = open(zip_path, O_RDONLY), ne = 0, nj = 0;
    struct stat st;
    zent_t *ents = malloc(ZIP_MAXENT * sizeof(*ents));
    rjob_t *jobs = calloc(ZIP_MAXSAVES, sizeof(*jobs));
    if (!ents || !jobs) { snprintf(err, errsz, "%s", T("Kein Speicher")); goto done; }
    if (zfd < 0 || fstat(zfd, &st) != 0) {
        snprintf(err, errsz, T("Datei konnte nicht geöffnet werden: %s"), strerror(errno));
        goto done;
    }
    if (zip_dir_n(zfd, (uint64_t)st.st_size, ents, ZIP_MAXENT, &ne, err, errsz)) goto done;
    /* erst alle Spielstände der ZIP prüfen, dann schreiben: eine fehlerhafte ZIP ändert nichts */
    for (int i = 0; i < ne; i++) {
        size_t l = strlen(ents[i].name), m = strlen("metadata.json");
        if (l < m || strcmp(ents[i].name + l - m, "metadata.json") != 0 || (l > m && ents[i].name[l - m - 1] != '/')) continue;
        if (nj == ZIP_MAXSAVES) {
            snprintf(err, errsz, "%s", T("Zu viele Spielstände in einer ZIP-Datei (höchstens 64)"));
            goto done;
        }
        for (int k = 0; k < nj; k++) {   /* zweimal derselbe Spielstand in einer ZIP: Mehrdeutigkeit abweisen */
            (void)k;
        }
        if (rplan(zfd, ents, ne, &ents[i], &jobs[nj], err, errsz)) goto done;
        for (int k = 0; k < nj; k++)
            if (!strcmp(jobs[k].dest, jobs[nj].dest)) {
                snprintf(err, errsz, "%s", T("Keine gültige Spielstand-ZIP-Datei"));
                goto done;
            }
        nj++;
    }
    if (!nj) {
        snprintf(err, errsz, "%s", T("metadata.json fehlt im ZIP oder ist ungültig"));
        goto done;
    }
    uint64_t total = 0, base = 0;
    for (int i = 0; i < nj; i++) total += jobs[i].img->size;
    for (int i = 0; i < nj; i++) {
        if (rexec(zfd, &jobs[i], prog, pctx, base, total, err, errsz)) {
            if (i) {   /* schon zurückgeschriebene sind fertig - dem Anwender sagen, wie viele */
                char first[256];
                scpy(first, sizeof(first), err);
                snprintf(err, errsz, T("%d von %d Spielständen wiederhergestellt, dann Fehler: %s"), i, nj, first);
            }
            if (count) *count = i;
            goto done;
        }
        base += jobs[i].img->size;
    }
    if (count) *count = nj;
    rc = 0;
done:
    if (zfd >= 0) close(zfd);
    free(ents);
    free(jobs);
    op_leave();
    return rc;
}

/* ── Sicherungen auf der PS5 (<root>/<Benutzer>_<uid>/<Titel>_<Name>.zip) auflisten ── */

static int cmp_backups(const void *a, const void *b) {
    const save_backup_t *x = a, *y = b;
    int c = strcmp(x->username, y->username);
    if (c) return c;
    c = strcmp(x->title_name, y->title_name);
    if (!c) c = strcmp(x->save_dir, y->save_dir);
    if (c) return c;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : 0;   /* gleiche Sicherung: neueste zuerst */
}

int saves_list_backups(const char *root, save_backup_t **out, int *count) {
    *out = NULL;
    *count = 0;
    DIR *rd = opendir(root);
    if (!rd) return -1;
    save_backup_t *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ue;
    while ((ue = readdir(rd))) {
        char ud[1000];
        if (ue->d_name[0] == '.' || path_fmt(ud, sizeof(ud), "%s/%s", root, ue->d_name)) continue;
        DIR *dd = opendir(ud);
        struct dirent *fe;
        while (dd && (fe = readdir(dd))) {
            size_t l = strlen(fe->d_name);
            char fp[1300];
            struct stat st;
            if (fe->d_name[0] == '.' || l < 5 || strcmp(fe->d_name + l - 4, ".zip") != 0 ||
                path_fmt(fp, sizeof(fp), "%s/%s", ud, fe->d_name) || stat(fp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            int fd = open(fp, O_RDONLY);
            zent_t ents[8];
            int ne = 0;
            char e2[100], meta[META_MAX + 1];
            if (fd < 0) continue;
            zent_t *me = NULL;
            if (zip_dir_n(fd, (uint64_t)st.st_size, ents, 8, &ne, e2, sizeof(e2)) == 0) me = zip_get(ents, ne, "metadata.json");
            if (me && me->size && me->size <= META_MAX && pread_full(fd, meta, me->size, (off_t)me->data) == 0 &&
                crc32_update(0, meta, me->size) == me->crc) {
                meta[me->size] = 0;
                if (n == cap) {
                    save_backup_t *p = realloc(arr, (size_t)(cap = cap ? cap * 2 : 32) * sizeof(*p));
                    if (!p) { free(arr); close(fd); closedir(dd); closedir(rd); return -1; }
                    arr = p;
                }
                save_backup_t *b = &arr[n];
                memset(b, 0, sizeof(*b));
                if (meta_get(meta, "title_id", b->title_id, sizeof(b->title_id)) == 0 &&
                    meta_get(meta, "save_dir", b->save_dir, sizeof(b->save_dir)) == 0 &&
                    meta_get(meta, "uid", b->uid, sizeof(b->uid)) == 0 && saves_name_ok(b->title_id, sizeof(b->title_id)) &&
                    saves_name_ok(b->save_dir, sizeof(b->save_dir)) && saves_name_ok(b->uid, sizeof(b->uid)) &&
                    !path_fmt(b->path, sizeof(b->path), "%s", fp)) {
                    meta_get(meta, "username", b->username, sizeof(b->username));
                    if (meta_get(meta, "platform", b->platform, sizeof(b->platform)) || !b->platform[0])
                        scpy(b->platform, sizeof(b->platform), "PS5");
                    if (meta_get(meta, "title_name", b->title_name, sizeof(b->title_name)) || !b->title_name[0])
                        scpy(b->title_name, sizeof(b->title_name), b->title_id);
                    b->size = (uint64_t)st.st_size;
                    b->mtime = st.st_mtime;
                    char mt[24] = "";
                    if (meta_get(meta, "mtime", mt, sizeof(mt)) == 0 && strlen(mt) < 12 && strspn(mt, "0123456789") == strlen(mt))
                        b->save_mtime = (int64_t)strtoll(mt, NULL, 10);
                    n++;
                }
            }
            close(fd);
            if (n >= SAVES_MAX) break;
        }
        if (dd) closedir(dd);
    }
    closedir(rd);
    if (n > 1) qsort(arr, (size_t)n, sizeof(*arr), cmp_backups);
    *out = arr;
    *count = n;
    return 0;
}

/* Zeit einer Datei setzen: erst futimens, dann (die PS5-libc meldet dort ENOENT) futimes und utimes, zuletzt utimensat */
static int set_times_fd(int fd, const char *path, const struct timespec ts[2]) {
    struct timeval tv[2];
    tv[0].tv_sec = ts[0].tv_sec; tv[0].tv_usec = ts[0].tv_nsec / 1000;
    tv[1].tv_sec = ts[1].tv_sec; tv[1].tv_usec = ts[1].tv_nsec / 1000;
    if (fd >= 0 && futimens(fd, ts) == 0) return 0;
    if (fd >= 0 && futimes(fd, tv) == 0) return 0;
    if (path && utimes(path, tv) == 0) return 0;
    if (path && utimensat(AT_FDCWD, path, ts, 0) == 0) return 0;
    return -1;
}

static int set_file_time(const char *path, time_t t) {
    struct timespec ts[2];
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    ts[0].tv_sec = ts[1].tv_sec = t;
    int fd = open(path, O_RDONLY);
    int rc = set_times_fd(fd, path, ts);
    int e = errno;
    if (fd >= 0) close(fd);
    errno = e;
    return rc;
}

int saves_set_times(const char *key, int64_t mtime, int64_t mtime_key, char *err, size_t errsz) {
    save_info_t s;
    if (mtime < 946684800 || mtime > 4102444800LL || mtime_key > 4102444800LL) {   /* 2000 .. 2100 */
        snprintf(err, errsz, "%s", T("Ungültige Zeitangabe"));
        return -1;
    }
    if (saves_find(key, &s)) {
        snprintf(err, errsz, "%s", T("Spielstand nicht gefunden"));
        return -1;
    }
    if (op_enter(err, errsz)) return -1;
    int rc = -1;
    if (set_file_time(s.path, (time_t)mtime) != 0) {
        snprintf(err, errsz, T("Zeitstempel konnte nicht gesetzt werden: %s"), strerror(errno));
        goto done;
    }
    if (mtime_key >= 0 && s.key_path[0] && set_file_time(s.key_path, (time_t)mtime_key) != 0) {
        snprintf(err, errsz, T("Zeitstempel konnte nicht gesetzt werden: %s"), strerror(errno));
        goto done;
    }
    rc = 0;
done:
    op_leave();
    return rc;
}
