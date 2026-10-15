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
    if (!p || *p != '[') return -1;
    for (p = jws(p + 1, e); p < e && *p == '{'; ) {
        const char *next = jskip(p, e);
        if (!next) break;
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
    return 0;
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

static void folder_game(lib_root_t *r, const char *path, const char *dname, char *buf) {
    char p[sizeof(((lib_title_t *)0)->path) + 32], id[16], name[128];
    long n;
    int ok = -1;
    if (snprintf(p, sizeof(p), "%s/sce_sys/param.json", path) < (int)sizeof(p) &&
        (n = read_file(p, buf, PARAM_MAX)) > 0)
        ok = lib_parse_param_json(buf, (size_t)n, id, sizeof(id), name, sizeof(name));
    if (ok != 0 && snprintf(p, sizeof(p), "%s/sce_sys/param.sfo", path) < (int)sizeof(p) &&
        (n = read_file(p, buf, PARAM_MAX)) > 0)
        ok = lib_parse_param_sfo((const uint8_t *)buf, (size_t)n, id, sizeof(id), name, sizeof(name));
    if (ok != 0) return;   /* kein Spiel */
    lib_title_t *t = add_title(r);
    if (!t) return;
    copy_str(t->id, sizeof(t->id), id);
    copy_str(t->path, sizeof(t->path), path);
    if (copy_str(t->name, sizeof(t->name), name[0] ? name : dname)) t->name[0] = 0;
    copy_str(t->kind, sizeof(t->kind), "Ordner");
}

static void image_game(lib_root_t *r, const char *path, const char *fname, uint64_t size, char *buf) {
    lib_title_t *t = add_title(r);
    if (!t) return;
    copy_str(t->path, sizeof(t->path), path);
    t->size = size;
    snprintf(t->kind, sizeof(t->kind), "%s", ends_with(fname, ".ffpfs") || ends_with(fname, ".ffpfsc") ? "PFS" : "?");
    int fd = open(path, O_RDONLY);
    fsr_t *f = fd >= 0 ? fsr_open(fd, 0) : NULL;
    char name[128] = "";
    if (f) {
        long n;
        snprintf(t->kind, sizeof(t->kind), "%s", fsr_type(f));
        if ((n = fsr_read_file(f, "sce_sys/param.json", buf, PARAM_MAX)) <= 0 ||
            lib_parse_param_json(buf, (size_t)n, t->id, sizeof(t->id), name, sizeof(name)) != 0) {
            if ((n = fsr_read_file(f, "sce_sys/param.sfo", buf, PARAM_MAX)) > 0)
                lib_parse_param_sfo((const uint8_t *)buf, (size_t)n, t->id, sizeof(t->id), name, sizeof(name));
        }
        fsr_close(f);
    }
    if (fd >= 0) close(fd);
    /* .ffpfs/.ffpfsc (und was sonst nicht lesbar ist): Titel-ID von ShadowMountPlus */
    const lib_hint_t *hp = t->id[0] ? NULL : hint_by_path(path);
    if (hp) {
        copy_str(t->id, sizeof(t->id), hp->id);
        if (!name[0] && hp->name[0]) copy_str(name, sizeof(name), hp->name);
    }
    const char *hn = !name[0] && t->id[0] ? hint_name(t->id) : NULL;
    if (hn) copy_str(name, sizeof(name), hn);
    if (name[0]) snprintf(t->name, sizeof(t->name), "%s", name);
    else base_name(t->name, sizeof(t->name), fname);
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
            if (games && (ends_with(e->d_name, ".exfat") || ends_with(e->d_name, ".ffpkg") ||
                          ends_with(e->d_name, ".ffpfs") || ends_with(e->d_name, ".ffpfsc")))
                image_game(r, path, e->d_name, (uint64_t)st.st_size, buf);
            else if (pkgs && ends_with(e->d_name, ".pkg")) {
                r->pkgs++;
                r->pkg_bytes += (uint64_t)st.st_size;
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
        root_folders(out);
    }
    free(buf);
    return 0;
}

void lib_free(lib_root_t *r) {
    free(r->titles);
    memset(r, 0, sizeof(*r));
}
