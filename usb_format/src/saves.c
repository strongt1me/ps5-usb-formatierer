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
#define IMG_MIN         0x1000ull
#define IMG_MAX         0xF0000000ull   /* ZIP ohne ZIP64: Größen passen in 32 Bit */
#define META_MAX        4096
#define SAVES_MAX       4096
#define CHUNK           65536
#define FREE_MARGIN     (1ull << 20)

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static pthread_mutex_t g_op = PTHREAD_MUTEX_INITIALIZER;

/* ein Vorgang zur Zeit: Sichern, Wiederherstellen und Neusignieren würden sich an derselben Datei in die Quere kommen */
static int op_enter(char *err, size_t errsz) {
    if (pthread_mutex_trylock(&g_op) == 0) return 0;
    snprintf(err, errsz, "%s", T("Es läuft bereits ein Spielstand-Vorgang"));
    return -1;
}
static void op_leave(void) { pthread_mutex_unlock(&g_op); }

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

/* Spielname aus der param.sfo des installierten Titels: <Ordner über /user/home>/appmeta (PS5: /user/appmeta) */
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
        if (path_fmt(p, sizeof(p), "%s/%s/param.sfo", dirs[i], title_id)) continue;
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        uint8_t buf[16384];
        ssize_t r = read(fd, buf, sizeof(buf));
        close(fd);
        char tid[32] = "";
        if (r > 0 && lib_parse_param_sfo(buf, (size_t)r, tid, sizeof(tid), name, n) == 0 && name[0]) return;
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
        if (path_fmt(pdir, sizeof(pdir), "%s/%s/" SAVE_SUBDIR, home, ue->d_name)) continue;
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
                snprintf(s->key, sizeof(s->key), "%s/%s/%s", s->uid, s->title_id, s->save_dir);
                s->size = (uint64_t)st.st_size;
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

/* ── ZIP schreiben (gespeichert, ohne Deflate), Strom als chunked HTTP ─ */

typedef struct {
    int      sock, broken, started;
    char     head[768];
    uint64_t off;
    struct { char name[300]; uint32_t crc, size, off; uint16_t flags; } cd[2];
    int      cd_n;
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

/* ein HTTP-Chunk; eine Länge 0 würde den Strom beenden und wird nie gesendet */
static void zs_chunk(zs_t *z, const void *buf, size_t len) {
    if (z->broken || !len) return;
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

/* flags: 0x0800 UTF-8, 0x0008 = Größen/CRC folgen hinter den Daten */
static void zs_local(zs_t *z, const char *name, uint16_t flags, uint32_t crc, uint32_t size) {
    uint8_t h[30 + 300];
    size_t nl = strlen(name);
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
    z->cd[z->cd_n].crc = crc;
    z->cd[z->cd_n].size = size;
    z->cd[z->cd_n].off = (uint32_t)z->off;
    z->cd[z->cd_n].flags = flags;
    scpy(z->cd[z->cd_n].name, sizeof(z->cd[0].name), name);
    z->cd_n++;
    zs_data(z, h, 30 + nl);
}

static void zs_finish(zs_t *z) {
    uint8_t buf[2 * (46 + 300) + 22];
    size_t pos = 0;
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
    if (!z->broken && z->started && sock_send(z->sock, "0\r\n\r\n", 5)) z->broken = 1;
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

int saves_download(int sock, const save_info_t *s, char *err, size_t errsz) {
    if (op_enter(err, errsz)) return -1;
    int rc = -1;
    int fd = open(s->path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        snprintf(err, errsz, T("Spielstand konnte nicht geöffnet werden: %s"), strerror(errno));
        goto done;
    }
    if ((uint64_t)st.st_size < IMG_MIN || (uint64_t)st.st_size > IMG_MAX) {
        snprintf(err, errsz, "%s", T("Der Spielstand hat eine ungültige Größe"));
        goto done;
    }
    uint32_t imgsz = (uint32_t)st.st_size;

    char tid[96], sdir[800], uid[64], user[256], tname[1100], meta[2600];
    json_put(tid, sizeof(tid), s->title_id);
    json_put(sdir, sizeof(sdir), s->save_dir);
    json_put(uid, sizeof(uid), s->uid);
    json_put(user, sizeof(user), s->username);
    json_put(tname, sizeof(tname), s->title_name);
    int ml = snprintf(meta, sizeof(meta),
                      "{\"format\":1,\"title_id\":\"%s\",\"title_name\":\"%s\",\"save_dir\":\"%s\",\"uid\":\"%s\","
                      "\"username\":\"%s\",\"size\":%u,\"timestamp\":%lld}",
                      tid, tname, sdir, uid, user, imgsz, (long long)time(NULL));
    if (ml <= 0 || ml >= (int)sizeof(meta)) {
        snprintf(err, errsz, "%s", T("Der Spielstand hat eine ungültige Größe"));
        goto done;
    }
    char imgname[320];
    path_fmt(imgname, sizeof(imgname), "%s_%s.img", s->title_id, s->save_dir);

    zs_t z;
    memset(&z, 0, sizeof(z));
    z.sock = sock;
    snprintf(z.head, sizeof(z.head),
             "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
             "Content-Disposition: attachment; filename=\"%s_%s.zip\"\r\nTransfer-Encoding: chunked\r\n"
             "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
             s->title_id, s->save_dir);
    struct timeval tv = {60, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    zs_local(&z, "metadata.json", 0x0800, crc32_update(0, meta, (size_t)ml), (uint32_t)ml);
    zs_data(&z, meta, (size_t)ml);
    zs_local(&z, imgname, 0x0808, 0, 0);   /* CRC und Größe stehen hinter den Daten (ein Durchgang) */
    z.cd[1].size = imgsz;
    uint32_t crc = 0;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) { snprintf(err, errsz, "%s", T("Kein Speicher")); goto done; }
    int readerr = 0;
    for (uint64_t pos = 0; pos < imgsz && !z.broken;) {
        size_t want = imgsz - pos < CHUNK ? (size_t)(imgsz - pos) : CHUNK;
        if (pread_full(fd, buf, want, (off_t)pos)) { readerr = 1; break; }
        crc = crc32_update(crc, buf, want);
        zs_data(&z, buf, want);
        pos += want;
    }
    free(buf);
    if (readerr || z.broken) { rc = z.started ? -2 : -1; if (rc == -1) snprintf(err, errsz, "%s", T("Spielstand konnte nicht gelesen werden")); goto done; }
    z.cd[1].crc = crc;
    uint8_t dd[16];
    wr32(dd, 0x08074b50);
    wr32(dd + 4, crc);
    wr32(dd + 8, imgsz);
    wr32(dd + 12, imgsz);
    zs_data(&z, dd, sizeof(dd));
    zs_finish(&z);
    rc = z.broken ? -2 : 0;
done:
    if (fd >= 0) close(fd);
    op_leave();
    return rc;
}

/* ── ZIP lesen ───────────────────────────────────────────────────────── */

typedef struct { char name[300]; uint32_t crc, size; uint64_t data; } zent_t;

#define ZIP_MAXENT 16

/* Zentralverzeichnis lesen und jeden Eintrag prüfen: gespeichert (ohne Kompression), keine Verschlüsselung, kein
 * ZIP64, Daten innerhalb der Datei */
static int zip_dir(int fd, uint64_t fsz, zent_t *ents, int *n, char *err, size_t errsz) {
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
    if (disk || cdisk || n1 != n2 || !n1 || n1 > ZIP_MAXENT || cd_size > 16384 || (uint64_t)cd_off + cd_size > eocd_pos)
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

int saves_restore_file(const char *zip_path, char *err, size_t errsz) {
    if (op_enter(err, errsz)) return -1;
    int rc = -1, zfd = open(zip_path, O_RDONLY), dfd = -1;
    char tmp[1300] = "";
    struct stat st;
    zent_t ents[ZIP_MAXENT];
    int ne = 0;
    if (zfd < 0 || fstat(zfd, &st) != 0) {
        snprintf(err, errsz, T("Datei konnte nicht geöffnet werden: %s"), strerror(errno));
        goto done;
    }
    if (zip_dir(zfd, (uint64_t)st.st_size, ents, &ne, err, errsz)) goto done;

    zent_t *me = zip_get(ents, ne, "metadata.json");
    if (!me || !me->size || me->size > META_MAX) {
        snprintf(err, errsz, "%s", T("metadata.json fehlt im ZIP oder ist ungültig"));
        goto done;
    }
    char meta[META_MAX + 1];
    if (pread_full(zfd, meta, me->size, (off_t)me->data) || crc32_update(0, meta, me->size) != me->crc) {
        snprintf(err, errsz, "%s", T("metadata.json fehlt im ZIP oder ist ungültig"));
        goto done;
    }
    meta[me->size] = 0;
    char tid[32] = "", sdir[256] = "", uid[16] = "";
    if (meta_get(meta, "title_id", tid, sizeof(tid)) || meta_get(meta, "save_dir", sdir, sizeof(sdir)) ||
        meta_get(meta, "uid", uid, sizeof(uid)) || !saves_name_ok(tid, sizeof(tid)) ||
        !saves_name_ok(sdir, sizeof(sdir)) || !saves_name_ok(uid, sizeof(uid))) {
        snprintf(err, errsz, "%s", T("Die Angaben in metadata.json sind unvollständig oder ungültig"));
        goto done;
    }
    char imgname[320];
    path_fmt(imgname, sizeof(imgname), "%s_%s.img", tid, sdir);
    zent_t *ie = zip_get(ents, ne, imgname);
    if (!ie) {
        snprintf(err, errsz, T("Container (%s) fehlt im ZIP"), imgname);
        goto done;
    }
    if (ie->size < IMG_MIN || ie->size > IMG_MAX) {
        snprintf(err, errsz, "%s", T("Der Spielstand hat eine ungültige Größe"));
        goto done;
    }
    uint8_t first;
    if (pread_full(zfd, &first, 1, (off_t)ie->data)) goto done;
    if (first == 0x01) {
        snprintf(err, errsz, "%s", T("PS4-Spielstände werden noch nicht unterstützt"));
        goto done;
    }

    const char *home = plat_saves_dir();
    char udir[700], tdir[900], dest[1300], d2[1300];
    if (!home || path_fmt(udir, sizeof(udir), "%s/%s", home, uid) || stat(udir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, errsz, T("Der Benutzer %s ist auf dieser PS5 nicht vorhanden"), uid);
        goto done;
    }
    if (path_fmt(d2, sizeof(d2), "%s/" SAVE_SUBDIR, udir) || path_fmt(tdir, sizeof(tdir), "%s/%s", d2, tid) ||
        path_fmt(dest, sizeof(dest), "%s/%s", tdir, sdir) || path_fmt(tmp, sizeof(tmp), "%s/.%s.restore.tmp", tdir, sdir)) {
        snprintf(err, errsz, "%s", T("Pfad zu lang"));
        goto done;
    }
    if (!space_ok(udir, ie->size)) {
        snprintf(err, errsz, "%s", T("Nicht genug freier Speicher auf der PS5"));
        goto done;
    }
    mkdir(d2, 0777);
    mkdir(tdir, 0777);
    mode_t mode = 0644;
    uid_t own_u = (uid_t)-1;
    gid_t own_g = (gid_t)-1;
    if (stat(dest, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            snprintf(err, errsz, "%s", T("Ziel ist keine Datei"));
            goto done;
        }
        mode = st.st_mode & 0777;
        own_u = st.st_uid;
        own_g = st.st_gid;
    }
    unlink(tmp);
    dfd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (dfd < 0) {
        snprintf(err, errsz, T("Zieldatei konnte nicht angelegt werden: %s"), strerror(errno));
        tmp[0] = 0;
        goto done;
    }
    uint8_t *buf = malloc(CHUNK);
    if (!buf) { snprintf(err, errsz, "%s", T("Kein Speicher")); goto done; }
    uint32_t crc = 0;
    int bad = 0;
    for (uint64_t pos = 0; pos < ie->size && !bad;) {
        size_t want = ie->size - pos < CHUNK ? (size_t)(ie->size - pos) : CHUNK;
        if (pread_full(zfd, buf, want, (off_t)(ie->data + pos)) || write_full(dfd, buf, want)) bad = 1;
        else { crc = crc32_update(crc, buf, want); pos += want; }
    }
    free(buf);
    if (bad) {
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    if (crc != ie->crc) {
        snprintf(err, errsz, "%s", T("Prüfsumme des Spielstands stimmt nicht – die ZIP-Datei ist beschädigt"));
        goto done;
    }
    fchmod(dfd, mode);
    if (own_u != (uid_t)-1) (void)!fchown(dfd, own_u, own_g);
    if (fsync(dfd) != 0 || close(dfd) != 0) {
        dfd = -1;
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    dfd = -1;
    if (rename(tmp, dest) != 0) {
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    tmp[0] = 0;
    rc = 0;
done:
    if (dfd >= 0) close(dfd);
    if (tmp[0]) unlink(tmp);
    if (zfd >= 0) close(zfd);
    op_leave();
    return rc;
}
