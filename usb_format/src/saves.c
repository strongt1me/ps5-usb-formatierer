/*
 * PS5-Spielstand-Sicherung, Wiederherstellung und Neusignierung.
 * Sichern: das verschlüsselte Container-Image + metadata.json als ZIP (nichts wird entschlüsselt).
 * Neusignieren: Container über sceFsMountSaveData einhängen (Schlüssel über /dev/pfsmgr), Account-ID und Benutzer-ID
 * in sce_sys/param.sfo ändern. Ablauf und Offsets nach garlic-savemgr (GPL-3.0).
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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
#include "sqlite3.h"

#define SAVE_SUBDIR     "savedata_prospero"
#define SEALED_KEY_OFF  0x800   /* Sealed Key im PS5-Container */
#define SEALED_KEY_LEN  0x60
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

/* ── Benutzer und Account-IDs ────────────────────────────────────────── */

typedef int (*fn_regstr_t)(int key, char *buf, size_t sz);
typedef int (*fn_regbin_t)(int key, void *buf, size_t sz);

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

/* Account-ID des Benutzers aus der Registry: Konto mit gleichem Namen (wie garlic-savemgr). 0 = gefunden */
static int account_id_of(const char *username, char *out, size_t outsz) {
    static fn_regstr_t get_str;   /* einmal suchen: die Auflistung fragt für jeden Benutzer */
    static fn_regbin_t get_bin;
    if (!get_str || !get_bin) {
        get_str = (fn_regstr_t)plat_dynsym("libSceRegMgr.sprx", "sceRegMgrGetStr");
        get_bin = (fn_regbin_t)plat_dynsym("libSceRegMgr.sprx", "sceRegMgrGetBin");
    }
    if (!get_str || !get_bin || !username[0]) return -1;
    for (int slot = 1; slot <= 16; slot++) {
        char reg_name[64] = "";
        if (get_str((slot - 1) * 65536 + 125829632, reg_name, sizeof(reg_name)) < 0 || !reg_name[0]) continue;
        if (strcmp(reg_name, username) != 0) continue;
        uint64_t aid = 0;
        if (get_bin((slot - 1) * 65536 + 125830400, &aid, sizeof(aid)) < 0 || !aid) return -1;
        snprintf(out, outsz, "%016llx", (unsigned long long)aid);
        return 0;
    }
    return -1;
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
        if (account_id_of(name, u->account_id, sizeof(u->account_id)) < 0) u->account_id[0] = 0;
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

/* ── param.sfo ändern ────────────────────────────────────────────────── */

/* Wert-Position und -Länge eines Schlüssels */
static int sfo_find(int fd, const char *want, off_t *off, uint32_t *len) {
    uint8_t h[20];
    if (pread_full(fd, h, sizeof(h), 0) || memcmp(h, "\0PSF", 4) != 0) return -1;
    uint32_t kt = rd32(h + 8), dt = rd32(h + 12), cnt = rd32(h + 16);
    if (cnt > 256 || kt < 20 || dt < kt) return -1;
    for (uint32_t i = 0; i < cnt; i++) {
        uint8_t e[16];
        char key[32] = "";
        if (pread_full(fd, e, sizeof(e), 20 + (off_t)i * 16)) return -1;
        ssize_t r = pread(fd, key, sizeof(key) - 1, (off_t)kt + rd16(e));
        if (r <= 0 || strcmp(key, want) != 0) continue;
        *off = (off_t)dt + rd32(e + 12);
        *len = rd32(e + 4);
        return 0;
    }
    return -1;
}

/* ── param.sfo lesen und ändern ──────────────────────────────────────── */

typedef struct {
    char     title_id[16], dir[64], main_title[128], sub_title[128], detail[256], game_title_id[16];
    uint64_t blocks, sys_blocks;
    uint32_t user_param;
    uint8_t  hmac[32];   /* PARAMS +8: bindet den Spielstand an die Konsole */
} sfo_meta_t;

static int sfo_str(int fd, const char *key, char *out, size_t n) {
    off_t o;
    uint32_t l;
    out[0] = 0;
    if (sfo_find(fd, key, &o, &l)) return -1;
    if (l >= n) l = (uint32_t)n - 1;
    ssize_t r = l ? pread(fd, out, l, o) : 0;
    if (r < 0) return -1;
    out[r] = 0;
    return 0;
}

static uint64_t sfo_num(int fd, const char *key, uint32_t bytes) {
    off_t o;
    uint32_t l;
    uint8_t b[8];
    uint64_t v = 0;
    if (sfo_find(fd, key, &o, &l) || l < bytes || pread_full(fd, b, bytes, o)) return 0;
    for (uint32_t i = 0; i < bytes; i++) v |= (uint64_t)b[i] << (8 * i);
    return v;
}

static int sfo_read_meta(const char *path, sfo_meta_t *m) {
    memset(m, 0, sizeof(*m));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    off_t po;
    uint32_t pl;
    int ok = sfo_find(fd, "ACCOUNT_ID", &po, &pl) == 0;
    sfo_str(fd, "TITLE_ID", m->title_id, sizeof(m->title_id));
    sfo_str(fd, "SAVEDATA_DIRECTORY", m->dir, sizeof(m->dir));
    if (sfo_str(fd, "MAINTITLE", m->main_title, sizeof(m->main_title)) || !m->main_title[0])
        sfo_str(fd, "TITLE", m->main_title, sizeof(m->main_title));
    sfo_str(fd, "SUBTITLE", m->sub_title, sizeof(m->sub_title));
    sfo_str(fd, "DETAIL", m->detail, sizeof(m->detail));
    m->blocks = sfo_num(fd, "SAVEDATA_BLOCKS", 8);
    m->sys_blocks = sfo_num(fd, "SYSTEM_BLOCKS", 8);
    m->user_param = (uint32_t)sfo_num(fd, "SAVEDATA_LIST_PARAM", 4);
    if (sfo_find(fd, "PARAMS", &po, &pl) == 0 && pl >= 0x28) {
        pread_full(fd, m->hmac, sizeof(m->hmac), po + 8);
        if (pl >= 0x44) {   /* +0x38: Titel-ID des Spiels */
            char g[13] = "";
            if (pread_full(fd, g, 12, po + 0x38) == 0) scpy(m->game_title_id, sizeof(m->game_title_id), g);
        }
    }
    close(fd);
    if (!m->game_title_id[0]) scpy(m->game_title_id, sizeof(m->game_title_id), m->title_id);
    return ok ? 0 : -1;
}

static int is_zero(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (b[i]) return 0;
    return 1;
}

int saves_sfo_patch(const char *path, const uint8_t aid[8], uint32_t user_id, const uint8_t *hmac, char *err,
                    size_t errsz) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        snprintf(err, errsz, "%s", T("param.sfo des Spielstands konnte nicht geöffnet werden"));
        return -1;
    }
    off_t ao = 0, uo = 0;
    uint32_t al = 0, ul = 0;
    uint8_t uid4[4], chk[32];
    wr32(uid4, user_id);
    int ok = sfo_find(fd, "ACCOUNT_ID", &ao, &al) == 0 && al >= 8;
    if (ok) {
        ok = pwrite(fd, aid, 8, ao) == 8 && fsync(fd) == 0 && pread_full(fd, chk, 8, ao) == 0 && !memcmp(chk, aid, 8);
        /* die Benutzer-ID steht in PARAMS bei +4 (kein eigener Schlüssel; garlic-savemgr: SFO-Offset 0x660) */
        if (ok && sfo_find(fd, "PARAMS", &uo, &ul) == 0 && ul >= 8) {
            ok = pwrite(fd, uid4, 4, uo + 4) == 4 && fsync(fd) == 0 && pread_full(fd, chk, 4, uo + 4) == 0 &&
                 !memcmp(chk, uid4, 4);
            if (ok && hmac && ul >= 0x28)
                ok = pwrite(fd, hmac, 32, uo + 8) == 32 && fsync(fd) == 0 && pread_full(fd, chk, 32, uo + 8) == 0 &&
                     !memcmp(chk, hmac, 32);
        }
    }
    close(fd);
    if (!ok) snprintf(err, errsz, "%s", T("param.sfo konnte nicht geändert werden (ACCOUNT_ID nicht gefunden)"));
    return ok ? 0 : -1;
}

/* ── Neusignieren ────────────────────────────────────────────────────── */

typedef struct { uint8_t reserved; char *budgetid; } MountOpt;
typedef struct { uint8_t dummy; } UmountOpt;
typedef int (*fn_mount_init_t)(MountOpt *);
typedef int (*fn_mount_t)(MountOpt *, const char *, const char *, uint8_t *);
typedef int (*fn_umount_init_t)(UmountOpt *);
typedef int (*fn_umount_t)(UmountOpt *, const char *, int, int);
typedef struct { fn_mount_init_t mi; fn_mount_t m; fn_umount_init_t ui; fn_umount_t u; } fsapi_t;

/* Datei src nach dst kopieren (0 = ok); dst wird neu angelegt */
static int copy_file(const char *src, const char *dst, mode_t mode, uid_t u, gid_t g) {
    int s = open(src, O_RDONLY), d = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    uint8_t *buf = malloc(CHUNK);
    int rc = -1;
    if (s >= 0 && d >= 0 && buf) {
        ssize_t r;
        rc = 0;
        while ((r = read(s, buf, CHUNK)) > 0) {
            if (write_full(d, buf, (size_t)r)) { rc = -1; break; }
        }
        if (r < 0) rc = -1;
        fchmod(d, mode);
        if (u != (uid_t)-1) (void)!fchown(d, u, g);
        if (fsync(d) != 0) rc = -1;
    }
    if (s >= 0) close(s);
    if (d >= 0 && close(d) != 0) rc = -1;
    free(buf);
    return rc;
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

/* Container src in den Arbeitsordner kopieren (Einhängen direkt aus savedata_prospero endet mit EPIPE) und unter mnt
 * einhängen. 0 = ok; die Arbeitskopie work löscht der Aufrufer. */
static int cont_mount(const fsapi_t *f, const char *src, const char *work, const char *mnt, char *err, size_t errsz) {
    uint8_t sealed[SEALED_KEY_LEN], key[0x20];
    if (copy_file(src, work, 0777, (uid_t)-1, (gid_t)-1)) {
        snprintf(err, errsz, "%s", T("Arbeitskopie konnte nicht erstellt werden"));
        return -1;
    }
    int fd = open(work, O_RDONLY);
    if (fd < 0 || pread_full(fd, sealed, sizeof(sealed), SEALED_KEY_OFF)) {
        if (fd >= 0) close(fd);
        snprintf(err, errsz, "%s", T("Sealed Key konnte nicht gelesen werden"));
        return -1;
    }
    close(fd);
    if (plat_pfs_decrypt(sealed, key) != 0) {
        snprintf(err, errsz, "%s", T("Schlüssel konnte nicht entschlüsselt werden (pfsmgr)"));
        return -1;
    }
    MountOpt mopt;
    memset(&mopt, 0, sizeof(mopt));
    f->mi(&mopt);
    char budget[] = "system";
    mopt.budgetid = budget;
    struct sigaction dfl, old;   /* das Einhängen braucht SIGPIPE in Standardbehandlung (wie garlic-savemgr) */
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &dfl, &old);
    int mr = f->m(&mopt, work, mnt, key);
    sigaction(SIGPIPE, &old, NULL);
    memset(key, 0, sizeof(key));
    if (mr < 0) {
        snprintf(err, errsz, T("Einhängen fehlgeschlagen (0x%08x)"), (unsigned)mr);
        return -1;
    }
    return 0;
}

static void cont_umount(const fsapi_t *f, const char *mnt) {
    UmountOpt uo;
    memset(&uo, 0, sizeof(uo));
    f->ui(&uo);
    f->u(&uo, mnt, 0, 0);   /* verschlüsselt den Container wieder */
    sync();
}

/* PARAMS-HMAC eines anderen Spielstands dieser Konsole (bevorzugt vom Zielbenutzer): wie garlic-savemgr, das den
 * HMAC aus einem vorhandenen Spielstand übernimmt, statt ihn zu berechnen (Schlüssel der PS5 unbekannt) */
static int donor_hmac(const fsapi_t *f, const save_info_t *skip, const char *target_uid, uint8_t out[32],
                      const char *work, const char *mnt) {
    save_info_t *all = NULL;
    int n = 0, tried = 0, rc = -1;
    if (saves_scan(&all, &n) < 0) return -1;
    for (int pass = 0; pass < 2 && rc; pass++) {
        for (int i = 0; i < n && tried < 3 && rc; i++) {
            int same = !strcmp(all[i].uid, target_uid);
            if (!strcmp(all[i].key, skip->key) || (pass == 0 ? !same : same) || all[i].size > (64ull << 20)) continue;
            char e[200], sfo[500];
            sfo_meta_t m;
            if (cont_mount(f, all[i].path, work, mnt, e, sizeof(e)) == 0) {
                tried++;
                if (!path_fmt(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", mnt) && sfo_read_meta(sfo, &m) == 0 &&
                    !is_zero(m.hmac, 32)) {
                    memcpy(out, m.hmac, 32);
                    rc = 0;
                }
                cont_umount(f, mnt);
            }
            unlink(work);
        }
    }
    free(all);
    return rc;
}

/* Eintrag in der Spielstand-Datenbank des Benutzers (Einhängen/Anzeige der PS5 liest sie): vorhandene Zeile ändern,
 * sonst anlegen. Vorher Sicherungskopie der Datenbank (…usbfmt.bak). 0 = ok, 1 = Datenbank nicht vorhanden (übersprungen) */
static int db_update(const char *dbfile, const sfo_meta_t *m, uint64_t aid, uint32_t uid, int create, char *err,
                     size_t errsz) {
    struct stat st;
    int exists = stat(dbfile, &st) == 0;
    if (!exists && !create) return 1;
    char dir[1300], bak[1400];
    scpy(dir, sizeof(dir), dbfile);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdirs(dir); }
    if (exists && (path_fmt(bak, sizeof(bak), "%s.usbfmt.bak", dbfile) || copy_file(dbfile, bak, 0644, st.st_uid, st.st_gid))) {
        snprintf(err, errsz, "%s", T("Sicherungskopie der Spielstand-Datenbank fehlgeschlagen"));
        return -1;
    }
    sqlite3 *db = NULL;
    int rc = -1;
    sqlite3_stmt *q = NULL;
    char mtime[40];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(mtime, sizeof(mtime), "%Y-%m-%dT%H:%M:%S.00Z", &tmv);
    if (sqlite3_open(dbfile, &db) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(db, 8000);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS savedata (id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
            "title_id NOT NULL, dir_name NOT NULL, main_title NOT NULL, sub_title, detail, tmp_dir_name, is_broken,"
            "user_param, blocks, free_blocks, size_kib, mtime NOT NULL, fake_broken, account_id, user_id, faked_owner,"
            "cloud_icon_url, cloud_revision, game_title_id NOT NULL, system_blocks)", NULL, NULL, NULL) != SQLITE_OK)
        goto fail_tx;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM savedata WHERE title_id=?1 AND dir_name=?2", -1, &q, NULL) != SQLITE_OK)
        goto fail_tx;
    sqlite3_bind_text(q, 1, m->title_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, m->dir, -1, SQLITE_STATIC);
    int have = sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0) > 0;
    sqlite3_finalize(q);
    q = NULL;
    if (!have && !create) {   /* nur vorhandene Zeilen ändern, bei einem Neusignieren am Ort keine neue anlegen */
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        rc = 1;
        goto out;
    }
    const char *sql = have
        ? "UPDATE savedata SET main_title=?3, sub_title=?4, detail=?5, user_param=?6, blocks=?7, free_blocks=?7,"
          "size_kib=?8, mtime=?9, account_id=?10, user_id=?11 WHERE title_id=?1 AND dir_name=?2"
        : "INSERT INTO savedata (title_id, dir_name, main_title, sub_title, detail, is_broken, user_param, blocks,"
          "free_blocks, size_kib, mtime, fake_broken, account_id, user_id, faked_owner, cloud_revision, game_title_id,"
          "system_blocks) VALUES (?1,?2,?3,?4,?5,0,?6,?7,?7,?8,?9,0,?10,?11,0,0,?12,?13)";
    if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK) goto fail_tx;
    sqlite3_bind_text(q, 1, m->title_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, m->dir, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 3, m->main_title, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 4, m->sub_title, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 5, m->detail, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 6, (sqlite3_int64)m->user_param);
    sqlite3_bind_int64(q, 7, (sqlite3_int64)m->blocks);
    sqlite3_bind_int64(q, 8, (sqlite3_int64)(m->blocks * 0x40));
    sqlite3_bind_text(q, 9, mtime, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 10, (sqlite3_int64)aid);
    sqlite3_bind_int64(q, 11, (sqlite3_int64)uid);
    if (!have) {
        sqlite3_bind_text(q, 12, m->game_title_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(q, 13, (sqlite3_int64)m->sys_blocks);
    }
    if (sqlite3_step(q) != SQLITE_DONE) goto fail_tx;
    sqlite3_finalize(q);
    q = NULL;
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto fail_tx;
    rc = 0;
    goto out;
fail_tx:
    if (q) sqlite3_finalize(q);
    q = NULL;
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
fail:
    snprintf(err, errsz, T("Die Spielstand-Datenbank des Zielbenutzers konnte nicht aktualisiert werden: %s"),
             db ? sqlite3_errmsg(db) : "open");
out:
    if (db) sqlite3_close(db);
    return rc;
}

int saves_resign(const save_info_t *s, const char *target_uid, char *err, size_t errsz) {
    const char *home = plat_saves_dir();
    char udir[700], username[64] = "", aid_hex[17] = "";
    struct stat st;
    if (!saves_name_ok(target_uid, sizeof(s->uid)) || !home || path_fmt(udir, sizeof(udir), "%s/%s", home, target_uid) ||
        stat(udir, &st) != 0 || !S_ISDIR(st.st_mode) || read_username(home, target_uid, username, sizeof(username))) {
        snprintf(err, errsz, "%s", T("Der Zielbenutzer ist auf dieser PS5 nicht vorhanden"));
        return -1;
    }
    if (account_id_of(username, aid_hex, sizeof(aid_hex)) < 0) {
        snprintf(err, errsz, T("Für den Benutzer %s wurde keine Account-ID in der Registry gefunden"), username);
        return -1;
    }
    uint8_t aid[8];
    uint64_t av = strtoull(aid_hex, NULL, 16);
    for (int i = 0; i < 8; i++) aid[i] = (uint8_t)(av >> (8 * i));   /* little-endian wie in der param.sfo */
    uint32_t user_id = (uint32_t)strtoul(target_uid, NULL, 16);
    const int copy = strcmp(target_uid, s->uid) != 0;   /* anderer Benutzer: Kopie bei ihm, das Original bleibt */

    fsapi_t f;
    f.mi = (fn_mount_init_t)plat_dynsym("libSceFsInternalForVsh.sprx", "sceFsInitMountSaveDataOpt");
    f.m = (fn_mount_t)plat_dynsym("libSceFsInternalForVsh.sprx", "sceFsMountSaveData");
    f.ui = (fn_umount_init_t)plat_dynsym("libSceFsInternalForVsh.sprx", "sceFsInitUmountSaveDataOpt");
    f.u = (fn_umount_t)plat_dynsym("libSceFsInternalForVsh.sprx", "sceFsUmountSaveData");
    if (!f.mi || !f.m || !f.ui || !f.u) {
        snprintf(err, errsz, "%s", T("libSceFsInternalForVsh.sprx nicht gefunden"));
        return -1;
    }

    /* Ziel bei einer Kopie: <home>/<ziel>/savedata_prospero/<Titel>/<Name>; ein vorhandener Spielstand wird nie überschrieben */
    char tdir[1000] = "", dest[1300] = "", final_path[600];
    scpy(final_path, sizeof(final_path), s->path);
    if (copy) {
        if (path_fmt(tdir, sizeof(tdir), "%s/%s/" SAVE_SUBDIR "/%s", home, target_uid, s->title_id) ||
            path_fmt(dest, sizeof(dest), "%s/%s", tdir, s->save_dir)) {
            snprintf(err, errsz, "%s", T("Pfad zu lang"));
            return -1;
        }
        if (stat(dest, &st) == 0) {
            snprintf(err, errsz, "%s", T("Beim Zielbenutzer existiert bereits ein Spielstand gleichen Namens"));
            return -1;
        }
    }
    if (op_enter(err, errsz)) return -1;

    int rc = -1, mounted = 0, elevated = 0, wrote = 0, have_icon_dest = 0;
    plat_creds_t creds;
    char work[700], back[1300], mnt[400], sfo[500], icon_dest[1400] = "";
    uint8_t *icon = NULL, donor[32];
    size_t icon_len = 0;
    int have_donor = 0;
    work[0] = back[0] = 0;
    if (stat(s->path, &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size < IMG_MIN || (uint64_t)st.st_size > IMG_MAX) {
        snprintf(err, errsz, "%s", T("Der Spielstand hat eine ungültige Größe"));
        goto done;
    }
    mode_t mode = st.st_mode & 0777;
    uid_t own_u = st.st_uid;
    gid_t own_g = st.st_gid;
    uint64_t size = (uint64_t)st.st_size;
    {
        int fd = open(s->path, O_RDONLY);
        uint8_t first = 0;
        int bad = fd < 0 || pread_full(fd, &first, 1, 0);
        if (fd >= 0) close(fd);
        if (bad) {
            snprintf(err, errsz, T("Spielstand konnte nicht geöffnet werden: %s"), strerror(errno));
            goto done;
        }
        if (first == 0x01) {
            snprintf(err, errsz, "%s", T("PS4-Spielstände werden noch nicht unterstützt"));
            goto done;
        }
    }
    if (!space_ok(saves_workdir(), size) || (copy && !space_ok(udir, size))) {
        snprintf(err, errsz, "%s", T("Nicht genug freier Speicher auf der PS5"));
        goto done;
    }
    if (path_fmt(work, sizeof(work), "%s/resign_%s_%s", saves_workdir(), s->title_id, s->save_dir) ||
        path_fmt(mnt, sizeof(mnt), "%s/mnt", saves_workdir())) {
        snprintf(err, errsz, "%s", T("Pfad zu lang"));
        goto done;
    }
    mkdir(mnt, 0755);

    plat_creds_elevate(&creds);
    elevated = 1;

    if (copy) have_donor = donor_hmac(&f, s, target_uid, donor, work, mnt) == 0;

    if (cont_mount(&f, s->path, work, mnt, err, errsz)) goto done;
    mounted = 1;

    sfo_meta_t meta;
    int pr = path_fmt(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", mnt);
    char e2[256] = "";
    if (!pr) {
        sfo_read_meta(sfo, &meta);
        pr = saves_sfo_patch(sfo, aid, user_id, have_donor && memcmp(donor, meta.hmac, 32) ? donor : NULL, e2, sizeof(e2));
    }
    if (!pr && copy) {   /* Symbolbild für die Anzeige beim Zielbenutzer */
        char ip[500];
        int ifd = path_fmt(ip, sizeof(ip), "%s/sce_sys/icon0.png", mnt) ? -1 : open(ip, O_RDONLY);
        struct stat is;
        if (ifd >= 0 && fstat(ifd, &is) == 0 && is.st_size > 0 && is.st_size <= (8 << 20) && (icon = malloc((size_t)is.st_size)) &&
            pread_full(ifd, icon, (size_t)is.st_size, 0) == 0)
            icon_len = (size_t)is.st_size;
        else { free(icon); icon = NULL; }
        if (ifd >= 0) close(ifd);
    }
    cont_umount(&f, mnt);
    mounted = 0;
    if (pr) {
        snprintf(err, errsz, "%s", e2[0] ? e2 : T("param.sfo konnte nicht geändert werden (ACCOUNT_ID nicht gefunden)"));
        goto done;
    }

    /* neben das Ziel schreiben und umbenennen: bricht etwas ab, bleibt das Vorhandene unverändert; der Name beginnt mit
     * ".", damit die Liste ihn nicht zeigt */
    const char *finalp = copy ? dest : final_path;
    {
        const char *sl = strrchr(finalp, '/');
        if (copy) { mkdirs(tdir); }
        if (!sl || path_fmt(back, sizeof(back), "%.*s/.%s.resign.tmp", (int)(sl - finalp), finalp, sl + 1)) {
            snprintf(err, errsz, "%s", T("Pfad zu lang"));
            back[0] = 0;
            goto done;
        }
    }
    if (copy_file(work, back, mode, own_u, own_g)) {
        snprintf(err, errsz, "%s", T("Zurückkopieren fehlgeschlagen"));
        goto done;
    }
    if (rename(back, finalp) != 0) {
        snprintf(err, errsz, T("Schreibfehler: %s"), strerror(errno));
        goto done;
    }
    back[0] = 0;
    wrote = 1;

    /* Symbolbild: <ziel>/savedata_prospero_meta/user/<Titel>/<SAVEDATA_DIRECTORY>_icon0.png (Fehler hier sind unkritisch) */
    if (copy && icon) {
        char md[1100];
        const char *dn = meta.dir[0] ? meta.dir : (!strncmp(s->save_dir, "sdimg_", 6) ? s->save_dir + 6 : s->save_dir);
        if (!path_fmt(md, sizeof(md), "%s/%s/savedata_prospero_meta/user/%s", home, target_uid, s->title_id) &&
            !path_fmt(icon_dest, sizeof(icon_dest), "%s/%s_icon0.png", md, dn)) {
            mkdirs(md);
            int ifd = open(icon_dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (ifd >= 0) {
                have_icon_dest = write_full(ifd, icon, icon_len) == 0;
                close(ifd);
                if (!have_icon_dest) unlink(icon_dest);
            }
        }
    }

    /* Spielstand-Datenbank des Zielbenutzers (PS5: /system_data/savedata_prospero/<uid>/db/user/savedata.db) */
    {
        const char *dbroot = plat_savedb_dir();
        char dbf[1300], e3[300] = "";
        if (dbroot && !path_fmt(dbf, sizeof(dbf), "%s/%s/db/user/savedata.db", dbroot, target_uid)) {
            if (!meta.title_id[0]) scpy(meta.title_id, sizeof(meta.title_id), s->title_id);
            if (!meta.dir[0]) scpy(meta.dir, sizeof(meta.dir), !strncmp(s->save_dir, "sdimg_", 6) ? s->save_dir + 6 : s->save_dir);
            int dr = db_update(dbf, &meta, av, user_id, copy, e3, sizeof(e3));
            if (dr < 0 && copy) {   /* ohne Eintrag zeigt die PS5 die Kopie nicht: zurücknehmen */
                unlink(dest);
                if (have_icon_dest) unlink(icon_dest);
                wrote = 0;
                snprintf(err, errsz, "%s", e3);
                goto done;
            }
        }
    }
    rc = copy ? 1 : 0;
done:
    if (mounted) cont_umount(&f, mnt);
    if (elevated) plat_creds_restore(&creds);
    if (back[0]) unlink(back);
    if (work[0]) unlink(work);
    (void)wrote;
    free(icon);
    op_leave();
    return rc;
}
