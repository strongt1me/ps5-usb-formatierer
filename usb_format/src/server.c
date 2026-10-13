/*
 * Minimaler HTTP-Server: liefert die Weboberflaeche und eine JSON-API.
 *   GET  /               Weboberflaeche
 *   GET  /api/devices    USB-Geräte mit Format, Bezeichnung, Partitionstabelle und Belegung der Einbindungen
 *   GET  /api/library    Spiele auf allen Laufwerken (Hintergrundsuche, wie ShadowMountPlus); ?refresh=1 sucht neu
 *   POST /api/format     {"device":"da0","fs":"exfat|fat32|ntfs|ext4","label":"...","size":<bytes>,
 *                         "cluster":<bytes, 0 = Standard>, "replug":true (für eingebundene Sticks),
 *                         "verify":true (vorher Echtheit und Tempo prüfen), "full":true (jede Stelle beschreiben
 *                         und prüfen), "folders":true (Ordner homebrew, etaHEN/games, pkg anlegen),
 *                         "scheme":"auto|mbr|gpt", "limit":<bytes> (nur den echten Bereich nutzen)}
 *   POST /api/speed      {"device","size"} Lesetempo messen (nur lesen, auch eingebunden)
 *   POST /api/check      {"device","size","repair":true|false,"replug"} exFAT prüfen bzw. reparieren
 *   POST /api/label      {"device","size","label","replug"} Bezeichnung ändern (exFAT/FAT32)
 *   POST /api/delete     {"keys":"<key>,<key>","uninstall":true} Spiele löschen (Schlüssel aus /api/library; über
 *                         ShadowMountPlus, das eingebundene Abbilder selbst freigibt; uninstall: Kachel entfernen)
 *   POST /api/cancel     {} laufenden Auftrag abbrechen (Warten auf Abziehen, Prüfen, Beschreiben)
 *   GET  /api/status     Fortschritt des laufenden/letzten Auftrags: "kind" und "result" (Einzelheiten)
 *   GET  /api/info       LAN-Adresse der PS5 (fuer den Hinweis im PS5-Browser)
 *   POST /api/open       {} Web-UI im PS5-Browser öffnen (für die App, app/title/starter.c)
 *   GET  /api/settings   {"theme":"auto|light|dark","scale_tv":160,"scale_pc":100}; POST ändert (einzeln
 *                         oder zusammen) und speichert es
 *   POST /api/diag       Darstellungsbericht der Seite ablegen; GET liefert {"tv":"...","pc":"..."}
 *   POST /api/quit       Server beenden
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

#include "devtest.h"
#include "exfat_fsck.h"
#include "fsdetect.h"
#include "library.h"
#include "mkfs.h"
#include "platform.h"
#include "server.h"
#include "vollabel.h"

#define REQ_MAX   16384
#define BODY_MAX  4096
#define JOB_STACK (4 * 1024 * 1024)   /* Threads haben auf der PS5 nur 64 KiB - fsck.exfat braucht mehr */
#define MIB       (1024ull * 1024ull)

/* Weboberflaeche wird beim Kompilieren eingebettet (Pfad relativ zum Projektordner) */
__asm__(".section .rodata\n"
        ".global web_index_html\n"
        "web_index_html:\n"
        ".incbin \"web/index.html\"\n"
        ".byte 0\n"
        ".global web_bg_jpg\n"
        "web_bg_jpg:\n"
        ".incbin \"web/bg.jpg\"\n"
        ".global web_bg_jpg_end\n"
        "web_bg_jpg_end:\n"
        ".global web_page_dark\n"
        "web_page_dark:\n"
        ".incbin \"web/page-dark.jpg\"\n"
        ".global web_page_dark_end\n"
        "web_page_dark_end:\n"
        ".global web_page_light\n"
        "web_page_light:\n"
        ".incbin \"web/page-light.jpg\"\n"
        ".global web_page_light_end\n"
        "web_page_light_end:\n"
        ".previous\n");
extern const char web_index_html[];
extern const char web_bg_jpg[], web_bg_jpg_end[];
extern const char web_page_dark[], web_page_dark_end[], web_page_light[], web_page_light_end[];

/* ------------------------------------------------------------ JSON-Ausgabe */

typedef struct {
    char  *p;
    size_t len, cap;
} sbuf_t;

static void sb_add(sbuf_t *b, const char *fmt, ...) {
    va_list ap;
    if (!b->p) {
        b->p = malloc(256);
        if (!b->p) return;
        b->cap = 256;
        b->p[0] = 0;
    }
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (b->len + (size_t)n < b->cap) { b->len += n; return; }
        size_t cap = (b->cap + n) * 2;
        char *np = realloc(b->p, cap);
        if (!np) return;
        b->p = np;
        b->cap = cap;
    }
}

static void sb_str(sbuf_t *b, const char *s) {
    sb_add(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') sb_add(b, "\\%c", c);
        else if (c < 0x20) sb_add(b, "\\u%04x", c);
        else sb_add(b, "%c", c);
    }
    sb_add(b, "\"");
}

/* "58,6 GB" (1024er-Einheiten wie in der Oberfläche) */
static const char *size_text(uint64_t b, char *buf, size_t n) {
    static const char *const unit[] = {"Bytes", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    snprintf(buf, n, i ? "%.1f %s" : "%.0f %s", v, unit[i]);
    char *dot = strchr(buf, '.');
    if (dot) *dot = ',';
    return buf;
}

/* ------------------------------------------------------------ Zustand und Aufträge */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int  busy, done, ok;
    char kind[12];        /* format, speed, check, label */
    char device[16];
    char step[64];
    int  percent;
    char message[512];
    char result[4096];    /* JSON-Objekt mit Einzelheiten oder leer */
} st;

static volatile int quit_requested;
static volatile int cancel_requested;   /* POST /api/cancel */

static void lib_refresh(void);

static void on_progress(const char *step, int pct) {
    pthread_mutex_lock(&lock);
    snprintf(st.step, sizeof(st.step), "%s", step);
    st.percent = pct;
    pthread_mutex_unlock(&lock);
    plat_throttle();
}

enum { JOB_FORMAT = 1, JOB_SPEED, JOB_CHECK, JOB_LABEL, JOB_DELETE };
static const char *const JOB_KIND[] = {"", "format", "speed", "check", "label", "delete"};

/* Spiel zum Löschen (aus der Bibliothek übernommen) */
typedef struct {
    char id[16], name[128], path[256], root[96], device[16];
} del_item_t;

typedef struct {
    int      kind;
    usbdev_t dev;
    int      replug;     /* von der PS5 eingebunden: per Software abmelden und exklusiv übernehmen */
    char     label[33];
    /* Formatieren */
    int      fs;
    uint32_t cluster;    /* Bytes, 0 = Standard */
    int      verify, full, folders, scheme;
    uint64_t limit;
    /* Prüfen */
    int      repair;
    /* Spiele löschen */
    del_item_t *items;
    int      nitems, uninstall;
} job_t;

static const char *const TEMPLATE_DIRS[] = {"homebrew", "etaHEN/games", "pkg", NULL};

static void finish(int ok, const char *msg, const char *result) {
    pthread_mutex_lock(&lock);
    st.busy = 0;
    st.done = 1;
    st.ok = ok;
    st.percent = ok ? 100 : st.percent;
    snprintf(st.step, sizeof(st.step), "%s", ok ? "Fertig" : "Fehler");
    snprintf(st.message, sizeof(st.message), "%s", msg ? msg : "");
    /* abgeschnittenes JSON wäre ungültig - dann lieber keine Einzelheiten */
    if (result && strlen(result) < sizeof(st.result)) memcpy(st.result, result, strlen(result) + 1);
    else st.result[0] = 0;
    pthread_mutex_unlock(&lock);
    plat_notify(msg ? msg : "");
}

static void dev_title(const usbdev_t *d, sbuf_t *b) {
    if (d->model[0]) sb_add(b, "%s (%s)", d->model, d->name);
    else sb_add(b, "%s", d->name);
}

/* Datenträger zum Schreiben übernehmen: eingebunden per Software-Abziehen (bzw. Abziehen von Hand), sonst
 * direkt exklusiv. Liefert den fd oder -1 mit Fehlertext. */
static int job_acquire(job_t *j, char *err, size_t n) {
    on_progress("Laufwerk öffnen", 0);
    if (!j->replug) return plat_open_rw(&j->dev, err, n);
    usbdev_t fresh;
    int fd = plat_wait_replug(&j->dev, &fresh, on_progress, &cancel_requested, err, n);
    if (fd >= 0) {
        if (!fresh.model[0]) snprintf(fresh.model, sizeof(fresh.model), "%s", j->dev.model);
        snprintf(fresh.label, sizeof(fresh.label), "%s", j->dev.label);
        snprintf(fresh.format, sizeof(fresh.format), "%s", j->dev.format);
        j->dev = fresh;   /* kann nach dem Neu-Anmelden einen anderen Namen haben (daN) */
        pthread_mutex_lock(&lock);
        snprintf(st.device, sizeof(st.device), "%s", fresh.name);
        pthread_mutex_unlock(&lock);
    }
    return fd;
}

static void speed_json(sbuf_t *b, const devtest_t *t) {
    if (t->read_mbps > 0) sb_add(b, ",\"read\":%.1f", t->read_mbps);
    if (t->write_mbps > 0) sb_add(b, ",\"write\":%.1f", t->write_mbps);
    if (t->read_mbps > 0) { sb_add(b, ",\"rating\":"); sb_str(b, dt_rating(t->read_mbps)); }
}

static const char *rating_text(double read_mbps) {
    const char *r = dt_rating(read_mbps);
    return !strcmp(r, "schnell") ? "schnell genug für Spiele" : !strcmp(r, "ausreichend") ? "für Spiele ausreichend"
                                                                                          : "langsam – Spiele laden länger";
}

static void job_done(job_t *j, int ok, sbuf_t *msg, sbuf_t *res, int refresh) {
    finish(ok, msg->p, res->p);
    free(msg->p);
    free(res->p);
    free(j);
    if (refresh) lib_refresh();
}

static void *format_worker(void *arg) {
    job_t *j = arg;
    char err[256] = "", summary[160] = "", s1[24], s2[24];
    sbuf_t msg = {0}, res = {0};
    devtest_t t;
    int tested = 0;
    memset(&t, 0, sizeof(t));

    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    const uint64_t size = j->limit && j->limit < j->dev.size ? j->limit : j->dev.size;

    /* Vorher prüfen: Stichproben (schnell) oder jede Stelle (vollständig). Gefälscht oder defekt: nichts
     * formatieren, die Oberfläche bietet "nur den echten Bereich formatieren" an. Abgebrochen: trotzdem
     * formatieren - die Daten sind ohnehin schon überschrieben. */
    int test_cancelled = 0;
    if (j->verify || j->full) {
        tested = j->full ? 2 : 1;
        int rc = j->full ? dt_full(fd, j->dev.ss, size, on_progress, &cancel_requested, &t, err, sizeof(err))
                         : dt_verify(fd, j->dev.ss, size, on_progress, &cancel_requested, &t, err, sizeof(err));
        if (rc == 0 && t.cancelled) {
            test_cancelled = 1;
            cancel_requested = 0;
        } else if (rc != 0 || t.fake || t.bad) {
            close(fd);
            sb_add(&res, "{\"test\":\"%s\",\"fake\":%s,\"bad\":%s,\"reported\":%llu,\"real\":%llu,\"bad_at\":%llu",
                   tested == 2 ? "full" : "quick", t.fake ? "true" : "false", t.bad ? "true" : "false",
                   (unsigned long long)j->dev.size, (unsigned long long)t.real_bytes, (unsigned long long)t.bad_at);
            speed_json(&res, &t);
            sb_add(&res, "}");
            if (rc != 0) {
                sb_add(&msg, "Prüfung fehlgeschlagen: %s Nichts formatiert.", err);
            } else if (t.fake) {
                dev_title(&j->dev, &msg);
                sb_add(&msg, " meldet %s, hat aber nur etwa %s echten Speicher. Nichts formatiert – sicher nutzbar ist "
                             "nur der echte Bereich.", size_text(j->dev.size, s1, sizeof(s1)),
                       size_text(t.real_bytes, s2, sizeof(s2)));
            } else {
                sb_add(&msg, "Fehlerhafte Bereiche ab %llu MB: ", (unsigned long long)(t.bad_at / MIB));
                dev_title(&j->dev, &msg);
                sb_add(&msg, " arbeitet nicht zuverlässig. Nichts formatiert.");
            }
            job_done(j, 0, &msg, &res, 1);
            return NULL;
        }
    }

    blkdev_t d = {0};
    d.fd = fd;
    d.ss = j->dev.ss;
    d.sectors = j->dev.size / j->dev.ss;
    d.progress = on_progress;
    const int native = j->fs == FS_EXFAT || j->fs == FS_FAT32;
    mkfs_opts_t o = {0};
    o.scheme = j->scheme;
    o.limit = j->limit;
    o.dirs = j->folders && native ? TEMPLATE_DIRS : NULL;
    int rc;
    if (native) {
        rc = format_disk(&d, j->fs, j->label, arc4random(), j->cluster, &o);
        close(fd);
        if (rc == 0) snprintf(summary, sizeof(summary), "%s", mkfs_summary());
        else snprintf(err, sizeof(err), "%s", mkfs_error());
    } else {
        /* NTFS/ext4: Partition selbst anlegen, Dateisystem schreibt das Originalwerkzeug */
        uint64_t start, count;
        rc = prepare_partition(&d, j->fs, j->label, arc4random(), &o, &start, &count);
        close(fd);
        if (rc != 0) snprintf(err, sizeof(err), "%s", mkfs_error());
        else rc = plat_external_mkfs(&j->dev, j->fs, j->label, j->cluster, start, count, o.is_gpt, on_progress,
                                     summary, sizeof(summary), err, sizeof(err));
    }
    if (rc != 0) {
        sb_add(&msg, "Formatieren von %s fehlgeschlagen: %s", j->dev.name, err);
        job_done(j, 0, &msg, &res, 1);
        return NULL;
    }

    dev_title(&j->dev, &msg);
    sb_add(&msg, " erfolgreich formatiert: %s", summary);
    if (j->label[0]) sb_add(&msg, ", Bezeichnung %s", j->label);
    if (o.is_gpt) sb_add(&msg, ", Partitionstabelle GPT");
    if (j->limit) sb_add(&msg, ", nur die echten %s genutzt", size_text(j->limit, s1, sizeof(s1)));
    if (o.dirs) sb_add(&msg, ", Ordner homebrew, etaHEN/games und pkg angelegt");
    sb_add(&msg, ".");
    if (tested && !test_cancelled && t.read_mbps > 0)
        sb_add(&msg, " %s: echt%s, lesen %.0f MB/s, schreiben %.0f MB/s – %s.", tested == 2 ? "Jede Stelle geprüft"
               : "Prüfung", tested == 2 ? ", keine Fehler" : "", t.read_mbps, t.write_mbps, rating_text(t.read_mbps));
    if (test_cancelled) sb_add(&msg, " Die Prüfung wurde abgebrochen – formatiert wurde trotzdem.");
    sb_add(&msg, " %s", native ? "Die PS5 bindet ihn gleich selbst ein." : "Die PS5 kann dieses Format nicht nutzen.");

    sb_add(&res, "{\"test\":\"%s\",\"scheme\":\"%s\",\"folders\":%s,\"limit\":%llu",
           test_cancelled ? "cancelled" : tested == 2 ? "full" : tested ? "quick" : "none", o.is_gpt ? "GPT" : "MBR",
           o.dirs ? "true" : "false", (unsigned long long)j->limit);
    if (tested && !test_cancelled) speed_json(&res, &t);
    sb_add(&res, "}");
    job_done(j, 1, &msg, &res, 1);
    return NULL;
}

static void *speed_worker(void *arg) {
    job_t *j = arg;
    char err[256] = "";
    sbuf_t msg = {0}, res = {0};
    devtest_t t;
    int fd = plat_open_ro(&j->dev, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    int rc = dt_read_speed(fd, j->dev.ss, j->dev.size, on_progress, &cancel_requested, &t, err, sizeof(err));
    close(fd);
    if (rc != 0) { sb_add(&msg, "Tempo messen fehlgeschlagen: %s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    if (t.cancelled) { sb_add(&msg, "Abgebrochen."); job_done(j, 0, &msg, &res, 0); return NULL; }
    sb_add(&msg, "Lesetempo von ");
    dev_title(&j->dev, &msg);
    sb_add(&msg, ": %.0f MB/s – %s.", t.read_mbps, rating_text(t.read_mbps));
    sb_add(&res, "{\"x\":0");
    speed_json(&res, &t);
    sb_add(&res, "}");
    job_done(j, 1, &msg, &res, 0);
    return NULL;
}

static void *check_worker(void *arg) {
    job_t *j = arg;
    char err[256] = "", sum[256];
    sbuf_t msg = {0}, res = {0};
    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    fsinfo_t fi;
    fs_probe(fd, j->dev.ss, j->dev.size, &fi);
    if (strcmp(fi.fs, "exFAT") != 0) {
        close(fd);
        sb_add(&msg, "Prüfen und Reparieren geht nur bei exFAT (dieser Datenträger: %s).", fi.fs);
        job_done(j, 0, &msg, &res, 0);
        return NULL;
    }
    on_progress(j->repair ? "Reparieren (fsck.exfat)" : "Prüfen (fsck.exfat)", 40);
    fsck_result_t r;
    exfat_fsck_run(fd, fi.start, fi.size ? fi.size : j->dev.size - fi.start, j->dev.ss, j->repair, &r);
    close(fd);
    exfat_fsck_summary(&r, j->repair, sum, sizeof(sum));
    dev_title(&j->dev, &msg);
    sb_add(&msg, ": %s", sum);
    sb_add(&res, "{\"code\":%d,\"repair\":%s,\"dirs\":%ld,\"files\":%ld,\"log\":", r.code,
           j->repair ? "true" : "false", r.dirs, r.files);
    size_t loglen = strlen(r.log);
    sb_str(&res, loglen > 1000 ? r.log + loglen - 1000 : r.log);   /* nur das Ende */
    sb_add(&res, "}");
    int ok = r.code == 0 || r.code == 1 || (!j->repair && r.code == 4);
    job_done(j, ok, &msg, &res, j->repair);
    return NULL;
}

static void *label_worker(void *arg) {
    job_t *j = arg;
    char err[256] = "";
    sbuf_t msg = {0}, res = {0};
    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    on_progress("Bezeichnung schreiben", 50);
    int rc = vol_set_label(fd, j->dev.ss, j->dev.size, j->label, err, sizeof(err));
    close(fd);
    if (rc != 0) { sb_add(&msg, "Umbenennen fehlgeschlagen: %s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    if (j->label[0]) { sb_add(&msg, "Neue Bezeichnung für "); dev_title(&j->dev, &msg); sb_add(&msg, ": „%s“.", j->label); }
    else { sb_add(&msg, "Bezeichnung von "); dev_title(&j->dev, &msg); sb_add(&msg, " entfernt."); }
    sb_add(&res, "{\"label\":");
    sb_str(&res, j->label);
    sb_add(&res, "}");
    job_done(j, 1, &msg, &res, 1);
    return NULL;
}

/* ------------------------------------------------------------ Spielebibliothek (Hintergrundsuche) */

typedef struct {
    char       device[16];   /* leer: interner Ordner (z. B. /data/homebrew) */
    int        usb;
    lib_root_t r;
} libroot_t;

static pthread_mutex_t lib_lock = PTHREAD_MUTEX_INITIALIZER;
static libroot_t *lib_snap;
static int lib_n, lib_busy, lib_again;
static time_t lib_when;

/* Kurzer Schlüssel eines Spiels für /api/delete (FNV-1a über den Pfad): gelöscht wird nur, was die Suche fand */
static uint32_t path_key(const char *p) {
    uint32_t h = 2166136261u;
    while (*p) { h ^= (unsigned char)*p++; h *= 16777619u; }
    return h;
}

static void lib_free_all(libroot_t *r, int n) {
    for (int i = 0; i < n; i++) lib_free(&r[i].r);
    free(r);
}

static void *lib_worker(void *arg) {
    (void)arg;
    for (;;) {
        lib_hint_t *hints = NULL;   /* Titel-IDs für .ffpfs/.ffpfsc von ShadowMountPlus */
        int nh = 0;
        plat_library_hints(&hints, &nh);
        lib_set_hints(hints, nh);
        free(hints);
        usbdev_t *devs = calloc(MAX_DEVS, sizeof(*devs));
        int nd = devs ? plat_scan(devs, MAX_DEVS) : 0;
        const char *const *extra = plat_library_roots();
        int cap = (nd > 0 ? nd : 0) * MAX_MOUNTS + 16, n = 0;
        libroot_t *roots = calloc((size_t)cap, sizeof(*roots));
        for (int i = 0; roots && i < nd; i++) {
            if (devs[i].locked) continue;
            for (int m = 0; m < devs[i].nmounts && n < cap; m++) {
                snprintf(roots[n].device, sizeof(roots[n].device), "%s", devs[i].name);
                roots[n].usb = 1;
                lib_scan(devs[i].mounts[m].path, 1, &roots[n].r);
                n++;
            }
        }
        for (int k = 0; roots && extra && extra[k] && extra[k + 1] && n < cap; k += 2) {
            roots[n].usb = !strcmp(extra[k + 1], "usb");
            lib_scan(extra[k], roots[n].usb, &roots[n].r);
            if (roots[n].r.ntitles || roots[n].r.pkgs) n++;
            else lib_free(&roots[n].r);
        }
        free(devs);

        pthread_mutex_lock(&lib_lock);
        libroot_t *old = lib_snap;
        int oldn = lib_n;
        lib_snap = roots;
        lib_n = roots ? n : 0;
        lib_when = time(NULL);
        int again = lib_again;
        lib_again = 0;
        if (!again) lib_busy = 0;
        pthread_mutex_unlock(&lib_lock);
        lib_free_all(old, oldn);
        if (!again) return NULL;
    }
}

static void lib_refresh(void) {
    pthread_mutex_lock(&lib_lock);
    if (lib_busy) {
        lib_again = 1;
        pthread_mutex_unlock(&lib_lock);
        return;
    }
    lib_busy = 1;
    pthread_mutex_unlock(&lib_lock);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int rc = pthread_create(&t, &attr, lib_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc == 0) pthread_detach(t);
    else { pthread_mutex_lock(&lib_lock); lib_busy = 0; pthread_mutex_unlock(&lib_lock); }
}

static void api_library(int c, int refresh);

/* ------------------------------------------------------------ JSON lesen */

/* Flaches JSON-Objekt mit String-, Zahl- und Bool-Werten einlesen */
#define KV_MAX 16
typedef struct { char key[16]; char val[64]; } kv_t;

static const char *ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

static const char *json_string(const char *p, char *out, size_t n) {
    size_t i = 0;
    if (*p++ != '"') return NULL;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c != '"' && c != '\\' && c != '/') return NULL;
        }
        if ((unsigned char)c < 0x20 || i + 1 >= n) return NULL;
        out[i++] = c;
    }
    if (*p != '"') return NULL;
    out[i] = 0;
    return p + 1;
}

static int json_parse(const char *p, kv_t *kv, int max) {
    int n = 0;
    p = ws(p);
    if (*p++ != '{') return -1;
    p = ws(p);
    if (*p == '}') return 0;
    for (;;) {
        if (n >= max) return -1;
        if (!(p = json_string(ws(p), kv[n].key, sizeof(kv[n].key)))) return -1;
        p = ws(p);
        if (*p++ != ':') return -1;
        p = ws(p);
        if (*p == '"') {
            if (!(p = json_string(p, kv[n].val, sizeof(kv[n].val)))) return -1;
        } else {
            size_t i = 0;
            while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z')) {
                if (i + 1 >= sizeof(kv[n].val)) return -1;
                kv[n].val[i++] = *p++;
            }
            if (!i) return -1;
            kv[n].val[i] = 0;
        }
        n++;
        p = ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') return n;
        return -1;
    }
}

static const char *kv_get(kv_t *kv, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (strcmp(kv[i].key, key) == 0) return kv[i].val;
    return NULL;
}

static int kv_true(kv_t *kv, int n, const char *key) {
    const char *v = kv_get(kv, n, key);
    return v && strcmp(v, "true") == 0;
}

/* ------------------------------------------------------------ HTTP */

static void send_all(int c, const char *p, size_t len) {
    while (len) {
        ssize_t n = send(c, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        p += n;
        len -= n;
    }
}

static void respond_ex(int c, int code, const char *ctype, const char *body, size_t len, const char *cache) {
    char hdr[512];
    const char *reason = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" :
                         code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
                         code == 409 ? "Conflict" : code == 415 ? "Unsupported Media Type" : "Error";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: %s\r\nX-Content-Type-Options: nosniff\r\n"
                     "X-Frame-Options: DENY\r\nConnection: close\r\n\r\n",
                     code, reason, ctype, len, cache);
    send_all(c, hdr, (size_t)n);
    send_all(c, body, len);
}

static void respond(int c, int code, const char *ctype, const char *body, size_t len) {
    respond_ex(c, code, ctype, body, len, "no-store");
}

static void respond_json(int c, int code, sbuf_t *b) {
    respond(c, code, "application/json; charset=utf-8", b->p ? b->p : "{}", b->p ? b->len : 2);
}

static void respond_error(int c, int code, const char *msg) {
    sbuf_t b = {0};
    sb_add(&b, "{\"error\":");
    sb_str(&b, msg);
    sb_add(&b, "}");
    respond_json(c, code, &b);
    free(b.p);
}

static void respond_ok(int c, int code) {
    sbuf_t b = {0};
    sb_add(&b, "{\"ok\":true}");
    respond_json(c, code, &b);
    free(b.p);
}

/* Header-Wert (ohne fuehrende Leerzeichen) in out kopieren */
static int header(const char *hdrs, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    for (const char *p = hdrs; (p = strstr(p, "\r\n")); ) {
        p += 2;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1, *e;
            while (*v == ' ' || *v == '\t') v++;
            e = strstr(v, "\r\n");
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 1;
        }
    }
    return 0;
}

/* Host muss eine IPv4-Adresse oder localhost sein (Schutz gegen DNS-Rebinding) */
static int host_ok(const char *host) {
    if (strncmp(host, "localhost", 9) == 0 && (host[9] == 0 || host[9] == ':')) return 1;
    for (const char *p = host; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ':')) return 0;
    return *host != 0;
}

/* Bei Browser-Anfragen muss Origin zum Host passen (Schutz gegen fremde Webseiten) */
static int origin_ok(const char *hdrs, const char *host) {
    char origin[160];
    if (!header(hdrs, "Origin", origin, sizeof(origin))) return 1;
    const char *o = strncmp(origin, "http://", 7) == 0 ? origin + 7 : NULL;
    return o && strcmp(o, host) == 0;
}

static void api_devices(int c) {
    usbdev_t devs[MAX_DEVS];
    int n = plat_scan(devs, MAX_DEVS);
    sbuf_t b = {0};

    pthread_mutex_lock(&lock);
    int busy = st.busy;
    pthread_mutex_unlock(&lock);

    sb_add(&b, "{\"busy\":%s,\"devices\":[", busy ? "true" : "false");
    for (int i = 0; i < n; i++) {
        usbdev_t *d = &devs[i];
        sb_add(&b, "%s{\"name\":", i ? "," : "");
        sb_str(&b, d->name);
        sb_add(&b, ",\"model\":");
        sb_str(&b, d->model);
        sb_add(&b, ",\"format\":");
        sb_str(&b, d->format);
        sb_add(&b, ",\"label\":");
        sb_str(&b, d->label);
        sb_add(&b, ",\"scheme\":");
        sb_str(&b, d->scheme);
        sb_add(&b, ",\"size\":%llu,\"sector\":%u,\"locked\":%s,\"mounts\":[",
               (unsigned long long)d->size, d->ss, d->locked ? "true" : "false");
        for (int m = 0; m < d->nmounts; m++) {
            struct statvfs vs;
            sb_add(&b, "%s{\"path\":", m ? "," : "");
            sb_str(&b, d->mounts[m].path);
            sb_add(&b, ",\"fs\":");
            sb_str(&b, d->mounts[m].fstype);
            if (statvfs(d->mounts[m].path, &vs) == 0 && vs.f_blocks)
                sb_add(&b, ",\"total\":%llu,\"free\":%llu", (unsigned long long)vs.f_blocks * vs.f_frsize,
                       (unsigned long long)vs.f_bavail * vs.f_frsize);
            sb_add(&b, "}");
        }
        sb_add(&b, "]}");
    }
    sb_add(&b, "]}");
    respond_json(c, 200, &b);
    free(b.p);
}

/* Spiele aller Laufwerke; doppelte Titel-IDs (ShadowMountPlus wechselt dann ständig zwischen ihnen) gesondert */
static void api_library(int c, int refresh) {
    if (refresh) lib_refresh();
    sbuf_t b = {0};
    pthread_mutex_lock(&lib_lock);
    if (!lib_snap && !lib_busy) {   /* erste Anfrage: Suche anstoßen */
        pthread_mutex_unlock(&lib_lock);
        lib_refresh();
        pthread_mutex_lock(&lib_lock);
    }
    sb_add(&b, "{\"scanning\":%s,\"age\":%ld,\"roots\":[", lib_busy ? "true" : "false",
           lib_when ? (long)(time(NULL) - lib_when) : -1L);
    for (int i = 0; i < lib_n; i++) {
        const libroot_t *r = &lib_snap[i];
        sb_add(&b, "%s{\"root\":", i ? "," : "");
        sb_str(&b, r->r.root);
        sb_add(&b, ",\"device\":");
        sb_str(&b, r->device);
        sb_add(&b, ",\"usb\":%s,\"titles\":%d,\"pkgs\":%d,\"pkg_bytes\":%llu,\"folders\":",
               r->usb ? "true" : "false", r->r.ntitles, r->r.pkgs, (unsigned long long)r->r.pkg_bytes);
        sb_str(&b, r->r.folders);
        sb_add(&b, "}");
    }
    sb_add(&b, "],\"titles\":[");
    int first = 1;
    for (int i = 0; i < lib_n; i++)
        for (int k = 0; k < lib_snap[i].r.ntitles; k++) {
            const lib_title_t *t = &lib_snap[i].r.titles[k];
            sb_add(&b, "%s{\"id\":", first ? "" : ",");
            first = 0;
            sb_str(&b, t->id);
            sb_add(&b, ",\"name\":");
            sb_str(&b, t->name);
            sb_add(&b, ",\"kind\":");
            sb_str(&b, t->kind);
            sb_add(&b, ",\"path\":");
            sb_str(&b, t->path);
            sb_add(&b, ",\"key\":\"%08x\",\"size\":%llu,\"root\":", path_key(t->path), (unsigned long long)t->size);
            sb_str(&b, lib_snap[i].r.root);
            sb_add(&b, ",\"device\":");
            sb_str(&b, lib_snap[i].device);
            sb_add(&b, "}");
        }
    /* Duplikate: jede Titel-ID, die mehr als einmal vorkommt, einmal mit allen Fundorten */
    sb_add(&b, "],\"duplicates\":[");
    first = 1;
    for (int i = 0; i < lib_n; i++)
        for (int k = 0; k < lib_snap[i].r.ntitles; k++) {
            const lib_title_t *t = &lib_snap[i].r.titles[k];
            int seen_before = 0, count = 0;
            if (!t->id[0]) continue;
            for (int i2 = 0; i2 < lib_n; i2++)
                for (int k2 = 0; k2 < lib_snap[i2].r.ntitles; k2++) {
                    if (strcmp(lib_snap[i2].r.titles[k2].id, t->id) != 0) continue;
                    count++;
                    if (i2 < i || (i2 == i && k2 < k)) seen_before = 1;
                }
            if (count < 2 || seen_before) continue;
            sb_add(&b, "%s{\"id\":", first ? "" : ",");
            first = 0;
            sb_str(&b, t->id);
            sb_add(&b, ",\"name\":");
            sb_str(&b, t->name);
            sb_add(&b, ",\"paths\":[");
            int pf = 1;
            for (int i2 = 0; i2 < lib_n; i2++)
                for (int k2 = 0; k2 < lib_snap[i2].r.ntitles; k2++)
                    if (!strcmp(lib_snap[i2].r.titles[k2].id, t->id)) {
                        sb_add(&b, pf ? "" : ",");
                        pf = 0;
                        sb_str(&b, lib_snap[i2].r.titles[k2].path);
                    }
            sb_add(&b, "]}");
        }
    sb_add(&b, "]}");
    pthread_mutex_unlock(&lib_lock);
    respond_json(c, 200, &b);
    free(b.p);
}

static void api_info(int c) {
    char ip[64];
    sbuf_t b = {0};
    plat_local_ip(ip, sizeof(ip));
    sb_add(&b, "{\"ip\":");
    sb_str(&b, ip);
    sb_add(&b, ",\"port\":%d}", SERVER_PORT);
    respond_json(c, 200, &b);
    free(b.p);
}

/* ------------------------------------------------------------ Einstellungen (Design, Größe) */

static char theme[8] = "auto";
/* Größe der Oberfläche in Prozent, getrennt für den PS5-Browser (Fernseher) und andere Browser */
#define SCALE_MIN 50
#define SCALE_MAX 250
static int scale_tv = 160, scale_pc = 100;

/* "123" -> 123, sonst -1 (auch außerhalb SCALE_MIN..SCALE_MAX) */
static int scale_parse(const char *s) {
    if (!s || !*s || strlen(s) > 3) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
    }
    return v >= SCALE_MIN && v <= SCALE_MAX ? v : -1;
}

static int theme_valid(const char *t) {
    return !strcmp(t, "auto") || !strcmp(t, "light") || !strcmp(t, "dark");
}

static void theme_set(const char *t) {  /* t ist geprüft (theme_valid), passt also */
    memcpy(theme, t, strlen(t) + 1);
}

static void settings_path(char *p, size_t n) {
    snprintf(p, n, "%s/settings.json", plat_data_dir());
}

static void settings_load(void) {
    char path[256], buf[512];
    kv_t kv[KV_MAX];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    int nkv = json_parse(buf, kv, KV_MAX);
    if (nkv <= 0) return;
    const char *t = kv_get(kv, nkv, "theme");
    if (t && theme_valid(t)) theme_set(t);
    int v = scale_parse(kv_get(kv, nkv, "scale_tv"));
    if (v > 0) scale_tv = v;
    v = scale_parse(kv_get(kv, nkv, "scale_pc"));
    if (v > 0) scale_pc = v;
}

static int settings_save(void) {
    char path[256];
    mkdir(plat_data_dir(), 0755);
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fprintf(f, "{\"theme\":\"%s\",\"scale_tv\":%d,\"scale_pc\":%d}\n", theme, scale_tv, scale_pc) > 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static void api_settings(int c, int is_post, const char *body) {
    if (is_post) {
        /* einzeln oder zusammen: theme (auto|light|dark), scale_tv / scale_pc (50..250 %) */
        kv_t kv[KV_MAX];
        int n = json_parse(body, kv, KV_MAX);
        const char *t = n > 0 ? kv_get(kv, n, "theme") : NULL;
        const char *stv = n > 0 ? kv_get(kv, n, "scale_tv") : NULL, *spc = n > 0 ? kv_get(kv, n, "scale_pc") : NULL;
        if (!t && !stv && !spc) { respond_error(c, 400, "theme, scale_tv oder scale_pc angeben"); return; }
        if (t && !theme_valid(t)) { respond_error(c, 400, "theme muss auto, light oder dark sein"); return; }
        if ((stv && scale_parse(stv) < 0) || (spc && scale_parse(spc) < 0)) {
            respond_error(c, 400, "Größe muss zwischen 50 und 250 % liegen");
            return;
        }
        pthread_mutex_lock(&lock);
        if (t) theme_set(t);
        if (stv) scale_tv = scale_parse(stv);
        if (spc) scale_pc = scale_parse(spc);
        pthread_mutex_unlock(&lock);
        if (settings_save() != 0) { respond_error(c, 500, "Einstellung ließ sich nicht speichern"); return; }
    }
    sbuf_t b = {0};
    pthread_mutex_lock(&lock);
    sb_add(&b, "{\"theme\":");
    sb_str(&b, theme);
    sb_add(&b, ",\"scale_tv\":%d,\"scale_pc\":%d}", scale_tv, scale_pc);
    pthread_mutex_unlock(&lock);
    respond_json(c, 200, &b);
    free(b.p);
}

/* Diagnose der Darstellung: Die Seite meldet, was der Browser von CSS und Bildern hält (POST /api/diag), lesbar
 * über GET /api/diag. Je ein Bericht für den PS5-Browser und für andere Browser, nur im Speicher. */
static char diag_tv[BODY_MAX], diag_pc[BODY_MAX];

static void api_diag(int c, int is_post, const char *body) {
    sbuf_t b = {0};
    pthread_mutex_lock(&lock);
    if (is_post) {
        char *dst = strstr(body, "PlayStation") ? diag_tv : diag_pc;
        size_t i = 0;
        for (; body[i] && i + 1 < BODY_MAX; i++)
            dst[i] = (unsigned char)body[i] < 0x20 ? ' ' : body[i];
        dst[i] = 0;
        sb_add(&b, "{\"ok\":true}");
    } else {
        sb_add(&b, "{\"tv\":");
        sb_str(&b, diag_tv);
        sb_add(&b, ",\"pc\":");
        sb_str(&b, diag_pc);
        sb_add(&b, "}");
    }
    pthread_mutex_unlock(&lock);
    respond_json(c, 200, &b);
    free(b.p);
}

/* Vom App-Starter (app/title/starter.c) aufgerufen: Web-UI im PS5-Browser öffnen */
static void api_open(int c) {
    char url[64], err[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", SERVER_PORT);
    if (plat_open_browser(url, err, sizeof(err)) != 0) {
        respond_error(c, 500, err);
        return;
    }
    respond_ok(c, 200);
}

static void api_status(int c) {
    sbuf_t b = {0};
    pthread_mutex_lock(&lock);
    sb_add(&b, "{\"busy\":%s,\"done\":%s,\"ok\":%s,\"percent\":%d,\"kind\":",
           st.busy ? "true" : "false", st.done ? "true" : "false", st.ok ? "true" : "false", st.percent);
    sb_str(&b, st.kind);
    sb_add(&b, ",\"device\":");
    sb_str(&b, st.device);
    sb_add(&b, ",\"step\":");
    sb_str(&b, st.step);
    sb_add(&b, ",\"message\":");
    sb_str(&b, st.message);
    sb_add(&b, ",\"result\":%s}", st.result[0] ? st.result : "null");
    pthread_mutex_unlock(&lock);
    respond_json(c, 200, &b);
    free(b.p);
}

/* ------------------------------------------------------------ Aufträge annehmen */

/* Zuordnungseinheit (Bytes, 0 = Standard): Zweierpotenz, mindestens eine Sektorgröße; ext4 kennt nur
 * Blöcke von 1, 2 und 4 KB */
static int cluster_valid(int fs, uint32_t c, uint32_t ss) {
    if (!c) return 1;
    if (c & (c - 1) || c < ss) return 0;
    switch (fs) {
    case FS_EXFAT: return c <= 32u * 1024 * 1024;
    case FS_FAT32:
    case FS_NTFS:  return c <= 65536;
    case FS_EXT4:  return c >= 1024 && c <= 4096;
    }
    return 0;
}

/* Gerät aus device + size der Anfrage (die Größe schützt davor, einen inzwischen getauschten Stick zu erwischen).
 * Gesperrte nie. 0 = gefunden; sonst ist die Fehlerantwort schon gesendet. */
static int request_device(int c, kv_t *kv, int n, usbdev_t *out) {
    const char *dev = kv_get(kv, n, "device"), *size = kv_get(kv, n, "size");
    if (!dev || !size) { respond_error(c, 400, "device und size sind erforderlich"); return -1; }
    usbdev_t devs[MAX_DEVS];
    int nd = plat_scan(devs, MAX_DEVS);
    for (int i = 0; i < nd; i++) {
        if (strcmp(devs[i].name, dev) != 0) continue;
        if (strtoull(size, NULL, 10) != devs[i].size) {
            respond_error(c, 409, "Gerätegröße hat sich geändert (anderer Stick?). Liste neu laden.");
            return -1;
        }
        if (devs[i].locked) {
            respond_error(c, 403, "Dieses Gerät ist vom System eingehängt und wird nicht verändert.");
            return -1;
        }
        *out = devs[i];
        return 0;
    }
    respond_error(c, 404, "Gerät nicht gefunden. Liste neu laden.");
    return -1;
}

/* Eingebunden: schreiben nur mit "replug":true - dann meldet der Formatierer den Stick per Software ab (oder der
 * Benutzer zieht ihn ab) und übernimmt ihn exklusiv, bevor die PS5 ihn wieder einbindet (plat_wait_replug) */
static int request_replug(int c, kv_t *kv, int n, const usbdev_t *d, int *replug) {
    *replug = d->nmounts > 0 && kv_true(kv, n, "replug");
    if (d->nmounts > 0 && !*replug) {
        respond_error(c, 409, "Die PS5 hat diesen Stick eingebunden. Mit \"replug\":true senden - der Formatierer "
                              "meldet ihn dann kurz ab.");
        return -1;
    }
    return 0;
}

static void start_job(int c, job_t *j, void *(*fn)(void *)) {
    pthread_mutex_lock(&lock);
    if (st.busy) {
        pthread_mutex_unlock(&lock);
        free(j);
        respond_error(c, 409, "Es läuft bereits ein Auftrag");
        return;
    }
    memset(&st, 0, sizeof(st));
    st.busy = 1;
    snprintf(st.kind, sizeof(st.kind), "%s", JOB_KIND[j->kind]);
    snprintf(st.device, sizeof(st.device), "%s", j->dev.name);
    snprintf(st.step, sizeof(st.step), "Startet");
    cancel_requested = 0;
    pthread_mutex_unlock(&lock);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, JOB_STACK);
    int trc = pthread_create(&t, &attr, fn, j);
    pthread_attr_destroy(&attr);
    if (trc != 0) {
        free(j);
        pthread_mutex_lock(&lock);
        st.busy = 0;
        pthread_mutex_unlock(&lock);
        respond_error(c, 500, "Thread konnte nicht gestartet werden");
        return;
    }
    pthread_detach(t);
    respond_ok(c, 202);
}

static job_t *new_job(int c, int kind) {
    job_t *j = calloc(1, sizeof(*j));
    if (!j) respond_error(c, 500, "Kein Speicher");
    else j->kind = kind;
    return j;
}

static void api_format(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }

    const char *fs = kv_get(kv, n, "fs"), *label = kv_get(kv, n, "label"), *cl = kv_get(kv, n, "cluster");
    const char *scheme = kv_get(kv, n, "scheme"), *limit = kv_get(kv, n, "limit");
    if (!fs) { respond_error(c, 400, "fs ist erforderlich"); return; }
    if (!label) label = "";
    int fstype = !strcmp(fs, "exfat") ? FS_EXFAT : !strcmp(fs, "fat32") ? FS_FAT32
               : !strcmp(fs, "ntfs") ? FS_NTFS : !strcmp(fs, "ext4") ? FS_EXT4 : 0;
    if (!fstype) { respond_error(c, 400, "Dateisystem muss exfat, fat32, ntfs oder ext4 sein"); return; }
    if (!label_valid(label, fstype)) {
        char m[160];
        snprintf(m, sizeof(m), "Ungültige Bezeichnung: max. %d Zeichen, nur A-Z, 0-9, Leerzeichen, - und _",
                 label_max(fstype));
        respond_error(c, 400, m);
        return;
    }
    int sch = !scheme || !strcmp(scheme, "auto") ? LAYOUT_AUTO : !strcmp(scheme, "mbr") ? LAYOUT_MBR
            : !strcmp(scheme, "gpt") ? LAYOUT_GPT : -1;
    if (sch < 0) { respond_error(c, 400, "scheme muss auto, mbr oder gpt sein"); return; }
    uint32_t cluster = cl ? (uint32_t)strtoul(cl, NULL, 10) : 0;

    usbdev_t d;
    int replug;
    if (request_device(c, kv, n, &d) || request_replug(c, kv, n, &d, &replug)) return;
    if (!cluster_valid(fstype, cluster, d.ss)) {
        respond_error(c, 400, "Diese Zuordnungseinheit passt nicht zum gewählten Dateisystem");
        return;
    }
    uint64_t lim = limit ? strtoull(limit, NULL, 10) : 0;
    if (lim && (lim >= d.size || lim < 64 * MIB)) {
        respond_error(c, 400, "limit muss zwischen 64 MB und der Größe des Datenträgers liegen");
        return;
    }

    job_t *j = new_job(c, JOB_FORMAT);
    if (!j) return;
    j->dev = d;
    j->fs = fstype;
    j->cluster = cluster;
    j->replug = replug;
    j->verify = kv_true(kv, n, "verify");
    j->full = kv_true(kv, n, "full");
    j->folders = kv_true(kv, n, "folders");
    j->scheme = sch;
    j->limit = lim;
    memcpy(j->label, label, strlen(label));  /* label_valid: max. 32 Zeichen */
    start_job(c, j, format_worker);
}

static void api_speed(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    usbdev_t d;
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }
    if (request_device(c, kv, n, &d)) return;
    job_t *j = new_job(c, JOB_SPEED);
    if (!j) return;
    j->dev = d;
    start_job(c, j, speed_worker);
}

static void api_check(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX), replug;
    usbdev_t d;
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }
    if (request_device(c, kv, n, &d) || request_replug(c, kv, n, &d, &replug)) return;
    if (strcmp(d.format, "exFAT") != 0) {
        respond_error(c, 400, "Prüfen und Reparieren geht nur bei exFAT");
        return;
    }
    job_t *j = new_job(c, JOB_CHECK);
    if (!j) return;
    j->dev = d;
    j->replug = replug;
    j->repair = kv_true(kv, n, "repair");
    start_job(c, j, check_worker);
}

static void api_label(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX), replug;
    usbdev_t d;
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }
    const char *label = kv_get(kv, n, "label");
    if (!label) { respond_error(c, 400, "label ist erforderlich"); return; }
    if (request_device(c, kv, n, &d) || request_replug(c, kv, n, &d, &replug)) return;
    int fs = !strcmp(d.format, "exFAT") ? FS_EXFAT : !strcmp(d.format, "FAT32") ? FS_FAT32 : 0;
    if (!fs) { respond_error(c, 400, "Umbenennen geht nur bei exFAT und FAT32"); return; }
    if (!label_valid(label, fs)) {
        respond_error(c, 400, "Ungültige Bezeichnung: max. 11 Zeichen, nur A-Z, 0-9, Leerzeichen, - und _");
        return;
    }
    job_t *j = new_job(c, JOB_LABEL);
    if (!j) return;
    j->dev = d;
    j->replug = replug;
    memcpy(j->label, label, strlen(label));
    start_job(c, j, label_worker);
}

/* ------------------------------------------------------------ Spiele löschen */

/* Fehlertext einer ShadowMountPlus-Antwort ({"status":<errno>,"error":"..."}) */
static void smp_error(const char *resp, size_t len, int http, char *out, size_t n) {
    char e[160] = "";
    if (resp) lib_json_str(resp, len, "error", e, sizeof(e));
    if (http == 409) snprintf(out, n, "ShadowMountPlus ist beschäftigt oder das Spiel läuft gerade – Spiel beenden und "
                                     "erneut versuchen");
    else if (http == 400 || http == 501) snprintf(out, n, "ShadowMountPlus kann das nicht (ab Version 1.7 nötig)%s%s",
                                                  e[0] ? ": " : "", e);
    else snprintf(out, n, "ShadowMountPlus meldet Fehler %d%s%s", http, e[0] ? ": " : "", e);
}

/* Löschen über ShadowMountPlus: Auftrag starten und bis zum Ende verfolgen (es hält dabei Scanner und Startseite an
 * und gibt eingebundene Abbilder selbst frei). 0 = gelöscht. */
static int smp_delete(const del_item_t *it, int base, int span, char *err, size_t n) {
    char body[96], resp_err[200];
    size_t len;
    int http;
    long long job = 0, v;
    snprintf(body, sizeof(body), "{\"title_id\":\"%s\",\"confirm\":true}", it->id);
    char *r = plat_smp_post("/api/v1/games/delete", body, &http, &len);
    if (!r) { snprintf(err, n, "ShadowMountPlus nicht erreichbar"); return -1; }
    if (http != 202 || lib_json_num(r, len, "job_id", &job) != 0) {
        smp_error(r, len, http, err, n);
        free(r);
        return -1;
    }
    free(r);
    snprintf(body, sizeof(body), "{\"job_id\":%lld}", job);
    for (int tries = 0; tries < 7200; tries++) {   /* höchstens eine Stunde */
        usleep(500 * 1000);
        if (!(r = plat_smp_post("/api/v1/games/storage/status", body, &http, &len))) continue;
        char state[24] = "";
        lib_json_str(r, len, "state", state, sizeof(state));
        if (lib_json_num(r, len, "progress_percent", &v) == 0 && v >= 0 && v <= 100)
            on_progress("Spiele löschen", base + (int)(v * span / 100));
        if (!strcmp(state, "completed")) { free(r); return 0; }
        if (!strcmp(state, "failed") || !strcmp(state, "cancelled") || http != 200) {
            resp_err[0] = 0;
            lib_json_str(r, len, "result_error", resp_err, sizeof(resp_err));
            snprintf(err, n, "ShadowMountPlus: %s", resp_err[0] ? resp_err : state[0] ? state : "Fehler");
            free(r);
            return -1;
        }
        free(r);
    }
    snprintf(err, n, "ShadowMountPlus antwortet nicht mehr");
    return -1;
}

/* Rekursiv löschen (nur Kopien, die ShadowMountPlus nicht verwaltet, z. B. doppelte) */
static int remove_tree(const char *path, int depth) {
    struct stat sb;
    if (depth > 32 || lstat(path, &sb) != 0) return -1;
    if (!S_ISDIR(sb.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    char *sub = malloc(1024);
    while (sub && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, 1024, "%s/%s", path, e->d_name) >= 1024 || remove_tree(sub, depth + 1) != 0) rc = -1;
    }
    free(sub);
    closedir(d);
    return rc == 0 ? rmdir(path) : -1;
}

static void *delete_worker(void *arg) {
    job_t *j = arg;
    sbuf_t msg = {0}, res = {0}, done_names = {0}, errs = {0};
    char err[256];
    int ok_n = 0, fail_n = 0, tiles = 0;

    /* Wer verwaltet welchen Pfad? Nur die Kopie, die ShadowMountPlus selbst nutzt, löscht es auch */
    lib_hint_t *smp = NULL;
    int nsmp = 0, http;
    size_t len;
    char *g = plat_smp_post("/api/v1/games", "{}", &http, &len);
    int smp_ok = g && http == 200 && lib_parse_smp_games(g, len, &smp, &nsmp) == 0;
    free(g);

    sb_add(&res, "{\"items\":[");
    for (int i = 0; i < j->nitems; i++) {
        const del_item_t *it = &j->items[i];
        int base = i * 100 / j->nitems, span = 100 / j->nitems, managed = 0, known = 0, rc;
        on_progress("Spiele löschen", base);
        for (int k = 0; k < nsmp; k++)
            if (!strcmp(smp[k].id, it->id)) { known = 1; managed |= !strcmp(smp[k].path, it->path); }
        err[0] = 0;
        if (managed) {
            rc = smp_delete(it, base, span, err, sizeof(err));
            if (rc == 0 && j->uninstall) {   /* Kachel entfernen; Quelle ist weg, ein neuer Suchlauf holt sie nicht zurück */
                char body[64];
                snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", it->id);
                char *r = plat_smp_post("/api/v1/games/uninstall", body, &http, &len);
                if (r && http == 200) tiles++;
                else if (!r || http != 404) {
                    char e2[200];
                    smp_error(r, len, http, e2, sizeof(e2));
                    sb_add(&errs, "%s%s: gelöscht, aber Kachel nicht entfernt (%s)", errs.len ? "; " : "", it->name, e2);
                }
                free(r);
            }
        } else if (!smp_ok) {
            rc = -1;
            snprintf(err, sizeof(err), "ShadowMountPlus nicht erreichbar – zum Schutz eingebundener Spiele nichts "
                                       "gelöscht");
        } else {
            /* nicht von ShadowMountPlus genutzt (z. B. zweite Kopie): selbst löschen - nur innerhalb der Wurzel des
             * USB-Laufwerks, auf dem die Suche es fand (api_delete übernimmt nur Spiele von USB-Laufwerken) */
            size_t rl = strlen(it->root);
            if (!rl || strncmp(it->path, it->root, rl) != 0 || it->path[rl] != '/' || strstr(it->path, "/../")) {
                rc = -1;
                snprintf(err, sizeof(err), "Pfad liegt nicht auf dem USB-Laufwerk");
            } else if ((rc = remove_tree(it->path, 0)) != 0) {
                snprintf(err, sizeof(err), "Löschen fehlgeschlagen: %s", strerror(errno));
            }
            (void)known;
        }
        sb_add(&res, "%s{\"id\":", i ? "," : "");
        sb_str(&res, it->id);
        sb_add(&res, ",\"name\":");
        sb_str(&res, it->name);
        sb_add(&res, ",\"ok\":%s,\"error\":", rc == 0 ? "true" : "false");
        sb_str(&res, err);
        sb_add(&res, "}");
        if (rc == 0) { ok_n++; sb_add(&done_names, "%s%s", done_names.len ? ", " : "", it->name); }
        else { fail_n++; sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, err); }
    }
    sb_add(&res, "],\"deleted\":%d,\"failed\":%d,\"tiles\":%d}", ok_n, fail_n, tiles);
    free(smp);

    if (ok_n) sb_add(&msg, "%d Spiel%s gelöscht: %s.", ok_n, ok_n == 1 ? "" : "e", done_names.p);
    if (tiles) sb_add(&msg, " %d Kachel%s von der Startseite entfernt.", tiles, tiles == 1 ? "" : "n");
    if (errs.len) sb_add(&msg, "%s%s.", ok_n ? " " : "Nicht gelöscht – ", errs.p);
    free(done_names.p);
    free(errs.p);
    free(j->items);
    job_done(j, fail_n == 0, &msg, &res, 1);
    return NULL;
}

/* {"keys":"1a2b3c4d,5e6f7a8b","uninstall":true} - Schlüssel aus /api/library; nur Spiele auf USB-Laufwerken */
static void api_delete(int c, const char *body) {
    char keys[1024] = "";
    const char *k = strstr(body, "\"keys\"");
    if (k) {
        k = ws(k + 6);
        if (*k == ':') json_string(ws(k + 1), keys, sizeof(keys));
    }
    if (!keys[0]) { respond_error(c, 400, "keys ist erforderlich"); return; }
    const char *u = strstr(body, "\"uninstall\"");
    int uninstall = 0;
    if (u) { u = ws(u + 11); if (*u == ':' && !strncmp(ws(u + 1), "true", 4)) uninstall = 1; }

    job_t *j = new_job(c, JOB_DELETE);
    if (!j) return;
    j->uninstall = uninstall;
    j->items = calloc(64, sizeof(*j->items));
    if (!j->items) { free(j); respond_error(c, 500, "Kein Speicher"); return; }
    pthread_mutex_lock(&lib_lock);
    for (char *p = keys, *save = NULL, *tok; (tok = strtok_r(p, ",", &save)); p = NULL) {
        uint32_t want = (uint32_t)strtoul(tok, NULL, 16);
        for (int i = 0; i < lib_n && j->nitems < 64; i++) {
            if (!lib_snap[i].usb || !lib_snap[i].device[0]) continue;
            for (int t = 0; t < lib_snap[i].r.ntitles; t++) {
                const lib_title_t *tt = &lib_snap[i].r.titles[t];
                if (path_key(tt->path) != want) continue;
                int dup = 0;
                for (int d = 0; d < j->nitems; d++) dup |= !strcmp(j->items[d].path, tt->path);
                if (dup) continue;
                del_item_t *it = &j->items[j->nitems++];
                snprintf(it->id, sizeof(it->id), "%s", tt->id);
                snprintf(it->name, sizeof(it->name), "%s", tt->name[0] ? tt->name : tt->id);
                snprintf(it->path, sizeof(it->path), "%s", tt->path);
                snprintf(it->root, sizeof(it->root), "%s", lib_snap[i].r.root);
                snprintf(it->device, sizeof(it->device), "%s", lib_snap[i].device);
            }
        }
    }
    pthread_mutex_unlock(&lib_lock);
    if (!j->nitems) {
        free(j->items);
        free(j);
        respond_error(c, 404, "Spiele nicht gefunden – Liste neu laden");
        return;
    }
    snprintf(j->dev.name, sizeof(j->dev.name), "%s", j->items[0].device);
    start_job(c, j, delete_worker);
}

static void handle(int c) {
    char *req = malloc(REQ_MAX + 1);
    size_t len = 0;
    char *body = NULL;
    long clen = 0;

    if (!req) return;
    /* Header lesen */
    while (len < REQ_MAX) {
        ssize_t n = recv(c, req + len, REQ_MAX - len, 0);
        if (n <= 0) goto out;
        len += n;
        req[len] = 0;
        if ((body = strstr(req, "\r\n\r\n"))) break;
    }
    if (!body) { respond_error(c, 400, "Anfrage zu groß"); goto out; }
    *body = 0;
    body += 4;

    char method[8], path[128], host[96] = "", tmp[32];
    if (sscanf(req, "%7s %127s", method, path) != 2) { respond_error(c, 400, "Ungültige Anfrage"); goto out; }
    if (!header(req, "Host", host, sizeof(host)) || !host_ok(host)) {
        respond_error(c, 403, "Ungültiger Host. Bitte die PS5 über ihre IP-Adresse aufrufen.");
        goto out;
    }
    if (header(req, "Content-Length", tmp, sizeof(tmp))) clen = strtol(tmp, NULL, 10);
    if (clen < 0 || clen > BODY_MAX || (size_t)(body - req) + (size_t)clen > REQ_MAX) {
        respond_error(c, 400, "Anfrage zu groß");
        goto out;
    }

    /* Rest des Bodys lesen */
    size_t have = len - (size_t)(body - req);
    while ((long)have < clen) {
        ssize_t n = recv(c, body + have, (size_t)clen - have, 0);
        if (n <= 0) goto out;
        have += n;
    }
    body[clen] = 0;

    char *q = strchr(path, '?');
    if (q) *q++ = 0;
    int is_get = strcmp(method, "GET") == 0, is_post = strcmp(method, "POST") == 0;

    if (is_post) {
        if (!origin_ok(req, host)) { respond_error(c, 403, "Fremder Ursprung abgelehnt"); goto out; }
        if (!header(req, "Content-Type", tmp, sizeof(tmp)) || strncasecmp(tmp, "application/json", 16) != 0) {
            respond_error(c, 415, "Content-Type muss application/json sein");
            goto out;
        }
    }

    if (strcmp(path, "/") == 0 && is_get)
        respond(c, 200, "text/html; charset=utf-8", web_index_html, strlen(web_index_html));
    else if (strcmp(path, "/bg.jpg") == 0 && is_get)
        respond_ex(c, 200, "image/jpeg", web_bg_jpg, (size_t)(web_bg_jpg_end - web_bg_jpg), "max-age=86400");
    else if (strcmp(path, "/page-dark.jpg") == 0 && is_get)
        respond_ex(c, 200, "image/jpeg", web_page_dark, (size_t)(web_page_dark_end - web_page_dark), "max-age=86400");
    else if (strcmp(path, "/page-light.jpg") == 0 && is_get)
        respond_ex(c, 200, "image/jpeg", web_page_light, (size_t)(web_page_light_end - web_page_light), "max-age=86400");
    else if (strcmp(path, "/api/devices") == 0 && is_get)
        api_devices(c);
    else if (strcmp(path, "/api/library") == 0 && is_get)
        api_library(c, q && strstr(q, "refresh=1") != NULL);
    else if (strcmp(path, "/api/status") == 0 && is_get)
        api_status(c);
    else if (strcmp(path, "/api/info") == 0 && is_get)
        api_info(c);
    else if (strcmp(path, "/api/open") == 0 && is_post)
        api_open(c);
    else if (strcmp(path, "/api/settings") == 0 && (is_get || is_post))
        api_settings(c, is_post, body);
    else if (strcmp(path, "/api/diag") == 0 && (is_get || is_post))
        api_diag(c, is_post, body);
    else if (strcmp(path, "/api/format") == 0 && is_post)
        api_format(c, body);
    else if (strcmp(path, "/api/speed") == 0 && is_post)
        api_speed(c, body);
    else if (strcmp(path, "/api/check") == 0 && is_post)
        api_check(c, body);
    else if (strcmp(path, "/api/label") == 0 && is_post)
        api_label(c, body);
    else if (strcmp(path, "/api/delete") == 0 && is_post)
        api_delete(c, body);
    else if (strcmp(path, "/api/cancel") == 0 && is_post) {
        /* Warten auf Abziehen/Einstecken, Prüfen und Beschreiben brechen ab; das Formatieren selbst nicht */
        cancel_requested = 1;
        respond_ok(c, 200);
    }
    else if (strcmp(path, "/api/quit") == 0 && is_post) {
        pthread_mutex_lock(&lock);
        int busy = st.busy;
        pthread_mutex_unlock(&lock);
        if (busy) respond_error(c, 409, "Es läuft noch ein Auftrag");
        else {
            respond_ok(c, 200);
            quit_requested = 1;
        }
    } else if (strncmp(path, "/api/", 5) == 0 || strcmp(path, "/") == 0)
        respond_error(c, 405, "Methode nicht erlaubt");
    else
        respond_error(c, 404, "Nicht gefunden");
out:
    free(req);
}

int server_listen(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in addr = {0};

    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 8) != 0) {
        close(s);
        return -1;
    }
    settings_load();
    lib_refresh();
    return s;
}

void server_serve(int s) {
    while (!quit_requested) {
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        struct timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle(c);
        close(c);
    }
    close(s);
}
