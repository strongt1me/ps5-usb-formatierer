/* Spiele auf einem Laufwerk finden (siehe library.h). Nur lesend. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fsread.h"
#include "library.h"

#define PARAM_MAX (256 * 1024)

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ------------------------------------------------------------ kleines JSON-Lesen für param.json */

static const char *jws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static unsigned hex4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v << 4 | (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

static size_t put_utf8(char *out, size_t n, size_t o, unsigned c) {
    char t[4];
    int k = 0;
    if (c < 0x80) t[k++] = (char)c;
    else if (c < 0x800) { t[k++] = (char)(0xC0 | c >> 6); t[k++] = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { t[k++] = (char)(0xE0 | c >> 12); t[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
                            t[k++] = (char)(0x80 | (c & 0x3F)); }
    else { t[k++] = (char)(0xF0 | c >> 18); t[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
           t[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); t[k++] = (char)(0x80 | (c & 0x3F)); }
    if (out && o + (size_t)k < n) { memcpy(out + o, t, (size_t)k); o += (size_t)k; }
    return o;
}

/* String ab p ('"') nach out (UTF-8); Rückgabe hinter das schließende '"' oder NULL */
static const char *jstr(const char *p, const char *e, char *out, size_t n) {
    size_t o = 0;
    if (p >= e || *p != '"') return NULL;
    for (p++; p < e && *p != '"'; ) {
        unsigned c = (unsigned char)*p++;
        if (c == '\\') {
            if (p >= e) return NULL;
            char x = *p++;
            if (x == 'u') {
                if (e - p < 4) return NULL;
                unsigned u = hex4(p);
                p += 4;
                if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = hex4(p + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                o = put_utf8(out, n, o, u);
                continue;
            }
            c = x == 'n' ? '\n' : x == 't' ? '\t' : x == 'r' ? '\r' : x == 'b' ? '\b' : x == 'f' ? '\f'
                                                                                         : (unsigned char)x;
        }
        if (out && o + 1 < n) out[o++] = (char)c;
    }
    if (p >= e) return NULL;
    if (out && n) out[o] = 0;
    return p + 1;
}

static const char *jskip(const char *p, const char *e) {
    p = jws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') return jstr(p, e, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') { if (!(p = jstr(p, e, NULL, 0))) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
            p++;
        }
        return NULL;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* Wert zum Schlüssel key im Objekt ab p ('{'); NULL wenn nicht vorhanden */
static const char *jget(const char *p, const char *e, const char *key) {
    p = jws(p, e);
    if (p >= e || *p != '{') return NULL;
    for (p++;;) {
        char k[64];
        p = jws(p, e);
        if (p >= e || *p == '}' || !(p = jstr(p, e, k, sizeof(k)))) return NULL;
        p = jws(p, e);
        if (p >= e || *p != ':') return NULL;
        p = jws(p + 1, e);
        if (!strcmp(k, key)) return p;
        if (!(p = jskip(p, e))) return NULL;
        p = jws(p, e);
        if (p >= e || *p != ',') return NULL;
        p++;
    }
}

static int jget_str(const char *obj, const char *e, const char *key, char *out, size_t n) {
    const char *v = obj ? jget(obj, e, key) : NULL;
    return v && v < e && *v == '"' && jstr(v, e, out, n) ? 0 : -1;
}

int lib_json_str(const char *json, size_t len, const char *key, char *out, size_t n) {
    return jget_str(json, json + len, key, out, n);
}

int lib_json_raw(const char *json, size_t len, const char *key, const char **val, size_t *vlen) {
    const char *e = json + len, *p = jget(json, e, key), *q = p ? jskip(p, e) : NULL;
    if (!q) return -1;
    *val = p;
    *vlen = (size_t)(q - p);
    return 0;
}

int lib_json_num(const char *json, size_t len, const char *key, long long *v) {
    const char *e = json + len, *p = jget(json, e, key);
    if (!p || p >= e || !(*p == '-' || (*p >= '0' && *p <= '9'))) return -1;
    *v = strtoll(p, NULL, 10);
    return 0;
}

/* UTF-8 nicht mitten in einem Zeichen abschneiden */
static void trim_utf8(char *s) {
    size_t n = strlen(s), i = n;
    while (i && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i) {
        unsigned char lead = (unsigned char)s[i - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - (i - 1) < need) s[i - 1] = 0;
    }
}

/* Titel-ID wie PPSA01234 / CUSA00001: 4 Großbuchstaben + 5 Ziffern. Sie stammt von beliebigen Sticks und geht an
 * ShadowMountPlus weiter (JSON) - alles andere wird verworfen. */
static int title_id_ok(const char *s, size_t n) {
    if (n != 9) return 0;
    for (size_t i = 0; i < 9; i++)
        if (i < 4 ? !(s[i] >= 'A' && s[i] <= 'Z') : !(s[i] >= '0' && s[i] <= '9')) return 0;
    return 1;
}

static void keep_valid_id(char *id) { if (!title_id_ok(id, strlen(id))) id[0] = 0; }

int lib_parse_param_json(const char *json, size_t len, char *id, size_t idn, char *name, size_t namen) {
    const char *e = json + len, *lp;
    id[0] = name[0] = 0;
    if (jget_str(json, e, "titleId", id, idn) != 0 || !id[0]) {
        char cid[64];   /* contentId "UP9000-PPSA01234_00-..." enthält die Titel-ID */
        if (jget_str(json, e, "contentId", cid, sizeof(cid)) == 0 && strlen(cid) >= 16)
            snprintf(id, idn, "%.9s", cid + 7);
    }
    if ((lp = jget(json, e, "localizedParameters")) && *lp == '{') {
        char def[16] = "";
        jget_str(lp, e, "defaultLanguage", def, sizeof(def));
        const char *langs[] = {"de-DE", def, "en-US", "en-GB"};
        for (int i = 0; i < 4 && !name[0]; i++) {
            const char *v = langs[i][0] ? jget(lp, e, langs[i]) : NULL;
            if (v && *v == '{') jget_str(v, e, "titleName", name, namen);
        }
    }
    trim_utf8(name);
    keep_valid_id(id);
    return id[0] ? 0 : -1;
}

int lib_parse_param_sfo(const uint8_t *b, size_t len, char *id, size_t idn, char *name, size_t namen) {
    id[0] = name[0] = 0;
    if (len < 20 || memcmp(b, "\0PSF", 4) != 0) return -1;
    uint32_t kt = le32(b + 8), dt = le32(b + 12), n = le32(b + 16);
    for (uint32_t i = 0; i < n && 20 + 16 * (size_t)(i + 1) <= len; i++) {
        const uint8_t *ent = b + 20 + 16 * i;
        size_t ko = (size_t)kt + le16(ent), vo = (size_t)dt + le32(ent + 12), vl = le32(ent + 4);
        if (ko >= len || vo >= len || vl > len - vo) continue;
        const char *key = (const char *)b + ko;
        if (memchr(key, 0, len - ko) == NULL) continue;
        char *dst = !strcmp(key, "TITLE_ID") ? id : !strcmp(key, "TITLE") ? name : NULL;
        size_t dn = dst == id ? idn : namen;
        if (dst) snprintf(dst, dn, "%.*s", (int)strnlen((const char *)b + vo, vl), (const char *)b + vo);
    }
    trim_utf8(name);
    keep_valid_id(id);
    return id[0] ? 0 : -1;
}

/* ------------------------------------------------------------ Hinweise von ShadowMountPlus */

static lib_hint_t *hints;
static int nhints;

void lib_set_hints(const lib_hint_t *h, int n) {
    free(hints);
    hints = NULL;
    nhints = 0;
    if (!h || n <= 0 || !(hints = malloc((size_t)n * sizeof(*hints)))) return;
    memcpy(hints, h, (size_t)n * sizeof(*hints));
    nhints = n;
}

static lib_hint_t *hint_add(lib_hint_t **h, int *n) {
    lib_hint_t *p = realloc(*h, (size_t)(*n + 1) * sizeof(**h));
    if (!p) return NULL;
    *h = p;
    memset(&p[*n], 0, sizeof(**h));
    return &p[(*n)++];
}

static const lib_hint_t *hint_by_path(const char *path) {
    for (int i = nhints - 1; i >= 0; i--)   /* jüngster Eintrag gewinnt */
        if (!strcmp(hints[i].path, path)) return &hints[i];
    return NULL;
}

static const char *hint_name(const char *id) {
    for (int i = 0; i < nhints; i++)
        if (hints[i].name[0] && !strcmp(hints[i].id, id)) return hints[i].name;
    return NULL;
}

int lib_parse_smp_games(const char *json, size_t len, lib_hint_t **h, int *n) {
    const char *e = json + len, *p = jget(json, e, "games");
    const int n0 = *n;
    if (!p || *p != '[') return -1;
    for (p = jws(p + 1, e); p < e && *p == '{'; ) {
        const char *next = jskip(p, e);
        if (!next) { *n = n0; return -1; }   /* abgeschnitten: keine halbe Liste */
        lib_hint_t t;
        memset(&t, 0, sizeof(t));
        jget_str(p, next, "path", t.path, sizeof(t.path));
        jget_str(p, next, "title_id", t.id, sizeof(t.id));
        jget_str(p, next, "title_name", t.name, sizeof(t.name));
        trim_utf8(t.name);
        keep_valid_id(t.id);
        lib_hint_t *dst;
        if (t.path[0] && t.id[0] && (dst = hint_add(h, n))) *dst = t;
        p = jws(next, e);
        if (p < e && *p == ',') p = jws(p + 1, e);
    }
    if (p >= e || *p != ']') { *n = n0; return -1; }   /* Liste nicht abgeschlossen: unvollständig */
    return 0;
}

int lib_smp_mount(const char *json, size_t len, const char *mount_point, char *source, size_t sn, char *mount_id,
                  size_t mn) {
    const char *e = json + len, *p = jget(json, e, "mounts");
    int found = 0;
    if (!p || *p != '[') return -1;
    for (p = jws(p + 1, e); p < e && *p == '{'; ) {
        const char *next = jskip(p, e), *ej;
        if (!next) return -1;
        char mp[256] = "";
        if (jget_str(p, next, "mount_point", mp, sizeof(mp)) == 0 && !strcmp(mp, mount_point)) {
            found = 1;
            ej = jget(p, next, "ejectable");
            if (ej && next - ej >= 4 && !memcmp(ej, "true", 4) && jget_str(p, next, "mount_id", mount_id, mn) == 0 &&
                mount_id[0] && jget_str(p, next, "source", source, sn) == 0)
                return 0;
        }
        p = jws(next, e);
        if (p < e && *p == ',') p = jws(p + 1, e);
    }
    return found ? 1 : -1;
}

int lib_smp_title(const char *json, size_t len, const char *id, char *version, size_t vn, char *path, size_t pn,
                  int *installed) {
    const char *e = json + len, *p = jget(json, e, "games");
    if (!p || *p != '[') return -1;
    for (p = jws(p + 1, e); p < e && *p == '{'; ) {
        const char *next = jskip(p, e);
        if (!next) return -1;
        char tid[16] = "";
        jget_str(p, next, "title_id", tid, sizeof(tid));
        if (!strcmp(tid, id)) {
            version[0] = path[0] = 0;
            jget_str(p, next, "version", version, vn);
            jget_str(p, next, "path", path, pn);
            const char *v = jget(p, next, "installed");
            *installed = v && next - v >= 4 && !strncmp(v, "true", 4);
            return 1;
        }
        p = jws(next, e);
        if (p < e && *p == ',') p = jws(p + 1, e);
    }
    return p < e && *p == ']' ? 0 : -1;
}

int lib_smp_mounted(const char *json, size_t len, char *name, size_t nn) {
    const char *e = json + len, *p = jget(json, e, "games");
    if (!p || *p != '[') return -1;
    for (p = jws(p + 1, e); p < e && *p == '{'; ) {
        const char *next = jskip(p, e);
        if (!next) return -1;
        const char *v = jget(p, next, "mounted");
        if (v && next - v >= 4 && !strncmp(v, "true", 4)) {
            name[0] = 0;
            if (jget_str(p, next, "title_name", name, nn) != 0 || !name[0]) jget_str(p, next, "title_id", name, nn);
            trim_utf8(name);
            return 1;
        }
        p = jws(next, e);
        if (p < e && *p == ',') p = jws(p + 1, e);
    }
    return p < e && *p == ']' ? 0 : -1;
}

/* Zeilen wie "[LINK] mount_img.lnk created: /user/app/PPSA03117/mount_img.lnk -> /mnt/usb1/x.ffpkg layers=1" */
int lib_parse_smp_log(const char *log, size_t len, lib_hint_t **h, int *n) {
    static const char key[] = "mount_img.lnk created: /user/app/";
    const char *e = log + len;
    for (const char *p = log; p < e; ) {
        const char *eol = memchr(p, '\n', (size_t)(e - p));
        if (!eol) eol = e;
        const char *k = NULL;
        for (const char *q = p; q + sizeof(key) - 1 <= eol && !k; q++)
            if (!memcmp(q, key, sizeof(key) - 1)) k = q + sizeof(key) - 1;
        const char *slash = k ? memchr(k, '/', (size_t)(eol - k)) : NULL;
        const char *arrow = NULL;
        for (const char *q = slash; q && q + 4 <= eol && !arrow; q++)
            if (!memcmp(q, " -> ", 4)) arrow = q + 4;
        if (slash && arrow && title_id_ok(k, (size_t)(slash - k))) {
            const char *end = eol;
            for (const char *q = arrow; q + 8 <= eol; q++)
                if (!memcmp(q, " layers=", 8)) { end = q; break; }
            while (end > arrow && (end[-1] == '\r' || end[-1] == ' ')) end--;
            lib_hint_t *t;
            if (end - arrow > 0 && end - arrow < (long)sizeof(t->path) && (t = hint_add(h, n))) {
                memcpy(t->id, k, (size_t)(slash - k));
                memcpy(t->path, arrow, (size_t)(end - arrow));
            }
        }
        p = eol + 1;
    }
    return 0;
}

/* ------------------------------------------------------------ Suche */

static int ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), m = strlen(ext);
    return n > m && strcasecmp(s + n - m, ext) == 0;
}

static lib_title_t *add_title(lib_root_t *r) {
    if (r->ntitles == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        lib_title_t *t = realloc(r->titles, (size_t)cap * sizeof(*t));
        if (!t) return NULL;
        r->titles = t;
        r->cap = cap;
    }
    lib_title_t *t = &r->titles[r->ntitles++];
    memset(t, 0, sizeof(*t));
    return t;
}

static long read_file(const char *path, char *buf, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return (long)n;
}

/* Name ohne Endung als Ersatz, wenn das Spiel keinen nennt */
static void base_name(char *out, size_t n, const char *file) {
    size_t l = strnlen(file, n - 1);   /* gewollt gekürzt: der Name ist nur Ersatzanzeige */
    memcpy(out, file, l);
    out[l] = 0;
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

/* Zeichenkette kopieren; 0 = passte ganz */
static int copy_str(char *dst, size_t n, const char *src) {
    size_t len = strlen(src);
    if (len >= n) return -1;
    memcpy(dst, src, len + 1);
    return 0;
}

static int sfo_val(const uint8_t *b, size_t len, const char *key, char *out, size_t n, int is_num);

/* Version aus param.json (contentVersion) bzw. param.sfo (VERSION, sonst APP_VER) */
static void param_version(const char *buf, size_t n, int json, char *out, size_t on) {
    out[0] = 0;
    char v[32];
    if (json) {
        if (lib_json_str(buf, n, "contentVersion", v, sizeof(v)) == 0) (void)copy_str(out, on, v);
    } else if (sfo_val((const uint8_t *)buf, n, "VERSION", v, sizeof(v), 0) == 0 ||
               sfo_val((const uint8_t *)buf, n, "APP_VER", v, sizeof(v), 0) == 0) {
        (void)copy_str(out, on, v);
    }
}

static void folder_game(lib_root_t *r, const char *path, const char *dname, char *buf) {
    char p[sizeof(((lib_title_t *)0)->path) + 32], id[16], name[128], ver[16] = "";
    long n;
    int ok = -1;
    if (snprintf(p, sizeof(p), "%s/sce_sys/param.json", path) < (int)sizeof(p) &&
        (n = read_file(p, buf, PARAM_MAX)) > 0) {
        ok = lib_parse_param_json(buf, (size_t)n, id, sizeof(id), name, sizeof(name));
        if (ok == 0) param_version(buf, (size_t)n, 1, ver, sizeof(ver));
    }
    if (ok != 0 && snprintf(p, sizeof(p), "%s/sce_sys/param.sfo", path) < (int)sizeof(p) &&
        (n = read_file(p, buf, PARAM_MAX)) > 0) {
        ok = lib_parse_param_sfo((const uint8_t *)buf, (size_t)n, id, sizeof(id), name, sizeof(name));
        if (ok == 0) param_version(buf, (size_t)n, 0, ver, sizeof(ver));
    }
    if (ok != 0) return;   /* kein Spiel */
    lib_title_t *t = add_title(r);
    if (!t) return;
    copy_str(t->id, sizeof(t->id), id);
    copy_str(t->path, sizeof(t->path), path);
    copy_str(t->version, sizeof(t->version), ver);
    if (copy_str(t->name, sizeof(t->name), name[0] ? name : dname)) t->name[0] = 0;
    copy_str(t->kind, sizeof(t->kind), "Ordner");
    /* Pflichtdateien: fehlt eine, ist das meist der Rest eines abgebrochenen Kopierens oder Entpackens. Statt eboot.bin
     * genügt eboot2.bin (ShadowMountPlus ab 1.7 startet beide) */
    static const char *const need[] = {"eboot.bin", "sce_sys/icon0.png"}, *const alt[] = {"eboot2.bin", NULL};
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        struct stat sb;
        if (snprintf(p, sizeof(p), "%s/%s", path, need[i]) < (int)sizeof(p) && stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) continue;
        if (alt[i] && snprintf(p, sizeof(p), "%s/%s", path, alt[i]) < (int)sizeof(p) && stat(p, &sb) == 0 && S_ISREG(sb.st_mode)) continue;
        size_t l = strlen(t->missing);
        snprintf(t->missing + l, sizeof(t->missing) - l, "%s%s", l ? ", " : "", need[i]);
    }
}

/* Abbild-Endung (.exfat, .ffpkg, .ffpfs, .ffpfsc): 1; mit angehängtem ".off" (deaktiviert): 2; sonst 0 */
static int image_name(const char *n) {
    static const char *const ext[] = {".exfat", ".ffpkg", ".ffpfs", ".ffpfsc"};
    char act[300];
    const size_t l = strlen(n);
    const int off = l > 4 && !strcmp(n + l - 4, ".off");
    copy_str(act, sizeof(act), n);
    if (off) act[l - 4] = 0;
    for (int i = 0; i < 4; i++)
        if (ends_with(act, ext[i])) return off ? 2 : 1;
    return 0;
}

static void image_game(lib_root_t *r, const char *path, const char *fname0, uint64_t size, char *buf) {
    lib_title_t *t = add_title(r);
    if (!t) return;
    copy_str(t->path, sizeof(t->path), path);
    t->size = size;
    char fname[256], apath[256];   /* deaktiviert: Endung, Art und Name ohne ".off" */
    copy_str(fname, sizeof(fname), fname0);
    copy_str(apath, sizeof(apath), path);
    if (image_name(fname0) == 2) {
        t->off = 1;
        fname[strlen(fname) - 4] = 0;
        apath[strlen(apath) - 4] = 0;
    }
    {
        const char *dot = strrchr(fname, '.');
        copy_str(t->ext, sizeof(t->ext), dot ? dot + 1 : "");
    }
    snprintf(t->kind, sizeof(t->kind), "%s", ends_with(fname, ".ffpfs") || ends_with(fname, ".ffpfsc") ? "PFS" : "?");
    int fd = open(path, O_RDONLY);
    fsr_t *f = fd >= 0 ? fsr_open(fd, 0) : NULL;
    char name[128] = "";
    if (f) {
        long n;
        snprintf(t->kind, sizeof(t->kind), "%s", fsr_type(f));
        if ((n = fsr_read_file(f, "sce_sys/param.json", buf, PARAM_MAX)) <= 0 ||
            lib_parse_param_json(buf, (size_t)n, t->id, sizeof(t->id), name, sizeof(name)) != 0) {
            if ((n = fsr_read_file(f, "sce_sys/param.sfo", buf, PARAM_MAX)) > 0 &&
                lib_parse_param_sfo((const uint8_t *)buf, (size_t)n, t->id, sizeof(t->id), name, sizeof(name)) == 0)
                param_version(buf, (size_t)n, 0, t->version, sizeof(t->version));
        } else {
            param_version(buf, (size_t)n, 1, t->version, sizeof(t->version));
        }
        fsr_close(f);
    }
    if (fd >= 0) close(fd);
    /* .ffpfs/.ffpfsc (und was sonst nicht lesbar ist): Titel-ID von ShadowMountPlus */
    const lib_hint_t *hp = t->id[0] ? NULL : hint_by_path(apath);   /* ShadowMountPlus kennt den aktiven Namen */
    if (hp) {
        copy_str(t->id, sizeof(t->id), hp->id);
        if (!name[0] && hp->name[0]) copy_str(name, sizeof(name), hp->name);
    }
    const char *hn = !name[0] && t->id[0] ? hint_name(t->id) : NULL;
    if (hn) copy_str(name, sizeof(name), hn);
    if (name[0]) snprintf(t->name, sizeof(t->name), "%s", name);
    else base_name(t->name, sizeof(t->name), fname);
}

static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int pread_all(int fd, void *buf, size_t n, uint64_t off) { return pread(fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1; }

/* Eintrag der CNT-Metadaten (Tabelle ab cnt_off, Big-Endian) mit der Kennung want lesen: Puffer (mit Endnull) oder NULL;
 * *ctype = Inhaltstyp aus dem CNT-Kopf. Nur kleine Einträge (param.sfo/param.json). */
static uint8_t *cnt_entry(int fd, uint64_t cnt_off, uint32_t want, size_t *len, uint32_t *ctype) {
    uint8_t h[0x80];
    if (pread_all(fd, h, sizeof(h), cnt_off) != 0 || memcmp(h, "\x7f" "CNT", 4) != 0) return NULL;
    *ctype = be32p(h + 0x74);
    const uint32_t count = be32p(h + 0x10), table = be32p(h + 0x18);
    if (!count || count > 512 || table < 0x20 || table > 0x100000) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t e[32];
        if (pread_all(fd, e, sizeof(e), cnt_off + table + (uint64_t)i * 32) != 0) return NULL;
        if (be32p(e) != want) continue;
        const uint32_t off = be32p(e + 16), size = be32p(e + 20);
        if (!size || size > 65536) return NULL;
        uint8_t *buf = malloc((size_t)size + 1);
        if (!buf || pread_all(fd, buf, size, cnt_off + off) != 0) { free(buf); return NULL; }
        buf[size] = 0;
        *len = size;
        return buf;
    }
    return NULL;
}

/* Wert eines Schlüssels der param.sfo: Text (is_num = 0) bzw. 32-Bit-Zahl (is_num = 1, als Dezimaltext) */
static int sfo_val(const uint8_t *b, size_t len, const char *key, char *out, size_t n, int is_num) {
    if (len < 20 || memcmp(b, "\0PSF", 4) != 0) return -1;
    const uint32_t kt = le32(b + 8), dt = le32(b + 12), cnt = le32(b + 16);
    for (uint32_t i = 0; i < cnt && 20 + 16 * (size_t)(i + 1) <= len; i++) {
        const uint8_t *ent = b + 20 + 16 * i;
        const size_t ko = (size_t)kt + le16(ent), vo = (size_t)dt + le32(ent + 12), vl = le32(ent + 4);
        if (ko >= len || vo >= len || vl > len - vo || memchr(b + ko, 0, len - ko) == NULL) continue;
        if (strcmp((const char *)b + ko, key) != 0) continue;
        if (is_num) {
            if (vl < 4) return -1;
            snprintf(out, n, "%u", (unsigned)le32(b + vo));
        } else {
            snprintf(out, n, "%.*s", (int)strnlen((const char *)b + vo, vl), (const char *)b + vo);
        }
        return 0;
    }
    return -1;
}

static void fw_text(unsigned hi, unsigned lo, char *out, size_t n) { snprintf(out, n, "%x.%02x", hi & 0xff, lo & 0xff); }

/* Spielname, Version, Art und Mindest-Firmware aus den Metadaten des Pakets */
static void pkg_read_meta(int fd, lib_pkg_t *p, uint64_t cnt_off, int ps5) {
    size_t len = 0;
    uint32_t ctype = 0;
    uint8_t *b = cnt_entry(fd, cnt_off, ps5 ? 0x2000 : 0x1000, &len, &ctype);
    if (!b) return;
    char id[16], name[128], v[32];
    if (!ps5) {   /* param.sfo */
        lib_parse_param_sfo(b, len, id, sizeof(id), name, sizeof(name));
        copy_str(p->title, sizeof(p->title), name);
        if (sfo_val(b, len, "VERSION", v, sizeof(v), 0) == 0 || sfo_val(b, len, "APP_VER", v, sizeof(v), 0) == 0) copy_str(p->version, sizeof(p->version), v);
        if (sfo_val(b, len, "CATEGORY", v, sizeof(v), 0) == 0)
            copy_str(p->cat, sizeof(p->cat), !strcmp(v, "gp") ? "update" : !strcmp(v, "ac") || !strcmp(v, "al") ? "dlc" : "game");
        if (sfo_val(b, len, "SYSTEM_VER", v, sizeof(v), 1) == 0) {
            const unsigned long sv = strtoul(v, NULL, 10);
            if (sv) fw_text((unsigned)(sv >> 24), (unsigned)(sv >> 16), p->minfw, sizeof(p->minfw));
        }
    } else {   /* param.json */
        const char *j = (const char *)b;
        size_t jl = len;
        if (jl >= 3 && !memcmp(j, "\xEF\xBB\xBF", 3)) { j += 3; jl -= 3; }
        lib_parse_param_json(j, jl, id, sizeof(id), name, sizeof(name));
        if (id[0] && !p->title_id[0]) copy_str(p->title_id, sizeof(p->title_id), id);   /* Dateiname ohne Titel-ID (z. B. DLC-Pakete) */
        copy_str(p->title, sizeof(p->title), name);
        if (lib_json_str(j, jl, "contentVersion", v, sizeof(v)) == 0) copy_str(p->version, sizeof(p->version), v);
        copy_str(p->cat, sizeof(p->cat), ctype == 0x21 ? "dlc" : lib_json_str(j, jl, "targetContentVersion", v, sizeof(v)) == 0 ? "update" : "game");
        if (lib_json_str(j, jl, "requiredSystemSoftwareVersion", v, sizeof(v)) == 0) {
            const unsigned long long fv = strtoull(v, NULL, 16);
            if (fv) fw_text((unsigned)(fv >> 56), (unsigned)(fv >> 48), p->minfw, sizeof(p->minfw));
        }
    }
    if (!strcmp(p->cat, "update")) p->update = 1;
    free(b);
}

/* Kopf eines Pakets lesen (wie die LibProsperoPkg es tut): PS4/CNT (7F 43 4E 54, Big-Endian) mit Content-ID bei 0x40 und Inhalts-Flags bei
 * 0x78; PS5 (7F 46 49 48 "FIH", auch 7F 4C 49 48 "LIH"): Byte 0x05 = 0x80 offiziell signiert, 0x00 Debug-Signatur (= fPKG). Eine PS4-fPKG
 * lässt sich vom offiziellen Paket im Kopf nicht unterscheiden (gleiche DRM-/Inhaltstypen). */
static void pkg_read_header(lib_pkg_t *p, const char *fname) {
    uint8_t h[0x80];
    int fd = open(p->path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, h, sizeof(h)) : -1;
    if (n == (ssize_t)sizeof(h) && h[0] == 0x7F && h[1] == 'C' && h[2] == 'N' && h[3] == 'T') {
        pkg_read_meta(fd, p, 0, 0);
        copy_str(p->platform, sizeof(p->platform), "PS4");
        const uint32_t flags = (uint32_t)h[0x78] << 24 | (uint32_t)h[0x79] << 16 | (uint32_t)h[0x7A] << 8 | h[0x7B];
        p->update |= (flags & 0x40000000u) || (flags & 0x00100000u);   /* (bereits aus den Metadaten gesetzt, wenn dort ein Patch steht) */
        char cid[0x25];
        memcpy(cid, h + 0x40, 0x24);
        cid[0x24] = 0;
        const char *dash = strchr(cid, '-');   /* "EP0001-CUSA00775_00-TETRISGAME000000": Titel-ID hinter dem ersten Strich */
        if (dash && strlen(dash) >= 10 && (!strncmp(dash + 1, "CUSA", 4) || !strncmp(dash + 1, "PPSA", 4))) {
            memcpy(p->title_id, dash + 1, 9);
            p->title_id[9] = 0;
        }
    } else if (n == (ssize_t)sizeof(h) && h[0] == 0x7F && (h[1] == 'F' || h[1] == 'L') && h[2] == 'I' && h[3] == 'H') {
        copy_str(p->platform, sizeof(p->platform), "PS5");
        copy_str(p->type, sizeof(p->type), h[5] == 0x80 ? "PKG" : "fPKG");
        uint64_t cnt_off = 0;
        for (int i = 7; i >= 0; i--) cnt_off = cnt_off << 8 | h[0x58 + i];   /* Offset der eingebetteten Metadaten (Little-Endian) */
        if (cnt_off) pkg_read_meta(fd, p, cnt_off, 1);
    }
    if (fd >= 0) close(fd);
    if (!p->title_id[0]) {   /* Dateiname: "…PPSA16833_00…" bzw. "CUSA15884 - …" */
        for (const char *c = fname; *c; c++)
            if ((!strncmp(c, "CUSA", 4) || !strncmp(c, "PPSA", 4)) && strspn(c + 4, "0123456789") >= 5) {
                memcpy(p->title_id, c, 9);
                p->title_id[9] = 0;
                break;
            }
    }
}

/* games: Spiele-Ordner und Abbilder suchen; pkgs: .pkg-Dateien zählen */
static void scan_dir(lib_root_t *r, const char *dir, int games, int pkgs, char *buf) {
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[sizeof(((lib_title_t *)0)->path)];
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_name[0] == '$') continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;   /* zu lang */
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (games) folder_game(r, path, e->d_name, buf);
        } else if (S_ISREG(st.st_mode)) {
            if (games && image_name(e->d_name))
                image_game(r, path, e->d_name, (uint64_t)st.st_size, buf);
            else if (pkgs && ends_with(e->d_name, ".pkg")) {
                r->pkgs++;
                r->pkg_bytes += (uint64_t)st.st_size;
                if (r->npkglist < LIB_PKG_MAX) {
                    lib_pkg_t *p = realloc(r->pkglist, (size_t)(r->npkglist + 1) * sizeof(*p));
                    if (p) {
                        r->pkglist = p;
                        p = &r->pkglist[r->npkglist++];
                        memset(p, 0, sizeof(*p));
                        copy_str(p->path, sizeof(p->path), path);
                        p->size = (uint64_t)st.st_size;
                        pkg_read_header(p, e->d_name);
                    }
                }
            }
        }
    }
    closedir(d);
}

static void root_folders(lib_root_t *r) {
    DIR *d = opendir(r->root);
    struct dirent *e;
    char path[sizeof(r->root) + 260];
    size_t o = 0;
    if (!d) return;
    while ((e = readdir(d))) {
        struct stat st;
        if (e->d_name[0] == '.' || e->d_name[0] == '$' || !strcmp(e->d_name, "System Volume Information")) continue;
        if (snprintf(path, sizeof(path), "%s/%s", r->root, e->d_name) >= (int)sizeof(path)) continue;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        size_t len = strlen(e->d_name);
        if (o + len + 2 >= sizeof(r->folders)) break;
        if (o) r->folders[o++] = '/';
        memcpy(r->folders + o, e->d_name, len);
        o += len;
    }
    r->folders[o] = 0;
    closedir(d);
}

int lib_scan(const char *root, int usb_root, lib_root_t *out) {
    char *buf = malloc(PARAM_MAX + 1), sub[sizeof(out->root) + 16];
    memset(out, 0, sizeof(*out));
    if (!buf || copy_str(out->root, sizeof(out->root), root)) { free(buf); return -1; }
    scan_dir(out, out->root, 1, 1, buf);
    if (usb_root) {
        snprintf(sub, sizeof(sub), "%s/homebrew", out->root);
        scan_dir(out, sub, 1, 1, buf);
        snprintf(sub, sizeof(sub), "%s/etaHEN/games", out->root);
        scan_dir(out, sub, 1, 0, buf);
        snprintf(sub, sizeof(sub), "%s/pkg", out->root);
        scan_dir(out, sub, 0, 1, buf);
        /* Pakete in Unterordnern je Titel: ps4/<CUSA…>/app.pkg, PS5-Updates/<Titel>/update.pkg */
        static const char *const pkg_parents[] = {"ps4", "PS5-Updates", "pkg"};
        for (size_t i = 0; i < sizeof(pkg_parents) / sizeof(*pkg_parents); i++) {
            char parent[sizeof(out->root) + 32];
            snprintf(parent, sizeof(parent), "%s/%s", out->root, pkg_parents[i]);
            DIR *pd = opendir(parent);
            struct dirent *pe;
            while (pd && (pe = readdir(pd))) {
                struct stat pst;
                if (pe->d_name[0] == '.' || snprintf(sub, sizeof(sub), "%s/%s", parent, pe->d_name) >= (int)sizeof(sub) ||
                    stat(sub, &pst) != 0 || !S_ISDIR(pst.st_mode))
                    continue;
                scan_dir(out, sub, 0, 1, buf);
            }
            if (pd) closedir(pd);
        }
        root_folders(out);
    }
    free(buf);
    return 0;
}

void lib_free(lib_root_t *r) {
    free(r->titles);
    free(r->pkglist);
    memset(r, 0, sizeof(*r));
}

int lib_same_game(const char *path, const char *id, uint64_t size) {
    struct stat st;
    char *buf;
    int same = 0;
    if (stat(path, &st) != 0 || !(buf = malloc(PARAM_MAX + 1))) return 0;
    if (S_ISDIR(st.st_mode)) {   /* Ordner: Titel-ID aus param.json/param.sfo wie bei der Suche */
        lib_root_t r;
        memset(&r, 0, sizeof(r));
        folder_game(&r, path, "", buf);
        same = r.ntitles == 1 && !strcmp(r.titles[0].id, id);
        lib_free(&r);
    } else if (S_ISREG(st.st_mode) && (uint64_t)st.st_size == size) {
        /* Abbild: gleiche Größe und - wenn lesbar - gleiche Titel-ID (.ffpfs/.ffpfsc liest nur der Kernel) */
        char now[16] = "", name[128];
        int fd = open(path, O_RDONLY);
        fsr_t *f = fd >= 0 ? fsr_open(fd, 0) : NULL;
        if (f) {
            long n;
            if ((n = fsr_read_file(f, "sce_sys/param.json", buf, PARAM_MAX)) <= 0 ||
                lib_parse_param_json(buf, (size_t)n, now, sizeof(now), name, sizeof(name)) != 0) {
                now[0] = 0;
                if ((n = fsr_read_file(f, "sce_sys/param.sfo", buf, PARAM_MAX)) > 0 &&
                    lib_parse_param_sfo((const uint8_t *)buf, (size_t)n, now, sizeof(now), name, sizeof(name)) != 0)
                    now[0] = 0;
            }
            fsr_close(f);
        }
        if (fd >= 0) close(fd);
        same = !now[0] || !strcmp(now, id);
    }
    free(buf);
    return same;
}
