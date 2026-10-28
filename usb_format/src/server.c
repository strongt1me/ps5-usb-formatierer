/*
 * Minimaler HTTP-Server: liefert die Weboberflaeche und eine JSON-API.
 *   GET  /               Weboberflaeche
 *   GET  /api/devices    USB-Geräte mit Format, Bezeichnung, Partitionstabelle und Belegung der Einbindungen
 *   GET  /api/library    Spiele auf allen Laufwerken (Hintergrundsuche, wie ShadowMountPlus); ?refresh=1 sucht neu
 *                         während der Suche "progress": {done, total, root} (durchsuchte Ordner)
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
 *   GET  /api/images     Orte für Image-Sicherungen (/data und eingebundene USB-Laufwerke, je Ordner usbfmt-backup)
 *                         samt der .img.zst darin
 *   POST /api/backup     {"device","size","ident","replug","dest":"<Ordner aus /api/images>"|"download"} Image sichern;
 *                         "download" liefert {"token","name"} für GET /api/backup/download?token=...
 *   POST /api/restore    {"device","size","ident","replug","path":"<.img.zst aus /api/images>"} Image zurückschreiben
 *   POST /api/restore/upload?device=..&size=..&ident=..&replug=1  Körper: die .img.zst (application/octet-stream)
 *   POST /api/cancel     {} laufenden Auftrag abbrechen (Warten auf Abziehen, Prüfen, Beschreiben)
 *   GET  /api/status     Fortschritt des laufenden/letzten Auftrags: "kind", "job" (laufende Nummer) und "result"
 *   GET  /api/info       LAN-Adresse der PS5 (fuer den Hinweis im PS5-Browser) und "version" des Formatierers
 *   GET  /api/tile       App-Kachel PPSA86920 laut ShadowMountPlus: {smp, found, installed, registered (angemeldete
 *                         contentVersion), folder (contentVersion im App-Ordner), formatter}
 *   POST /api/tile       {} Kachel über ShadowMountPlus neu anmelden (Uninstall + Scan) - Auftrag "tile"
 *   GET  /api/history    {"entries":[{t, kind, ok, cancelled, secs, device, model, size, ident, label, fs, file, count,
 *                         msg}]}
 *                         die letzten 100 Aufträge, neueste zuerst; POST /api/history/clear {} leert den Verlauf
 *   POST /api/open       {} Web-UI im PS5-Browser öffnen (für die App, app/title/starter.c)
 *   GET  /api/settings   {"theme":"auto|light|dark","scale_tv":160,"scale_pc":100,"lang":"de|en|it|es|fr|"};
 *                         POST ändert (einzeln oder zusammen) und speichert es
 *   Sprache der Antworten und Meldungen: ?lang=de|en|it|es|fr an jeder Adresse, sonst Accept-Language, sonst die
 *   gespeicherte Einstellung. Ein Auftrag meldet sich in der Sprache der Anfrage, die ihn gestartet hat.
 *   POST /api/diag       Darstellungsbericht der Seite ablegen; GET liefert {"tv":"...","pc":"..."}
 *   POST /api/quit       Server beenden
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

#include "devtest.h"
#include "exfat_fsck.h"
#include "fsdetect.h"
#include "i18n.h"
#include "imgbak.h"
#include "library.h"
#include "mkfs.h"
#include "platform.h"
#include "saves.h"
#include "sha256.h"
#include "server.h"
#include "vollabel.h"

#define REQ_MAX   65536
#define BODY_MAX  32768   /* Spielstand-Auswahl: bis zu 64 Schlüssel/Pfade in einer Anfrage */
#define JOB_STACK (4 * 1024 * 1024)   /* Threads haben auf der PS5 nur 64 KiB - fsck.exfat braucht mehr */
#define MIB       (1024ull * 1024ull)

/* Weboberflaeche wird beim Kompilieren eingebettet (Pfad relativ zum Projektordner) */
__asm__(".section .rodata\n"
        ".global web_index_html\n"
        "web_index_html:\n"
        ".incbin \"web/index.html\"\n"
        ".byte 0\n"
        ".global web_help_html\n"
        "web_help_html:\n"
        ".incbin \"web/help.html\"\n"
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
        ".global web_avatar_png\n"
        "web_avatar_png:\n"
        ".incbin \"web/avatar.png\"\n"
        ".global web_avatar_png_end\n"
        "web_avatar_png_end:\n"
        ".previous\n");
extern const char web_index_html[], web_help_html[];
extern const char web_bg_jpg[], web_bg_jpg_end[];
extern const char web_page_dark[], web_page_dark_end[], web_page_light[], web_page_light_end[];
extern const char web_avatar_png[], web_avatar_png_end[];   /* Profilbild des Entwicklers (Fußzeile) */

/* ------------------------------------------------------------ JSON-Ausgabe */

typedef struct {
    char  *p;
    size_t len, cap;
} sbuf_t;

static void sb_vadd(sbuf_t *b, const char *fmt, va_list ap0) {
    va_list ap;
    if (!b->p) {
        b->p = malloc(256);
        if (!b->p) return;
        b->cap = 256;
        b->p[0] = 0;
    }
    for (;;) {
        va_copy(ap, ap0);
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

static void sb_add(sbuf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vadd(b, fmt, ap);
    va_end(ap);
}

/* Meldungstext: fmt ist der deutsche Text, angehängt wird er in der Sprache des Threads (i18n.h) */
static void sb_tr(sbuf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vadd(b, T(fmt), ap);
    va_end(ap);
}

/* Länge einer gültigen UTF-8-Folge ab s (1-4), 0 = ungültig (auch einzelne Surrogate und zu lange Folgen) */
static int utf8_len(const unsigned char *s) {
    if (s[0] < 0x80) return 1;
    int n = s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3 : s[0] >= 0xC2 && s[0] <= 0xDF ? 2 : 0;
    if (!n) return 0;
    for (int i = 1; i < n; i++)
        if ((s[i] & 0xC0) != 0x80) return 0;
    if (n == 3 && ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0))) return 0;
    if (n == 4 && ((s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90))) return 0;
    return n;
}

/* JSON-Zeichenkette; ungültiges UTF-8 (FAT-Bezeichnungen in alter Codepage, kaputte Namen in param.json) wird zu
 * U+FFFD - sonst lehnte das PC-Programm die ganze Antwort ab */
static void sb_str(sbuf_t *b, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    sb_add(b, "\"");
    while (*p) {
        int n = utf8_len(p);
        if (!n) { sb_add(b, "\\ufffd"); p++; continue; }
        if (*p == '"' || *p == '\\') sb_add(b, "\\%c", *p);
        else if (*p < 0x20) sb_add(b, "\\u%04x", *p);
        else sb_add(b, "%.*s", n, (const char *)p);
        p += n;
    }
    sb_add(b, "\"");
}

/* "58,6 GB" (1024er-Einheiten wie in der Oberfläche) */
static const char *size_text(uint64_t b, char *buf, size_t n) {
    static const char *const unit[] = {N_("Bytes"), N_("KB"), N_("MB"), N_("GB"), N_("TB")};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    snprintf(buf, n, i ? "%.1f %s" : "%.0f %s", v, T(unit[i]));
    char *dot = strchr(buf, '.');
    if (dot && i18n_get() != LANG_EN) *dot = ',';   /* Dezimalkomma außer im Englischen */
    return buf;
}

/* ------------------------------------------------------------ Zustand und Aufträge */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int  busy, done, ok;
    char kind[16];        /* format, speed, check, label */
    char device[16];
    char step[128];   /* übersetzte Schrittnamen sind teils länger als 64 Bytes (Französisch) */
    int  percent;
    char message[512];
    char result[4096];    /* JSON-Objekt mit Einzelheiten oder leer */
    int  cancellable;     /* "Abbrechen" möglich (Warten aufs Umstecken, Prüfung, Tempomessung) */
    int  replug;          /* wartet auf Abziehen/Einstecken bzw. Software-Neuanmeldung des Sticks */
    int  cancelled;       /* Auftrag endete, weil "Abbrechen" gewählt wurde */
    char detail[160];     /* Zeile unter dem Balken: "12,3 GB von 58,6 GB · noch etwa 4 Min." (leer, solange nichts zu schätzen ist) */
    int  last_pct;        /* vorheriger Fortschrittswert (Rücksprung = neue Messung) */
    double step_t0;       /* Start dieses Schritts (Sekunden, monoton) */
    int  step_p0;         /* Fortschritt beim Start dieses Schritts */
    const uint64_t *total_ptr;   /* Gesamtbytes des laufenden Schritts (Bezug des Prozentwerts), NULL = unbekannt */
} st;

static unsigned job_seq;   /* zählt angenommene Aufträge (unter lock) - Download/Hochladen starten erst mit der Verbindung */
static volatile int quit_requested;
static int quitting;   /* /api/quit angenommen: keine neuen Aufträge mehr (unter lock) */
static volatile int cancel_requested;   /* POST /api/cancel */
static volatile int cancel_keep;        /* POST /api/cancel {"keep":true}: Teilsicherung einer Image-Sicherung behalten */

static void lib_refresh(void);
static uint64_t fake_get(const usbdev_t *d);
static void fake_put(const usbdev_t *d, uint64_t real);

/* step: deutscher Schrittname oder schon übersetzt (mkfs/devtest übersetzen selbst) */
static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Restzeit grob in Worten: "4 Min.", "1 Std. 5 Min." */
static const char *eta_text(long secs, char *buf, size_t n) {
    if (secs < 60) snprintf(buf, n, T("%ld Sek."), secs < 5 ? 5 : (secs + 4) / 5 * 5);
    else if (secs < 3600) snprintf(buf, n, T("%ld Min."), (secs + 30) / 60);
    else snprintf(buf, n, T("%ld Std. %ld Min."), secs / 3600, (secs % 3600 + 30) / 60);
    return buf;
}

/* Zeile unter dem Balken (unter lock): bearbeitete Menge und geschätzte Restzeit aus dem bisherigen Tempo dieses Schritts */
static void update_detail(int pct) {
    const double now = mono_s();
    if (!st.step_t0 || pct < st.step_p0 || pct < st.last_pct) {   /* Messung beginnt mit dem Auftrag und neu, wenn der Balken von vorn anfängt */
        st.step_t0 = now;
        st.step_p0 = pct;
    }
    st.last_pct = pct;
    st.detail[0] = 0;
    if (pct <= 0 || pct >= 100) return;
    char a[32], b[32], eta[40], part[64] = "";
    size_t at = 0;
    if (st.total_ptr && *st.total_ptr)
        at = (size_t)snprintf(st.detail, sizeof(st.detail), T("%s von %s"), size_text((uint64_t)((double)*st.total_ptr * pct / 100), a, sizeof(a)),
                              size_text(*st.total_ptr, b, sizeof(b)));
    const double el = now - st.step_t0;
    const int dp = pct - st.step_p0;
    if (dp >= 2 && el >= 8) {   /* erst nach einer brauchbaren Messung schätzen */
        char rate[40] = "";
        if (st.total_ptr && *st.total_ptr) {   /* Tempo aus der bisher bearbeiteten Menge dieses Schritts */
            char sp[24];
            size_text((uint64_t)((double)*st.total_ptr * dp / 100 / el), sp, sizeof(sp));
            snprintf(rate, sizeof(rate), "%s/s · ", sp);
        }
        snprintf(part, sizeof(part), T("noch etwa %s"), eta_text((long)(el * (100 - pct) / dp), eta, sizeof(eta)));
        snprintf(st.detail + at, sizeof(st.detail) - at, "%s%s%s", at ? " · " : "", rate, part);
    }
}

static void on_progress(const char *step, int pct) {
    step = T(step);
    pthread_mutex_lock(&lock);
    snprintf(st.step, sizeof(st.step), "%s", step);
    st.percent = pct;
    update_detail(pct);
    pthread_mutex_unlock(&lock);
    plat_throttle();
}

/* Kennzeichen für die Oberflächen - die Schrittnamen sind übersetzt und taugen nicht mehr zum Erkennen */
static void set_flags(int cancellable, int replug) {
    pthread_mutex_lock(&lock);
    st.cancellable = cancellable;
    st.replug = replug;
    pthread_mutex_unlock(&lock);
}

enum { JOB_FORMAT = 1, JOB_SPEED, JOB_CHECK, JOB_LABEL, JOB_DELETE, JOB_BACKUP, JOB_RESTORE, JOB_TILE, JOB_VERIFY,
       JOB_SAVE_RESTORE, JOB_SAVE_BACKUP, JOB_MOVE, JOB_PKGDEL, JOB_GVERIFY, JOB_GSUM, JOB_PARTDEL };
static const char *const JOB_KIND[] = {"", "format", "speed", "check", "label", "delete", "backup", "restore",
                                       "tile", "verify", "save_restore", "save_backup", "move", "pkg_delete", "game_verify", "game_checksum", "partial_discard"};

#define LIB_EXTRAS_MAX 8

/* Spiel zum Löschen (aus der Bibliothek übernommen) */
typedef struct {
    char id[16], name[128], path[256], root[96], device[16];
    uint64_t size;   /* Abbild: Größe laut Suche */
    dev_t    dev;    /* st_dev der Wurzel laut Suche: noch derselbe eingehängte Datenträger? */
    int      nex;
    char     ex[LIB_EXTRAS_MAX][300];   /* zugehörige Dateien/Ordner (find_extras) */
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
    int      extras;             /* zugehörige Dateien (Backport, Prüfsummen) mitkopieren bzw. mitlöschen */
    dev_t    tdev;               /* Verschieben: st_dev der Ziel-Wurzel bei der Auswahl */
    /* Image sichern (path: Zielordner) bzw. wiederherstellen (path: Image-Datei, schon geöffnet in img/rd) */
    char     path[512];
    FILE    *img;
    ib_reader_t *rd;
    char    *saves_list; /* Spielstände: Schlüssel bzw. Pfade, getrennt durch Zeilenumbruch */
    char     resume_part[720];   /* Image-Sicherung fortsetzen: die .part-Datei */
    int      thorough;           /* ... mit gründlicher Prüfung der Quelle */
    int      pkgmode;            /* Verschieben/Löschen betrifft .pkg-Dateien statt Spiele */
    int      sumgen;             /* Spiele prüfen: Prüfsummen-Dateien anlegen statt vergleichen */
    char     saves_dest[300];
    int      lang;       /* Sprache der Anfrage, die den Auftrag gestartet hat */
    struct timespec t0;  /* Start (für die Dauer im Verlauf) */
} job_t;

static void job_free(job_t *j) {
    if (!j) return;
    free(j->items);
    free(j->saves_list);
    ib_close(j->rd);
    if (j->img) fclose(j->img);
    free(j);
}

static const char *const TEMPLATE_DIRS[] = {"homebrew", "etaHEN/games", "pkg", NULL};

static void finish(int ok, const char *msg, const char *result) {
    pthread_mutex_lock(&lock);
    st.busy = 0;
    st.done = 1;
    st.ok = ok;
    st.percent = ok ? 100 : st.percent;
    st.detail[0] = 0;   /* keine Restzeit mehr, wenn nichts mehr läuft */
    st.cancellable = st.replug = 0;
    st.cancelled = !ok && __atomic_load_n(&cancel_requested, __ATOMIC_RELAXED);
    snprintf(st.step, sizeof(st.step), "%s", ok ? T("Fertig") : T("Fehler"));
    snprintf(st.message, sizeof(st.message), "%s", msg ? msg : "");
    if (msg && strlen(msg) >= sizeof(st.message)) {   /* gekürzt: kein halbes UTF-8-Zeichen am Ende, dafür "…" */
        size_t n = sizeof(st.message) - 4;
        while (n && ((unsigned char)st.message[n] & 0xC0) == 0x80) n--;
        memcpy(st.message + n, "…", 4);
    }
    /* abgeschnittenes JSON wäre ungültig - dann lieber keine Einzelheiten */
    if (result && strlen(result) < sizeof(st.result)) memcpy(st.result, result, strlen(result) + 1);
    else st.result[0] = 0;
    pthread_mutex_unlock(&lock);
    plat_notify(msg ? msg : "");
}

/* "Modell (daN)" für Meldungen */
static const char *dev_title(const usbdev_t *d, char *buf, size_t n) {
    if (d->model[0]) snprintf(buf, n, "%s (%s)", d->model, d->name);
    else snprintf(buf, n, "%s", d->name);
    return buf;
}

/* Läuft ein Spiel oder eine App (PPSA/CUSA-Sandbox, außer der Kachel des Formatierers)? ShadowMountPlus spiegelt
 * eingebundene USB-Laufwerke in deren Sandbox - ein Laufwerk dann abzumelden brachte den AutoMounter durcheinander,
 * das Spiel ließ sich danach nicht mehr beenden (Systemfehler, 2026-10-17). Liefert 1 und die Titel-ID. */
static int game_running(char *id, size_t n) {
    const char *dir = plat_sandbox_dir();
    DIR *d = dir ? opendir(dir) : NULL;
    struct dirent *e;
    int found = 0;
    while (d && !found && (e = readdir(d))) {
        if ((!strncmp(e->d_name, "PPSA", 4) || !strncmp(e->d_name, "CUSA", 4)) && strncmp(e->d_name, "PPSA86920", 9)) {
            snprintf(id, n, "%.9s", e->d_name);
            found = 1;
        }
    }
    if (d) closedir(d);
    return found;
}

static void game_error(const char *id, char *err, size_t n) {
    snprintf(err, n, T("Es läuft gerade ein Spiel oder eine App (%s) – bitte zuerst beenden. Die PS5 nutzt eingebundene "
                       "Laufwerke darin mit; sie jetzt abzumelden kann das Spiel und die PS5 durcheinanderbringen."), id);
}

/* Nach einem Neu-Anmelden den Stick frühestens HOLD_MS später wieder freigeben: Gab der Formatierer ihn binnen etwa
 * einer Sekunde zurück (exFAT prüfen, Abbrechen), war der AutoMounter der PS5 noch mit dem Neu-Anmelden beschäftigt und
 * brachte die Einbindung durcheinander ("deviceId ... is not reserved", 2026-10-17 zweimal, nur bei so kurzen Aufträgen) */
#define HOLD_MS 5000
static struct timespec acq_at;   /* Zeitpunkt der Übernahme (ein Auftrag zur Zeit) */
static int acq_replug;

static void job_release(int fd) {
    if (acq_replug) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (long)(now.tv_sec - acq_at.tv_sec) * 1000 + (now.tv_nsec - acq_at.tv_nsec) / 1000000;
        if (ms >= 0 && ms < HOLD_MS) usleep((useconds_t)(HOLD_MS - ms) * 1000);
        acq_replug = 0;
    }
    close(fd);
}

/* Datenträger zum Schreiben übernehmen: eingebunden per Software-Abziehen (bzw. Abziehen von Hand), sonst
 * direkt exklusiv. Liefert den fd oder -1 mit Fehlertext. */
static int job_acquire(job_t *j, char *err, size_t n) {
    on_progress("Laufwerk öffnen", 0);
    acq_replug = 0;
    if (!j->replug) return plat_open_rw(&j->dev, err, n);
    char gid[16];
    if (game_running(gid, sizeof(gid))) {   /* seit der Anfrage gestartet? Unmittelbar vor dem Abmelden noch einmal */
        game_error(gid, err, n);
        return -1;
    }
    usbdev_t fresh;
    set_flags(1, 1);   /* Warten aufs Umstecken: abbrechbar */
    int fd = plat_wait_replug(&j->dev, &fresh, on_progress, &cancel_requested, err, n);
    set_flags(0, 0);
    if (fd >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &acq_at);
        acq_replug = 1;
        if (!fresh.model[0]) snprintf(fresh.model, sizeof(fresh.model), "%s", j->dev.model);
        snprintf(fresh.label, sizeof(fresh.label), "%s", j->dev.label);
        snprintf(fresh.format, sizeof(fresh.format), "%s", j->dev.format);
        j->dev = fresh;   /* kann nach dem Neu-Anmelden einen anderen Namen haben (daN) */
        pthread_mutex_lock(&lock);
        snprintf(st.device, sizeof(st.device), "%s", fresh.name);
        pthread_mutex_unlock(&lock);
    }
    /* "Abbrechen" kam, während der Stick übernommen wurde: freigeben, bevor irgendetwas geschrieben ist */
    if (fd >= 0 && __atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) {
        job_release(fd);
        snprintf(err, n, "%s", T("Abgebrochen – der Datenträger wurde nicht verändert."));
        return -1;
    }
    return fd;
}

static void speed_json(sbuf_t *b, const devtest_t *t) {
    if (t->read_mbps > 0) sb_add(b, ",\"read\":%.1f", t->read_mbps);
    if (t->write_mbps > 0) sb_add(b, ",\"write\":%.1f", t->write_mbps);
    if (t->read_mbps > 0) { sb_add(b, ",\"rating\":"); sb_str(b, dt_rating(t->read_mbps)); }
}

/* Zeile des Prüfberichts nach dem Formatieren: state ok | warn | bad | info */
static void report_row(sbuf_t *b, const char *label, const char *state, const char *fmt, ...) {
    char v[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof(v), fmt, ap);
    va_end(ap);
    sb_add(b, "%s{\"label\":", b->len && b->p[b->len - 1] != '[' ? "," : "");
    sb_str(b, label);
    sb_add(b, ",\"state\":\"%s\",\"value\":", state);
    sb_str(b, v);
    sb_add(b, "}");
}

static const char *rating_text(double read_mbps) {
    const char *r = dt_rating(read_mbps);
    return !strcmp(r, "schnell") ? T("schnell genug für Spiele") : !strcmp(r, "ausreichend")
           ? T("für Spiele ausreichend") : T("langsam – Spiele laden länger");
}

/* ------------------------------------------------------------ Auftragsverlauf
 * Die letzten HIST_MAX Aufträge mit Ergebnis, neueste zuerst, als JSON-Zeilen in <datenordner>/history.jsonl - eine
 * kaputte Zeile kostet nur sich selbst. GET /api/history liefert sie, POST /api/history/clear leert ihn. */
#define HIST_MAX  100
#define HIST_LINE 2048
static pthread_mutex_t hist_lock = PTHREAD_MUTEX_INITIALIZER;

static void history_path(char *p, size_t n) {
    snprintf(p, n, "%s/history.jsonl", plat_data_dir());
}

/* höchstens max Zeilen der Datei (neueste zuerst), je mit malloc; liefert die Anzahl. Aufruf unter hist_lock. */
static int history_read(char **lines, int max) {
    char path[256], buf[HIST_LINE];
    history_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    int n = 0;
    while (f && n < max && fgets(buf, sizeof(buf), f)) {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        if (l < 2 || buf[0] != '{' || buf[l - 1] != '}') continue;   /* angeschnitten oder fremd: auslassen */
        if (!(lines[n] = strdup(buf))) break;
        n++;
    }
    if (f) fclose(f);
    return n;
}

/* neuen Eintrag vorn anfügen - erst vollständig in eine neue Datei, dann umbenennen (wie settings_save) */
static void history_add(const char *entry) {
    char *lines[HIST_MAX], path[256], tmp[264];
    pthread_mutex_lock(&hist_lock);
    int n = history_read(lines, HIST_MAX - 1);
    mkdir(plat_data_dir(), 0755);
    history_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.neu", path);
    FILE *f = fopen(tmp, "w");
    int ok = f && fprintf(f, "%s\n", entry) > 0;
    for (int i = 0; i < n; i++) {
        if (ok && fprintf(f, "%s\n", lines[i]) < 0) ok = 0;
        free(lines[i]);
    }
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) remove(tmp);
    pthread_mutex_unlock(&hist_lock);
}

/* Eintrag zum eben beendeten Auftrag. msg/cancelled kommen vom Aufrufer: nach finish() kann ein neuer Auftrag st schon
 * geleert haben */
static void history_job(const job_t *j, int ok, int cancelled, const char *msg) {
    static const char *const FS_NAME[] = {"", "exFAT", "FAT32", "NTFS", "ext4", "HFS+"};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long secs = (long)(now.tv_sec - j->t0.tv_sec);
    char message[512];
    snprintf(message, sizeof(message), "%s", msg ? msg : "");
    if (msg && strlen(msg) >= sizeof(message)) {   /* gekürzt wie in finish(): kein halbes UTF-8-Zeichen */
        size_t k = sizeof(message) - 4;
        while (k && ((unsigned char)message[k] & 0xC0) == 0x80) k--;
        memcpy(message + k, "…", 4);
    }

    sbuf_t e = {0};
    sb_add(&e, "{\"t\":%lld,\"kind\":", (long long)time(NULL));
    sb_str(&e, JOB_KIND[j->kind]);
    sb_add(&e, ",\"ok\":%s,\"cancelled\":%s,\"secs\":%ld", ok ? "true" : "false", cancelled ? "true" : "false",
           secs > 0 ? secs : 0L);
    if (j->dev.name[0]) {
        sb_add(&e, ",\"device\":");
        sb_str(&e, j->dev.name);
        sb_add(&e, ",\"model\":");
        sb_str(&e, j->dev.model);
        sb_add(&e, ",\"size\":%llu", (unsigned long long)j->dev.size);
        if (j->dev.ident[0]) {   /* erkennt den Stick wieder ("Zuletzt: …" am Laufwerk), auch unter anderem daN */
            sb_add(&e, ",\"ident\":");
            sb_str(&e, j->dev.ident);
        }
    }
    const char *label = j->kind == JOB_FORMAT || j->kind == JOB_LABEL ? j->label : j->dev.label;
    if (label[0]) { sb_add(&e, ",\"label\":"); sb_str(&e, label); }
    if (j->kind == JOB_FORMAT && j->fs > 0 && j->fs <= FS_HFSPLUS) sb_add(&e, ",\"fs\":\"%s\"", FS_NAME[j->fs]);
    if ((j->kind == JOB_RESTORE || j->kind == JOB_VERIFY) && j->path[0]) {
        const char *base = strrchr(j->path, '/');
        sb_add(&e, ",\"file\":");
        sb_str(&e, base ? base + 1 : j->path);
    }
    if (j->kind == JOB_DELETE) sb_add(&e, ",\"count\":%d", j->nitems);
    sb_add(&e, ",\"msg\":");
    sb_str(&e, message);
    sb_add(&e, "}");
    if (e.p && e.len < HIST_LINE - 1) history_add(e.p);
    free(e.p);
}

static void job_done(job_t *j, int ok, sbuf_t *msg, sbuf_t *res, int refresh) {
    int cancelled = !ok && __atomic_load_n(&cancel_requested, __ATOMIC_RELAXED);   /* wie finish() */
    finish(ok, msg->p, res->p);
    history_job(j, ok, cancelled, msg->p);
    free(msg->p);
    free(res->p);
    free(j->saves_list);
    free(j);
    if (refresh) lib_refresh();
}

/* NTFS/ext4: Partition vorbereiten belegt 0-10 %, das externe Werkzeug (mkntfs/mke2fs) den Rest - ohne Rücksprung */
static void ext_progress(const char *step, int pct) { on_progress(step, 10 + pct * 90 / 100); }

static void *format_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    char err[256] = "", summary[160] = "", s1[24], s2[24], title[160];
    sbuf_t msg = {0}, res = {0};
    devtest_t t;
    int tested = 0;
    memset(&t, 0, sizeof(t));

    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    const uint64_t size = j->limit && j->limit < j->dev.size ? j->limit : j->dev.size;

    /* Vorher prüfen: Stichproben (schnell) oder jede Stelle (vollständig). Gefälscht oder defekt: nichts
     * formatieren, die Oberfläche bietet "nur den echten Bereich formatieren" an. Abgebrochen: trotzdem
     * formatieren, wenn die Prüfung schon geschrieben hat (die Daten sind dann ohnehin überschrieben), sonst
     * nichts anfassen. */
    int test_cancelled = 0;
    if (j->verify || j->full) {
        tested = j->full ? 2 : 1;
        set_flags(1, 0);
        int rc = j->full ? dt_full(fd, j->dev.ss, size, on_progress, &cancel_requested, &t, err, sizeof(err))
                         : dt_verify(fd, j->dev.ss, size, on_progress, &cancel_requested, &t, err, sizeof(err));
        set_flags(0, 0);
        if (rc == 0 && t.cancelled && !dt_wrote()) {
            job_release(fd);
            sb_tr(&msg, "Abgebrochen – der Datenträger wurde nicht verändert.");
            job_done(j, 0, &msg, &res, 0);
            return NULL;
        }
        if (rc == 0 && t.cancelled) {
            test_cancelled = 1;
            __atomic_store_n(&cancel_requested, 0, __ATOMIC_RELAXED);
        } else if (rc == 0 && t.fake) {
            fake_put(&j->dev, t.real_bytes);   /* merken: Kennzeichnung und Begrenzung beim nächsten Mal */
        } else if (rc == 0 && !t.bad && !j->limit) {
            fake_put(&j->dev, 0);              /* ganzer Stick geprüft und echt: frühere Kennzeichnung löschen */
        }
        if (!test_cancelled && (rc != 0 || t.fake || t.bad)) {
            /* altes Dateisystem mit teils überschriebenen Dateien nicht stehen lassen: die PS5 bände es sonst mit
             * kaputtem Inhalt wieder ein */
            if (dt_wrote()) dt_scrub(fd, j->dev.size);
            job_release(fd);
            sb_add(&res, "{\"test\":\"%s\",\"fake\":%s,\"bad\":%s,\"reported\":%llu,\"real\":%llu,\"bad_at\":%llu",
                   tested == 2 ? "full" : "quick", t.fake ? "true" : "false", t.bad ? "true" : "false",
                   (unsigned long long)j->dev.size, (unsigned long long)t.real_bytes, (unsigned long long)t.bad_at);
            speed_json(&res, &t);
            sb_add(&res, "}");
            if (rc != 0) {
                sb_tr(&msg, "Prüfung fehlgeschlagen: %s Formatiert wurde noch nicht – die bisherigen Daten wurden bei "
                            "der Prüfung aber schon überschrieben.", err);
            } else if (t.fake) {
                sb_tr(&msg, "%s meldet %s, hat aber nur etwa %s echten Speicher. Formatiert wurde noch nicht – die "
                            "bisherigen Daten wurden bei der Prüfung aber schon überschrieben – %s, höchstens den echten "
                            "Bereich nutzen.", dev_title(&j->dev, title, sizeof(title)),
                      size_text(j->dev.size, s1, sizeof(s1)), size_text(t.real_bytes, s2, sizeof(s2)),
                      T("für Spiele an der PS5 nicht empfohlen"));
            } else {
                sb_tr(&msg, "Fehlerhafte Bereiche ab %llu MB: %s arbeitet nicht zuverlässig. Formatiert wurde noch "
                            "nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben.",
                      (unsigned long long)(t.bad_at / MIB), dev_title(&j->dev, title, sizeof(title)));
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
    fsck_result_t fr;
    int have_fsck = 0;
    if (native) {
        rc = format_disk(&d, j->fs, j->label, arc4random(), j->cluster, &o);
        if (rc == 0 && j->fs == FS_EXFAT) {   /* Nachkontrolle des frisch geschriebenen Dateisystems, solange der Stick noch uns gehört */
            fsinfo_t fi;
            on_progress(N_("Nachkontrolle: Dateisystem prüfen"), 99);
            if (fs_probe(fd, j->dev.ss, j->dev.size, &fi) == 0 && !strcmp(fi.fs, "exFAT")) {
                exfat_fsck_run(fd, fi.start, fi.size ? fi.size : j->dev.size - fi.start, j->dev.ss, 0, &fr);
                have_fsck = 1;
            }
        }
        job_release(fd);
        if (rc == 0) snprintf(summary, sizeof(summary), "%s", mkfs_summary());
        else snprintf(err, sizeof(err), "%s", mkfs_error());
    } else {
        /* NTFS/ext4: Partition selbst anlegen, Dateisystem schreibt das Originalwerkzeug */
        uint64_t start, count;
        rc = prepare_partition(&d, j->fs, j->label, arc4random(), &o, &start, &count);
        job_release(fd);
        if (rc != 0) snprintf(err, sizeof(err), "%s", mkfs_error());
        else rc = plat_external_mkfs(&j->dev, j->fs, j->label, j->cluster, start, count, o.is_gpt, ext_progress,
                                     summary, sizeof(summary), err, sizeof(err));
    }
    if (rc != 0) {
        sb_tr(&msg, "Formatieren von %s fehlgeschlagen: %s", j->dev.name, err);
        job_done(j, 0, &msg, &res, 1);
        return NULL;
    }

    /* auf den echten Bereich begrenzt formatiert: als gefälscht merken, falls noch nicht bekannt */
    if (j->limit && j->limit < j->dev.size && !fake_get(&j->dev)) fake_put(&j->dev, j->limit);
    sb_tr(&msg, "%s erfolgreich formatiert: %s", dev_title(&j->dev, title, sizeof(title)), summary);
    if (j->label[0]) sb_tr(&msg, ", Bezeichnung %s", j->label);
    if (o.is_gpt) sb_tr(&msg, ", Partitionstabelle GPT");
    if (j->limit) sb_tr(&msg, ", nur die echten %s genutzt", size_text(j->limit, s1, sizeof(s1)));
    if (o.dirs) sb_tr(&msg, ", Ordner homebrew, etaHEN/games und pkg angelegt");
    sb_add(&msg, ".");
    if (tested && !test_cancelled && t.read_mbps > 0) {
        sb_tr(&msg, " %s: echt%s, lesen %.0f MB/s, schreiben %.0f MB/s", tested == 2 ? T("Jede Stelle geprüft")
              : T("Prüfung"), tested == 2 ? T(", keine Fehler") : "", t.read_mbps, t.write_mbps);
        sb_add(&msg, " – %s.", rating_text(t.read_mbps));
    }
    if (test_cancelled) sb_tr(&msg, " Die Prüfung wurde abgebrochen – formatiert wurde trotzdem.");
    sb_add(&msg, " %s", native ? T("Die PS5 bindet ihn gleich selbst ein.") : T("Die PS5 kann dieses Format nicht nutzen."));

    /* Prüfbericht: was geprüft und geschrieben wurde, je Punkt mit Ergebnis */
    sbuf_t rep = {0};
    sb_add(&rep, "[");
    if (test_cancelled) report_row(&rep, T("Prüfung vorher"), "warn", "%s", T("abgebrochen – formatiert wurde trotzdem"));
    else if (tested == 2) report_row(&rep, T("Prüfung vorher"), "ok", "%s", T("Jede Stelle geprüft – echt, keine Fehler"));
    else if (tested) report_row(&rep, T("Prüfung vorher"), "ok", "%s", T("Stichproben geprüft – echt, keine Fehler"));
    else report_row(&rep, T("Prüfung vorher"), "info", "%s", T("nicht geprüft"));
    if (tested && !test_cancelled && t.read_mbps > 0)
        report_row(&rep, T("Tempo"), t.read_mbps >= 40 ? "ok" : "warn", T("Lesen %.0f MB/s, Schreiben %.0f MB/s – %s"), t.read_mbps,
                   t.write_mbps, rating_text(t.read_mbps));
    report_row(&rep, T("Dateisystem"), "ok", "%s", summary);
    report_row(&rep, T("Partitionstabelle"), "ok", "%s", o.is_gpt ? "GPT" : "MBR");
    if (have_fsck) {
        char sum[256];
        exfat_fsck_summary(&fr, 0, sum, sizeof(sum));
        report_row(&rep, T("Nachkontrolle"), fr.code == 0 || fr.code == 1 ? "ok" : "bad", "%s", sum);
    }
    if (o.dirs) report_row(&rep, T("Ordner"), "ok", "%s", T("homebrew, etaHEN/games und pkg angelegt"));
    if (j->limit) report_row(&rep, T("Größe"), "warn", T("Nur die echten %s genutzt, der Stick meldet %s"), size_text(j->limit, s1, sizeof(s1)),
                             size_text(j->dev.size, s2, sizeof(s2)));
    else report_row(&rep, T("Größe"), "ok", "%s", size_text(j->dev.size, s1, sizeof(s1)));
    report_row(&rep, T("Einbinden"), native ? "info" : "warn", "%s",
               native ? T("Die PS5 bindet ihn gleich selbst ein.") : T("Die PS5 kann dieses Format nicht nutzen."));
    sb_add(&rep, "]");
    if (have_fsck && fr.code != 0 && fr.code != 1) {
        char sum[256];
        exfat_fsck_summary(&fr, 0, sum, sizeof(sum));
        sb_tr(&msg, " %s: %s", T("Nachkontrolle"), sum);
    }
    sb_add(&res, "{\"test\":\"%s\",\"scheme\":\"%s\",\"folders\":%s,\"limit\":%llu",
           test_cancelled ? "cancelled" : tested == 2 ? "full" : tested ? "quick" : "none", o.is_gpt ? "GPT" : "MBR",
           o.dirs ? "true" : "false", (unsigned long long)j->limit);
    if (tested && !test_cancelled) speed_json(&res, &t);
    sb_add(&res, ",\"report\":%s}", rep.p ? rep.p : "[]");
    free(rep.p);
    job_done(j, 1, &msg, &res, 1);
    return NULL;
}

static void *speed_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    char err[256] = "", title[160];
    sbuf_t msg = {0}, res = {0};
    devtest_t t;
    int fd = plat_open_ro(&j->dev, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    set_flags(1, 0);
    int rc = dt_read_speed(fd, j->dev.ss, j->dev.size, on_progress, &cancel_requested, &t, err, sizeof(err));
    set_flags(0, 0);
    close(fd);
    if (rc != 0) { sb_tr(&msg, "Tempo messen fehlgeschlagen: %s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    if (t.cancelled) { sb_tr(&msg, "Abgebrochen."); job_done(j, 0, &msg, &res, 0); return NULL; }
    sb_tr(&msg, "Lesetempo von %s: %.0f MB/s – %s.", dev_title(&j->dev, title, sizeof(title)), t.read_mbps,
          rating_text(t.read_mbps));
    sb_add(&res, "{\"x\":0");
    speed_json(&res, &t);
    sb_add(&res, "}");
    job_done(j, 1, &msg, &res, 0);
    return NULL;
}

static void *check_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    char err[256] = "", sum[256], title[160];
    sbuf_t msg = {0}, res = {0};
    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    fsinfo_t fi;
    fs_probe(fd, j->dev.ss, j->dev.size, &fi);
    if (strcmp(fi.fs, "exFAT") != 0) {
        job_release(fd);
        sb_tr(&msg, "Prüfen und Reparieren geht nur bei exFAT (dieser Datenträger: %s).", T(fi.fs));
        job_done(j, 0, &msg, &res, 0);
        return NULL;
    }
    on_progress(j->repair ? N_("Dateisystem reparieren") : N_("Dateisystem prüfen"), 0);   /* 0 %: Lauf-Balken */
    fsck_result_t r;
    exfat_fsck_run(fd, fi.start, fi.size ? fi.size : j->dev.size - fi.start, j->dev.ss, j->repair, &r);
    job_release(fd);
    exfat_fsck_summary(&r, j->repair, sum, sizeof(sum));
    sb_add(&msg, "%s: %s", dev_title(&j->dev, title, sizeof(title)), sum);
    sb_add(&res, "{\"code\":%d,\"repair\":%s,\"dirs\":%ld,\"files\":%ld,\"log\":", r.code,
           j->repair ? "true" : "false", r.dirs, r.files);
    size_t loglen = strlen(r.log);
    const char *tail = loglen > 1000 ? r.log + loglen - 1000 : r.log;   /* nur das Ende ... */
    while ((*tail & 0xC0) == 0x80) tail++;                              /* ... ab einem ganzen UTF-8-Zeichen */
    sb_str(&res, tail);
    sb_add(&res, "}");
    int ok = r.code == 0 || r.code == 1 || (!j->repair && r.code == 4);
    job_done(j, ok, &msg, &res, j->repair);
    return NULL;
}

static void *label_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    char err[256] = "", title[160];
    sbuf_t msg = {0}, res = {0};
    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    on_progress("Bezeichnung schreiben", 50);
    int rc = vol_set_label(fd, j->dev.ss, j->dev.size, j->label, err, sizeof(err));
    job_release(fd);
    if (rc != 0) { sb_tr(&msg, "Umbenennen fehlgeschlagen: %s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    if (j->label[0]) sb_tr(&msg, "Neue Bezeichnung für %s: „%s“.", dev_title(&j->dev, title, sizeof(title)), j->label);
    else sb_tr(&msg, "Bezeichnung von %s entfernt.", dev_title(&j->dev, title, sizeof(title)));
    sb_add(&res, "{\"label\":");
    sb_str(&res, j->label);
    sb_add(&res, "}");
    job_done(j, 1, &msg, &res, 1);
    return NULL;
}

/* ------------------------------------------------------------ Spielebibliothek (Hintergrundsuche) */

typedef struct {
    char       device[16];   /* leer: interner Ordner (z. B. /data/homebrew) */
    char       place[16];    /* dann der Ort: "intern" (/data/...) oder "extN" (/mnt/extN) */
    char     **xtra;         /* je Titel: zugehörige Dateien/Ordner (Pfade, getrennt durch Zeilenumbruch) oder NULL - einmal je Suche
                                gesucht, damit /api/library nicht bei jedem Abruf den Datenträger durchsucht */
    int        usb;
    lib_root_t r;
    /* welcher Datenträger dort eingehängt war: steckt am selben Anschluss ein anderer (oder wurde neu eingehängt),
       gilt die Liste als veraltet */
    dev_t      dev;          /* st_dev der Wurzel - jede Einhängung bekommt eine neue */
    uint64_t   size;
    char       ident[64];
} libroot_t;

/* Name, unter dem Clients die Wurzel kennen: USB-Laufwerk (da1) oder fester Ort (intern, ext1) */
static const char *lroot_name(const libroot_t *r) { return r->device[0] ? r->device : r->place; }

/* Interner Speicher der PS5 (Tests: USBFMT_INTERNAL_BASE) */
static const char *internal_base(void) {
    const char *e = getenv("USBFMT_INTERNAL_BASE");
    return e && *e == '/' ? e : "/data";
}

static void place_of(const char *root, char *out, size_t n) {
    if (!strncmp(root, "/mnt/ext", 8) && root[8] >= '0' && root[8] <= '9') {
        const char *b = strrchr(root, '/');
        snprintf(out, n, "%s", b + 1);
    } else snprintf(out, n, "%s", "intern");
}

/* Ordner, unter dem der Pfad eines Spiels gespeichert wird: bei internen Orten /data, sonst die Wurzel */
static const char *move_base(const char *root, char *buf, size_t n) {
    const char *ib = internal_base();
    size_t l = strlen(ib);
    if (!strncmp(root, ib, l) && root[l] == '/') { snprintf(buf, n, "%s", ib); return buf; }
    return root;
}

static dev_t root_dev(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 ? s.st_dev : (dev_t)-1;
}

static pthread_mutex_t lib_lock = PTHREAD_MUTEX_INITIALIZER;
static libroot_t *lib_snap;
static int lib_n, lib_busy, lib_again;
/* Fortschritt der laufenden Suche: durchsuchte Ordner von allen und der gerade durchsuchte (für /api/library) */
static int lib_done, lib_total;
static char lib_cur[160];
static time_t lib_when;

/* Kurzer Schlüssel eines Spiels für /api/delete (FNV-1a über den Pfad): gelöscht wird nur, was die Suche fand */
static uint32_t path_key(const char *p) {
    uint32_t h = 2166136261u;
    while (*p) { h ^= (unsigned char)*p++; h *= 16777619u; }
    return h;
}

static int find_extras(const char *root, const char *path, const char *id, char (*out)[300], int max);

/* Zwischengespeicherte zugehörige Dateien (Zeilenumbruch-getrennt) in einen Auftragseintrag übernehmen */
static int xtra_has(const char *xt, const char *ex) {
    const size_t el = strlen(ex);
    for (const char *a = xt; a && *a;) {
        const char *e = strchr(a, 10);
        const size_t l = e ? (size_t)(e - a) : strlen(a);
        if (l == el && !memcmp(a, ex, el)) return 1;
        if (!e) break;
        a = e + 1;
    }
    return 0;
}

static void libroot_free(libroot_t *r) {
    for (int i = 0; r->xtra && i < r->r.ntitles; i++) free(r->xtra[i]);
    free(r->xtra);
    r->xtra = NULL;
    lib_free(&r->r);
}

/* Zugehörige Dateien aller Titel der Wurzel suchen und merken */
static void item_extras(del_item_t *it, const char *xt);
static void libroot_fill_extras(libroot_t *r) {
    char (*ex)[300] = malloc(sizeof(char[300]) * LIB_EXTRAS_MAX);
    r->xtra = r->r.ntitles > 0 ? calloc((size_t)r->r.ntitles, sizeof(*r->xtra)) : NULL;
    for (int k = 0; ex && r->xtra && k < r->r.ntitles; k++) {
        const lib_title_t *t = &r->r.titles[k];
        const int nex = find_extras(r->r.root, t->path, t->id, ex, LIB_EXTRAS_MAX);
        if (!nex) continue;
        char *joined = malloc((size_t)nex * 300 + 1);
        if (!joined) continue;
        size_t o = 0;
        for (int x = 0; x < nex; x++) o += (size_t)snprintf(joined + o, 301, "%s%s", x ? "\n" : "", ex[x]);
        r->xtra[k] = joined;
    }
    free(ex);
}

static void item_extras(del_item_t *it, const char *xt) {
    it->nex = 0;
    for (const char *a = xt; a && *a && it->nex < LIB_EXTRAS_MAX;) {
        const char *e = strchr(a, 10);
        const size_t l = e ? (size_t)(e - a) : strlen(a);
        if (l < sizeof(it->ex[0])) {
            memcpy(it->ex[it->nex], a, l);
            it->ex[it->nex][l] = 0;
            it->nex++;
        }
        if (!e) break;
        a = e + 1;
    }
}

/* Löschen: was auch ein anderer, bleibender Titel nutzt (zweite Kopie derselben Titel-ID, gemeinsame Prüfsumme), bleibt stehen.
 * Aufruf unter lib_lock. */
static void drop_shared_extras(job_t *j) {
    for (int i = 0; i < j->nitems; i++) {
        del_item_t *it = &j->items[i];
        int keep = 0;
        for (int x = 0; x < it->nex; x++) {
            int shared = 0;
            for (int r = 0; r < lib_n && !shared; r++)
                for (int t = 0; t < lib_snap[r].r.ntitles && !shared; t++) {
                    const char *tp = lib_snap[r].r.titles[t].path;
                    int selected = 0;
                    for (int q = 0; q < j->nitems; q++) selected |= !strcmp(j->items[q].path, tp);
                    if (!selected && lib_snap[r].xtra && lib_snap[r].xtra[t] && xtra_has(lib_snap[r].xtra[t], it->ex[x])) shared = 1;
                }
            if (!shared) {
                if (keep != x) memcpy(it->ex[keep], it->ex[x], sizeof(it->ex[0]));
                keep++;
            }
        }
        it->nex = keep;
    }
}

static void lib_free_all(libroot_t *r, int n) {
    for (int i = 0; i < n; i++) libroot_free(&r[i]);
    free(r);
}

/* done < 0: nächster Ordner (path) beginnt - der vorige ist fertig */
static void lib_progress(int done, int total, const char *path) {
    pthread_mutex_lock(&lib_lock);
    if (done >= 0) lib_done = done;
    else if (lib_cur[0]) lib_done++;
    lib_total = total;
    snprintf(lib_cur, sizeof(lib_cur), "%s", path ? path : "");
    pthread_mutex_unlock(&lib_lock);
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
        int cap = (nd > 0 ? nd : 0) * MAX_MOUNTS + 16, n = 0, total = 0;
        libroot_t *roots = calloc((size_t)cap, sizeof(*roots));
        for (int i = 0; i < nd; i++) total += devs[i].locked ? 0 : devs[i].nmounts;
        for (int k = 0; extra && extra[k] && extra[k + 1]; k += 2) total++;
        lib_progress(0, total, NULL);
        for (int i = 0; roots && i < nd; i++) {
            if (devs[i].locked) continue;
            for (int m = 0; m < devs[i].nmounts && n < cap; m++) {
                snprintf(roots[n].device, sizeof(roots[n].device), "%s", devs[i].name);
                roots[n].usb = 1;
                roots[n].dev = root_dev(devs[i].mounts[m].path);
                roots[n].size = devs[i].size;
                snprintf(roots[n].ident, sizeof(roots[n].ident), "%s", devs[i].ident);
                lib_progress(-1, total, devs[i].mounts[m].path);
                lib_scan(devs[i].mounts[m].path, 1, &roots[n].r);
                libroot_fill_extras(&roots[n]);
                n++;
            }
        }
        for (int k = 0; roots && extra && extra[k] && extra[k + 1] && n < cap; k += 2) {
            roots[n].usb = !strcmp(extra[k + 1], "usb");
            place_of(extra[k], roots[n].place, sizeof(roots[n].place));
            roots[n].dev = root_dev(extra[k]);
            lib_progress(-1, total, extra[k]);
            lib_scan(extra[k], roots[n].usb, &roots[n].r);
            if (roots[n].r.ntitles || roots[n].r.pkgs) { libroot_fill_extras(&roots[n]); n++; }
            else libroot_free(&roots[n]);
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
    lib_done = lib_total = 0;   /* bis die Suche ihre Ordner kennt: nicht der Stand der vorigen */
    lib_cur[0] = 0;
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

/* ------------------------------------------------------------ Zugehörige Dateien eines Spiels
 * Zu einem Spiel gehören neben seinem Ordner/Abbild: der Ordner <Titel-ID> im Backport-Ordner (nur dieser, nie andere)
 * und Begleitdateien neben dem Spiel (<Name>.sha256, <Name>.dump-info.json). Gefunden wird nur, was wirklich da ist. */
static int id_safe(const char *id) {
    if (!id[0]) return 0;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

static int extra_add(char (*out)[300], int n, int max, const char *path, int want_dir) {
    struct stat sb;
    if (n >= max || lstat(path, &sb) != 0 || (want_dir ? !S_ISDIR(sb.st_mode) : !S_ISREG(sb.st_mode))) return n;
    for (int i = 0; i < n; i++) if (!strcmp(out[i], path)) return n;
    if (snprintf(out[n], 300, "%s", path) >= 300) return n;
    return n + 1;
}

/* Backport-Ordner zu einer Titel-ID in <parent>/backports (auch "backport"): der Ordner mit genau diesem Namen, sonst der
 * erste (Byte-Reihenfolge), dessen Name mit der Titel-ID beginnt (z. B. "PPSA12345 Spielname"); andere nie */
static int backport_in(const char *parent, const char *id, char (*out)[300], int n, int max) {
    static const char *const names[] = {"backports", "backport"};
    const size_t il = strlen(id);
    for (int k = 0; k < 2; k++) {
        char dp[700], best[256] = "";
        snprintf(dp, sizeof(dp), "%s/%s", parent, names[k]);
        DIR *d = opendir(dp);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            struct stat sb;
            char t[1000];
            if (strncmp(e->d_name, id, il) != 0 || strlen(e->d_name) >= sizeof(best)) continue;
            if (snprintf(t, sizeof(t), "%s/%s", dp, e->d_name) >= (int)sizeof(t) || stat(t, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
            if (!strcmp(e->d_name, id)) { snprintf(best, sizeof(best), "%s", e->d_name); break; }
            if (!best[0] || strcmp(e->d_name, best) < 0) snprintf(best, sizeof(best), "%s", e->d_name);
        }
        closedir(d);
        if (best[0]) {
            char t[1000];
            snprintf(t, sizeof(t), "%s/%s", dp, best);
            n = extra_add(out, n, max, t, 1);
        }
    }
    return n;
}

static int find_extras(const char *root, const char *path, const char *id, char (*out)[300], int max) {
    int n = 0;
    char dir[300], base[200], bare[200], t[700];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (!sl) return 0;
    snprintf(base, sizeof(base), "%s", sl + 1);
    *sl = 0;
    snprintf(bare, sizeof(bare), "%s", base);
    char *dot = strrchr(bare, '.');
    if (dot && dot != bare) *dot = 0;
    static const char *const sfx[] = {".sha256", ".dump-info.json"};
    for (int k = 0; k < 2; k++) {
        snprintf(t, sizeof(t), "%s/%s%s", dir, base, sfx[k]);
        n = extra_add(out, n, max, t, 0);
        if (strcmp(bare, base) != 0) { snprintf(t, sizeof(t), "%s/%s%s", dir, bare, sfx[k]); n = extra_add(out, n, max, t, 0); }
    }
    if (id_safe(id)) {   /* wie ShadowMountPlus: <Suchordner>/backports/<Titel-ID>[ Zusatz] */
        const char *ib = internal_base();
        char b1[300], b2[300], b3[300], b4[300];
        snprintf(b1, sizeof(b1), "%s/homebrew", root);
        snprintf(b2, sizeof(b2), "%s/etaHEN/games", root);
        snprintf(b3, sizeof(b3), "%s/homebrew", ib);
        snprintf(b4, sizeof(b4), "%s/etaHEN/games", ib);
        const size_t il = strlen(ib);
        const int is_internal = !strncmp(root, ib, il) && root[il] == '/';   /* nur interne Spiele nutzen die internen Backports mit */
        const char *bases[] = {dir, root, b1, b2, is_internal ? ib : NULL, b3, b4, NULL};
        for (int b = 0; b < 7; b++)
            if (bases[b] && (b < 4 || is_internal)) n = backport_in(bases[b], id, out, n, max);
    }
    return n;
}

static void api_library(int c, int refresh);

/* ------------------------------------------------------------ JSON lesen */

/* Flaches JSON-Objekt mit String-, Zahl- und Bool-Werten einlesen */
#define KV_MAX 16
typedef struct { char key[16]; char val[512]; } kv_t;   /* Werte bis 511 Bytes (Pfade der Image-Dateien) */

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
    char hdr[1024];
    const char *reason = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" :
                         code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
                         code == 409 ? "Conflict" : code == 411 ? "Length Required" :
                         code == 415 ? "Unsupported Media Type" :
                         code == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: %s\r\nX-Content-Type-Options: nosniff\r\n"
                     "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
                     /* nur eigene Inhalte (die Seite ist eine Datei mit eingebettetem Skript/Stil); nach außen nur
                        die Abfrage der neuesten Version bei GitHub (Hinweis unten auf der Seite) */
                     "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; "
                     "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                     "connect-src 'self' https://api.github.com; "
                     "frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, ctype, len, cache);
    if (n <= 0 || (size_t)n >= sizeof(hdr)) return;   /* nie über den Puffer hinaus senden */
    send_all(c, hdr, (size_t)n);
    send_all(c, body, len);
}

static void respond(int c, int code, const char *ctype, const char *body, size_t len) {
    respond_ex(c, code, ctype, body, len, "no-store");
}

static void respond_json(int c, int code, sbuf_t *b) {
    respond(c, code, "application/json; charset=utf-8", b->p ? b->p : "{}", b->p ? b->len : 2);
}

/* msg: deutscher Text (oder schon übersetzt) - gesendet in der Sprache der Anfrage */
static void respond_error(int c, int code, const char *msg) {
    sbuf_t b = {0};
    sb_add(&b, "{\"error\":");
    sb_str(&b, T(msg));
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
        sb_add(&b, ",\"ident\":");   /* Seriennummer: Oberflächen schicken sie beim Auftrag zurück */
        sb_str(&b, d->ident);
        sb_add(&b, ",\"size\":%llu,\"sector\":%u,\"locked\":%s,\"part_end\":%llu,\"fake_real\":%llu,\"mounts\":[",
               (unsigned long long)d->size, d->ss, d->locked ? "true" : "false", (unsigned long long)d->part_end,
               (unsigned long long)fake_get(d));
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

    /* Laufwerk eingebunden, das die Spieleliste noch nicht kennt (z. B. die Spiele-SSD wurde angesteckt, nachdem
     * der Formatierer gestartet war) oder an dessen Einhängepunkt jetzt ein anderer Datenträger steckt: neu suchen -
     * sonst fehlte beim Formatieren die Warnung vor den Spielen darauf, und gelöscht würde nach einer alten Liste.
     * Veraltete Einträge fallen sofort weg ("wird durchsucht" statt der Spiele des vorigen Sticks). */
    dev_t mdev[MAX_DEVS][MAX_MOUNTS];
    for (int i = 0; i < n; i++)
        for (int m = 0; m < devs[i].nmounts; m++) mdev[i][m] = root_dev(devs[i].mounts[m].path);
    int stale = 0;
    pthread_mutex_lock(&lib_lock);
    for (int i = 0; i < n && !busy && lib_snap && !lib_busy; i++)
        for (int m = 0; !devs[i].locked && m < devs[i].nmounts; m++) {
            int known = 0;
            for (int k = 0; k < lib_n; k++) {
                libroot_t *r = &lib_snap[k];
                if (strcmp(r->r.root, devs[i].mounts[m].path) != 0) continue;
                if (!r->device[0]) known = 1;   /* fester Ordner (z. B. /mnt/ext0): wie bisher nur über den Pfad */
                else if (strcmp(r->device, devs[i].name) != 0 || r->size != devs[i].size || r->dev != mdev[i][m] ||
                    (r->ident[0] && devs[i].ident[0] && strcmp(r->ident, devs[i].ident) != 0)) {
                    libroot_free(r);   /* veraltet: Eintrag entfernen */
                    memmove(r, r + 1, (size_t)(lib_n - k - 1) * sizeof(*r));
                    lib_n--;
                    k--;
                } else known = 1;
            }
            stale |= !known;
        }
    pthread_mutex_unlock(&lib_lock);
    if (stale) lib_refresh();
}

/* Spiele aller Laufwerke; doppelte Titel-IDs (ShadowMountPlus wechselt dann ständig zwischen ihnen) gesondert */
/* ------------------------------------------------------------ Größen der Spiele-Ordner
 * Die Spielesuche kennt nur die Größe von Abbildern (Dateigröße); die von Ordner-Spielen ermittelt dieser Hintergrundlauf auf
 * Wunsch (POST /api/library/sizes), weil er jede Datei ansehen muss. Ergebnisse (je Spiel-Schlüssel) liefert GET /api/library/sizes. */
typedef struct { uint32_t key; uint64_t bytes; } gsize_t;
static pthread_mutex_t gs_lock = PTHREAD_MUTEX_INITIALIZER;
static gsize_t *gs_arr;
static int gs_n, gs_cap, gs_running, gs_done, gs_total;
static char gs_device[16];

static uint64_t tree_bytes(const char *path, int depth) {
    struct stat sb;
    if (depth > 32 || __atomic_load_n(&quit_requested, __ATOMIC_RELAXED) || lstat(path, &sb) != 0) return 0;
    if (!S_ISDIR(sb.st_mode)) return S_ISREG(sb.st_mode) ? (uint64_t)sb.st_size : 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    uint64_t w = 0;
    char *sub = malloc(1024);
    struct dirent *e;
    while (sub && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, 1024, "%s/%s", path, e->d_name) < 1024) w += tree_bytes(sub, depth + 1);
    }
    free(sub);
    closedir(d);
    return w;
}

typedef struct { uint32_t key; char path[256]; } gsjob_t;

static void *gsize_worker(void *arg) {
    (void)arg;
    gsjob_t *jobs = NULL;
    int n = 0;
    pthread_mutex_lock(&lib_lock);   /* Ordner-Spiele des Laufwerks aus dem letzten Suchlauf */
    for (int i = 0; i < lib_n; i++)
        if (!strcmp(lroot_name(&lib_snap[i]), gs_device))
            for (int k = 0; k < lib_snap[i].r.ntitles; k++)
                if (!strcmp(lib_snap[i].r.titles[k].kind, "Ordner")) {
                    gsjob_t *p = realloc(jobs, (size_t)(n + 1) * sizeof(*jobs));
                    if (!p) break;
                    jobs = p;
                    jobs[n].key = path_key(lib_snap[i].r.titles[k].path);
                    snprintf(jobs[n].path, sizeof(jobs[n].path), "%s", lib_snap[i].r.titles[k].path);
                    n++;
                }
    pthread_mutex_unlock(&lib_lock);
    pthread_mutex_lock(&gs_lock);
    gs_total = n;
    gs_done = 0;
    pthread_mutex_unlock(&gs_lock);
    for (int i = 0; i < n && !__atomic_load_n(&quit_requested, __ATOMIC_RELAXED); i++) {
        uint64_t b = tree_bytes(jobs[i].path, 0);
        pthread_mutex_lock(&gs_lock);
        int at = -1;
        for (int k = 0; k < gs_n; k++) if (gs_arr[k].key == jobs[i].key) at = k;
        if (at < 0 && gs_n == gs_cap) {
            gsize_t *p = realloc(gs_arr, (size_t)(gs_cap = gs_cap ? gs_cap * 2 : 64) * sizeof(*p));
            if (p) gs_arr = p; else gs_cap = gs_n;
        }
        if (at < 0 && gs_n < gs_cap) at = gs_n++;
        if (at >= 0) { gs_arr[at].key = jobs[i].key; gs_arr[at].bytes = b; }
        gs_done = i + 1;
        pthread_mutex_unlock(&gs_lock);
    }
    free(jobs);
    pthread_mutex_lock(&gs_lock);
    gs_running = 0;
    pthread_mutex_unlock(&gs_lock);
    return NULL;
}

/* POST /api/library/sizes {"device":"da1"}: Größen der Ordner-Spiele dieses Laufwerks ermitteln (läuft schon: nichts doppelt) */
static void api_library_sizes_start(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    const char *dev = n > 0 ? kv_get(kv, n, "device") : NULL;
    int known = 0;
    pthread_mutex_lock(&lib_lock);
    for (int i = 0; dev && i < lib_n; i++) if (!strcmp(lroot_name(&lib_snap[i]), dev)) known = 1;
    pthread_mutex_unlock(&lib_lock);
    if (!known) { respond_error(c, 400, N_("Laufwerk unbekannt")); return; }
    pthread_mutex_lock(&gs_lock);
    int start = !gs_running;
    if (start) { gs_running = 1; snprintf(gs_device, sizeof(gs_device), "%.15s", dev); }
    pthread_mutex_unlock(&gs_lock);
    if (start) {
        pthread_t th;
        if (pthread_create(&th, NULL, gsize_worker, NULL) == 0) pthread_detach(th);
        else { pthread_mutex_lock(&gs_lock); gs_running = 0; pthread_mutex_unlock(&gs_lock); respond_error(c, 500, N_("Thread konnte nicht gestartet werden")); return; }
    }
    respond_ok(c, 202);
}

/* GET /api/library/sizes */
static void api_library_sizes(int c) {
    sbuf_t b = {0};
    pthread_mutex_lock(&gs_lock);
    sb_add(&b, "{\"running\":%s,\"done\":%d,\"total\":%d,\"sizes\":{", gs_running ? "true" : "false", gs_done, gs_total);
    for (int i = 0; i < gs_n; i++) sb_add(&b, "%s\"%08x\":%llu", i ? "," : "", gs_arr[i].key, (unsigned long long)gs_arr[i].bytes);
    pthread_mutex_unlock(&gs_lock);
    sb_add(&b, "}}");
    respond_json(c, 200, &b);
}

/* Versionen wie "01.02", "01.010.000" oder "9.00" vergleichen: Zahlen je Abschnitt, fehlende Abschnitte zählen als 0 */
static int ver_cmp(const char *a, const char *b) {
    while (*a || *b) {
        unsigned long x = 0, y = 0;
        while (*a && *a != '.') { if (*a >= '0' && *a <= '9') x = x * 10 + (unsigned long)(*a - '0'); a++; }
        while (*b && *b != '.') { if (*b >= '0' && *b <= '9') y = y * 10 + (unsigned long)(*b - '0'); b++; }
        if (x != y) return x > y ? 1 : -1;
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

#define MOVE_DIR     ".usbfmt-move"

/* Wurzeln, in denen Kopieren seinen Zwischenordner anlegt (wie api_move): eingebundene USB-Laufwerke, interner Speicher, M.2-Erweiterungen */
typedef struct { char path[300]; char dev[16]; } proot_t;

static int partial_roots(proot_t *out, int max) {   /* lib_lock muss gehalten werden */
    int n = 0;
    for (int i = 0; i < lib_n && n < max; i++) {
        if (!lib_snap[i].usb || !lroot_name(&lib_snap[i])[0]) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= !strcmp(out[k].path, lib_snap[i].r.root);
        if (dup) continue;
        snprintf(out[n].path, sizeof(out[n].path), "%s", lib_snap[i].r.root);
        snprintf(out[n].dev, sizeof(out[n].dev), "%.15s", lroot_name(&lib_snap[i]));
        n++;
    }
    if (n < max) { snprintf(out[n].path, sizeof(out[n].path), "%s", internal_base()); snprintf(out[n].dev, sizeof(out[n].dev), "intern"); n++; }
    for (int x = 0; x < 10 && n < max; x++) {
        struct stat xs;
        char p[32];
        snprintf(p, sizeof(p), "/mnt/ext%d", x);
        if (stat(p, &xs) != 0 || !S_ISDIR(xs.st_mode) || root_dev(p) == root_dev("/mnt")) continue;
        snprintf(out[n].path, sizeof(out[n].path), "%s", p);
        snprintf(out[n].dev, sizeof(out[n].dev), "ext%d", x);
        n++;
    }
    return n;
}

static void api_library(int c, int refresh) {
    if (refresh) lib_refresh();
    sbuf_t b = {0};
    char fw[16];
    plat_fw_version(fw, sizeof(fw));
    pthread_mutex_lock(&lib_lock);
    if (!lib_snap && !lib_busy) {   /* erste Anfrage: Suche anstoßen */
        pthread_mutex_unlock(&lib_lock);
        lib_refresh();
        pthread_mutex_lock(&lib_lock);
    }
    sb_add(&b, "{\"scanning\":%s,\"age\":%ld,", lib_busy ? "true" : "false",
           lib_when ? (long)(time(NULL) - lib_when) : -1L);
    if (lib_busy) {   /* Fortschritt der laufenden Suche: durchsuchte Ordner von allen, der gerade durchsuchte */
        sb_add(&b, "\"progress\":{\"done\":%d,\"total\":%d,\"root\":", lib_done, lib_total);
        sb_str(&b, lib_cur);
        sb_add(&b, "},");
    }
    sb_add(&b, "\"roots\":[");
    for (int i = 0; i < lib_n; i++) {
        const libroot_t *r = &lib_snap[i];
        sb_add(&b, "%s{\"root\":", i ? "," : "");
        sb_str(&b, r->r.root);
        sb_add(&b, ",\"device\":");
        sb_str(&b, lroot_name(r));
        sb_add(&b, ",\"place\":%s,\"usb\":%s,\"titles\":%d,\"pkgs\":%d,\"pkg_bytes\":%llu,\"folders\":",
               r->device[0] ? "false" : "true", r->usb ? "true" : "false", r->r.ntitles, r->r.pkgs, (unsigned long long)r->r.pkg_bytes);
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
            sb_add(&b, ",\"ext\":");
            sb_str(&b, t->ext);
            sb_add(&b, ",\"version\":");
            sb_str(&b, t->version);
            sb_add(&b, ",\"key\":\"%08x\",\"size\":%llu,\"root\":", path_key(t->path), (unsigned long long)t->size);
            sb_str(&b, lib_snap[i].r.root);
            sb_add(&b, ",\"device\":");
            sb_str(&b, lroot_name(&lib_snap[i]));
            const char *xt = lib_snap[i].xtra ? lib_snap[i].xtra[k] : NULL;
            if (xt && *xt) {   /* zugehörige Dateien und Ordner (Backport, Prüfsummen): mit dem Spiel kopieren bzw. löschen */
                sb_add(&b, ",\"extras\":[");
                for (const char *a = xt; *a;) {
                    const char *e = strchr(a, '\n');
                    char one[300];
                    snprintf(one, sizeof(one), "%.*s", (int)(e ? (size_t)(e - a) : strlen(a)), a);
                    if (a != xt) sb_add(&b, ",");
                    sb_str(&b, one);
                    if (!e) break;
                    a = e + 1;
                }
                sb_add(&b, "]");
            }
            sb_add(&b, "}");
        }
    /* .pkg-Dateien (Installationspakete) mit Plattform */
    sb_add(&b, "],\"pkgfiles\":[");
    first = 1;
    for (int i = 0; i < lib_n; i++)
        for (int k = 0; k < lib_snap[i].r.npkglist; k++) {
            const lib_pkg_t *pk = &lib_snap[i].r.pkglist[k];
            const char *nm = strrchr(pk->path, '/');
            sb_add(&b, "%s{\"key\":\"%08x\",\"path\":", first ? "" : ",", path_key(pk->path));
            first = 0;
            sb_str(&b, pk->path);
            sb_add(&b, ",\"name\":");
            sb_str(&b, nm ? nm + 1 : pk->path);
            sb_add(&b, ",\"platform\":");
            sb_str(&b, pk->platform);
            sb_add(&b, ",\"type\":");
            sb_str(&b, pk->type);
            sb_add(&b, ",\"title\":");
            sb_str(&b, pk->title);
            sb_add(&b, ",\"version\":");
            sb_str(&b, pk->version);
            sb_add(&b, ",\"cat\":");
            sb_str(&b, pk->cat);
            sb_add(&b, ",\"minfw\":");
            sb_str(&b, pk->minfw);
            sb_add(&b, ",\"title_id\":");
            sb_str(&b, pk->title_id);
            sb_add(&b, ",\"update\":%s,\"size\":%llu,\"device\":", pk->update ? "true" : "false", (unsigned long long)pk->size);
            sb_str(&b, lroot_name(&lib_snap[i]));
            /* Vergleich mit dem, was schon auf der PS5 liegt: installierte Version (bei Spielen als Ordner) und Firmware */
            int found = 0;
            const char *inst = "";
            if (pk->title_id[0] && strcmp(pk->cat, "dlc") != 0)
                for (int i2 = 0; i2 < lib_n; i2++)
                    for (int k2 = 0; k2 < lib_snap[i2].r.ntitles; k2++) {
                        const lib_title_t *tt = &lib_snap[i2].r.titles[k2];
                        if (strcmp(tt->id, pk->title_id) != 0) continue;
                        found = 1;
                        if (tt->version[0] && (!inst[0] || ver_cmp(tt->version, inst) > 0)) inst = tt->version;
                    }
            int vc = inst[0] && pk->version[0] ? ver_cmp(pk->version, inst) : 0;
            sb_add(&b, ",\"installed\":%s,\"inst\":", found ? "true" : "false");
            sb_str(&b, inst);
            sb_add(&b, ",\"vs\":\"%s\",\"fwhigh\":%s", !inst[0] || !pk->version[0] ? "" : vc > 0 ? "newer" : vc < 0 ? "older" : "same",
                   !strcmp(pk->platform, "PS5") && pk->minfw[0] && fw[0] && ver_cmp(pk->minfw, fw) > 0 ? "true" : "false");
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
    /* liegengebliebene Teilkopien (abgebrochenes oder fehlgeschlagenes Kopieren): belegen Platz, den man sonst nirgends sieht */
    sb_add(&b, "],\"partials\":[");
    {
        proot_t pr[24];
        const int np = partial_roots(pr, 24);
        int pf = 1;
        for (int k = 0; k < np; k++) {
            char stage[400];
            struct stat ps;
            snprintf(stage, sizeof(stage), "%.299s/%s", pr[k].path, MOVE_DIR);
            if (lstat(stage, &ps) != 0 || !S_ISDIR(ps.st_mode)) continue;
            const uint64_t bytes = tree_bytes(stage, 0);
            if (!bytes) continue;
            sb_add(&b, "%s{\"device\":", pf ? "" : ",");
            pf = 0;
            sb_str(&b, pr[k].dev);
            sb_add(&b, ",\"bytes\":%llu}", (unsigned long long)bytes);
        }
    }
    sb_add(&b, "],\"fw\":");
    sb_str(&b, fw);
    sb_add(&b, "}");
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
    sb_add(&b, ",\"port\":%d,\"version\":", SERVER_PORT);
    sb_str(&b, USBFMT_VERSION);
    sb_add(&b, "}");
    respond_json(c, 200, &b);
    free(b.p);
}

/* ------------------------------------------------------------ Einstellungen (Design, Größe) */

static char theme[8] = "auto";
/* Größe der Oberfläche in Prozent, getrennt für den PS5-Browser (Fernseher) und andere Browser */
#define SCALE_MIN 50
#define SCALE_MAX 250
static int scale_tv = 160, scale_pc = 100;
static int lang_setting = -1;   /* in der Weboberfläche gewählte Sprache (LANG_*), -1 = Sprache des Browsers */

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
    lang_setting = i18n_parse(kv_get(kv, nkv, "lang"));
}

/* erst vollständig in eine neue Datei, dann umbenennen: ist /data voll, bleiben die alten Einstellungen erhalten */
static int settings_save(void) {
    char path[256], tmp[264];
    mkdir(plat_data_dir(), 0755);
    settings_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.neu", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    int ok = fprintf(f, "{\"theme\":\"%s\",\"scale_tv\":%d,\"scale_pc\":%d,\"lang\":\"%s\"}\n", theme, scale_tv,
                     scale_pc, lang_setting >= 0 ? i18n_code(lang_setting) : "") > 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* Auftragsverlauf (history_job): GET liefert ihn, clear leert ihn */
static void api_history(int c, int clear) {
    char path[256], *lines[HIST_MAX];
    if (clear) {
        pthread_mutex_lock(&hist_lock);
        history_path(path, sizeof(path));
        remove(path);
        pthread_mutex_unlock(&hist_lock);
        respond_ok(c, 200);
        return;
    }
    pthread_mutex_lock(&hist_lock);
    int n = history_read(lines, HIST_MAX);
    pthread_mutex_unlock(&hist_lock);
    sbuf_t b = {0};
    sb_add(&b, "{\"entries\":[");
    for (int i = 0; i < n; i++) {
        sb_add(&b, "%s%s", i ? "," : "", lines[i]);
        free(lines[i]);
    }
    sb_add(&b, "]}");
    respond_json(c, 200, &b);
    free(b.p);
}

static void api_settings(int c, int is_post, const char *body) {
    if (is_post) {
        /* einzeln oder zusammen: theme (auto|light|dark), scale_tv / scale_pc (50..250 %), lang (de|en|it|es|fr,
         * leer = Sprache des Browsers) */
        kv_t kv[KV_MAX];
        int n = json_parse(body, kv, KV_MAX);
        const char *t = n > 0 ? kv_get(kv, n, "theme") : NULL;
        const char *stv = n > 0 ? kv_get(kv, n, "scale_tv") : NULL, *spc = n > 0 ? kv_get(kv, n, "scale_pc") : NULL;
        const char *lg = n > 0 ? kv_get(kv, n, "lang") : NULL;
        if (!t && !stv && !spc && !lg) { respond_error(c, 400, "theme, scale_tv, scale_pc oder lang angeben"); return; }
        if (lg && *lg && i18n_parse(lg) < 0) { respond_error(c, 400, "lang muss de, en, it, es oder fr sein"); return; }
        if (t && !theme_valid(t)) { respond_error(c, 400, "theme muss auto, light oder dark sein"); return; }
        if ((stv && scale_parse(stv) < 0) || (spc && scale_parse(spc) < 0)) {
            respond_error(c, 400, "Größe muss zwischen 50 und 250 Prozent liegen");
            return;
        }
        pthread_mutex_lock(&lock);   /* Anfragen laufen parallel: setzen und speichern in einem Zug */
        if (t) theme_set(t);
        if (stv) scale_tv = scale_parse(stv);
        if (spc) scale_pc = scale_parse(spc);
        if (lg) lang_setting = *lg ? i18n_parse(lg) : -1;
        int saved = settings_save();
        pthread_mutex_unlock(&lock);
        if (saved != 0) { respond_error(c, 500, "Einstellung ließ sich nicht speichern"); return; }
    }
    sbuf_t b = {0};
    pthread_mutex_lock(&lock);
    sb_add(&b, "{\"theme\":");
    sb_str(&b, theme);
    sb_add(&b, ",\"scale_tv\":%d,\"scale_pc\":%d,\"lang\":\"%s\"}", scale_tv, scale_pc,
           lang_setting >= 0 ? i18n_code(lang_setting) : "");
    pthread_mutex_unlock(&lock);
    respond_json(c, 200, &b);
    free(b.p);
}

/* Sprache einer Anfrage: ?lang=xx in der Adresse (Weboberfläche, Windows-Programm), sonst die gespeicherte Wahl,
 * sonst die Sprache des Browsers (Accept-Language), sonst Deutsch */
static int request_lang(const char *path, const char *hdrs) {
    const char *q = strchr(path, '?');
    for (const char *p = q; p && (p = strstr(p, "lang=")); p += 5)
        if (p[-1] == '?' || p[-1] == '&') {
            int l = i18n_parse(p + 5);
            if (l >= 0) return l;
        }
    pthread_mutex_lock(&lock);
    int saved = lang_setting;
    pthread_mutex_unlock(&lock);
    if (saved >= 0) return saved;
    char al[128];
    if (header(hdrs, "Accept-Language", al, sizeof(al)))
        for (const char *p = al; *p; p++) {   /* "pt-BR,fr;q=0.8,en;q=0.5": erste unterstützte Sprache */
            while (*p == ' ') p++;
            int l = i18n_parse(p);
            if (l >= 0) return l;
            if (!(p = strchr(p, ','))) break;
        }
    return LANG_DE;
}

/* ------------------------------------------------------------ Gefälschte Sticks merken */

/* Ergebnis der Echtheitsprüfung je Stick (Seriennummer bzw. Modell + gemeldete Größe), dauerhaft in
 * <data>/fakes.txt: "<schlüssel>\t<echte Bytes>". So bleibt ein gefälschter Stick gekennzeichnet - auch nach
 * Neustart und Umstecken - und wird beim Formatieren auf seinen echten Bereich begrenzt. */
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

/* nur mit Seriennummer: ohne würde ein Ergebnis jeden gleich großen Stick ohne Seriennummer kennzeichnen */
static void fake_key(const usbdev_t *d, char *k, size_t n) {
    snprintf(k, n, "%s|%llu", d->ident, (unsigned long long)d->size);
    for (char *p = k; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
}

static void fake_path(char *p, size_t n) { snprintf(p, n, "%s/fakes.txt", plat_data_dir()); }

/* echte Größe eines bekannten gefälschten Sticks, sonst 0 */
static uint64_t fake_get(const usbdev_t *d) {
    char key[160], path[256], line[256];
    uint64_t real = 0;
    if (!d->ident[0]) return 0;
    fake_key(d, key, sizeof(key));
    fake_path(path, sizeof(path));
    pthread_mutex_lock(&fake_lock);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (tab && (size_t)(tab - line) == strlen(key) && !strncmp(line, key, strlen(key)))
            real = strtoull(tab + 1, NULL, 10);
    }
    if (f) fclose(f);
    pthread_mutex_unlock(&fake_lock);
    return real < d->size ? real : 0;
}

/* real > 0: als gefälscht merken; 0: Eintrag entfernen (Prüfung fand den Stick echt) */
static void fake_put(const usbdev_t *d, uint64_t real) {
    char key[160], path[256], tmp[264], line[256];
    if (!d->ident[0]) return;
    fake_key(d, key, sizeof(key));
    fake_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.neu", path);
    pthread_mutex_lock(&fake_lock);
    mkdir(plat_data_dir(), 0755);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    int ok = out != NULL;
    while (ok && in && fgets(line, sizeof(line), in)) {
        char *tab = strchr(line, '\t');
        if (tab && (size_t)(tab - line) == strlen(key) && !strncmp(line, key, strlen(key))) continue;
        ok = fputs(line, out) >= 0;
    }
    if (ok && real) ok = fprintf(out, "%s\t%llu\n", key, (unsigned long long)real) > 0;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    if (ok) rename(tmp, path);
    else remove(tmp);
    pthread_mutex_unlock(&fake_lock);
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
    sb_add(&b, "{\"busy\":%s,\"done\":%s,\"ok\":%s,\"percent\":%d,\"job\":%u,\"kind\":",
           st.busy ? "true" : "false", st.done ? "true" : "false", st.ok ? "true" : "false", st.percent, job_seq);
    sb_str(&b, st.kind);
    sb_add(&b, ",\"device\":");
    sb_str(&b, st.device);
    sb_add(&b, ",\"step\":");
    sb_str(&b, st.step);
    sb_add(&b, ",\"detail\":");
    sb_str(&b, st.detail);
    sb_add(&b, ",\"message\":");
    sb_str(&b, st.message);
    sb_add(&b, ",\"cancellable\":%s,\"replug\":%s,\"cancelled\":%s", st.cancellable ? "true" : "false",
           st.replug ? "true" : "false", st.cancelled ? "true" : "false");
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
    const char *dev = kv_get(kv, n, "device"), *size = kv_get(kv, n, "size"), *ident = kv_get(kv, n, "ident");
    if (!dev || !size) { respond_error(c, 400, "device und size sind erforderlich"); return -1; }
    usbdev_t devs[MAX_DEVS];
    int nd = plat_scan(devs, MAX_DEVS);
    for (int i = 0; i < nd; i++) {
        if (strcmp(devs[i].name, dev) != 0) continue;
        if (strtoull(size, NULL, 10) != devs[i].size) {
            respond_error(c, 409, "Gerätegröße hat sich geändert (anderer Stick?). Liste neu laden.");
            return -1;
        }
        /* Seriennummer gerade leer (frisch eingesteckt, GEOM-Abfrage gescheitert): kurz nachlesen - bleibt sie leer,
           ist es nicht sicher der Stick aus der Liste */
        usbdev_t *again = ident && ident[0] && !devs[i].ident[0] ? calloc(MAX_DEVS, sizeof(*again)) : NULL;
        for (int t = 0; again && t < 10 && !devs[i].ident[0]; t++) {
            usleep(100 * 1000);
            int na = plat_scan(again, MAX_DEVS);
            for (int k = 0; k < na; k++)
                if (!strcmp(again[k].name, dev) && again[k].size == devs[i].size)
                    memcpy(devs[i].ident, again[k].ident, sizeof(devs[i].ident));
        }
        free(again);
        /* gleicher Name und gleiche Größe, aber andere (oder keine) Seriennummer: ein baugleicher Stick wurde getauscht */
        if (ident && ident[0] && strcmp(ident, devs[i].ident) != 0) {
            respond_error(c, 409, "Unter diesem Namen steckt jetzt ein anderer Stick. Liste neu laden.");
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
    char gid[16], err[320];
    if (*replug && game_running(gid, sizeof(gid))) {
        game_error(gid, err, sizeof(err));
        respond_error(c, 409, err);
        return -1;
    }
    return 0;
}

/* Auftrag anmelden (einer zur Zeit). 0 = angemeldet; sonst ist die Fehlerantwort gesendet und j freigegeben. */
static int claim_job(int c, job_t *j) {
    pthread_mutex_lock(&lock);
    if (st.busy || quitting) {
        int q = quitting;
        pthread_mutex_unlock(&lock);
        job_free(j);
        respond_error(c, 409, q ? N_("Der Formatierer wird gerade beendet") : N_("Es läuft bereits ein Auftrag"));
        return -1;
    }
    memset(&st, 0, sizeof(st));
    st.busy = 1;
    clock_gettime(CLOCK_MONOTONIC, &j->t0);
    job_seq++;
    snprintf(st.kind, sizeof(st.kind), "%s", JOB_KIND[j->kind]);
    snprintf(st.device, sizeof(st.device), "%s", j->dev.name);
    snprintf(st.step, sizeof(st.step), "%s", T("Startet"));
    __atomic_store_n(&cancel_requested, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&cancel_keep, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&lock);
    return 0;
}

static void start_job(int c, job_t *j, void *(*fn)(void *)) {
    if (claim_job(c, j)) return;

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, JOB_STACK);
    int trc = pthread_create(&t, &attr, fn, j);
    pthread_attr_destroy(&attr);
    if (trc != 0) {
        job_free(j);
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
    else { j->kind = kind; j->lang = i18n_get(); }
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
        snprintf(m, sizeof(m), T("Ungültige Bezeichnung: max. %d Zeichen, nur A-Z, 0-9, Leerzeichen, - und _"),
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
    if (lim && lim < 64 * MIB && lim < d.size) {   /* gefälschter Stick mit sehr wenig echtem Speicher */
        char msg[256], s1[24];
        snprintf(msg, sizeof(msg), T("Dieser Stick hat nur etwa %s echten Speicher – zu wenig zum Formatieren (mindestens "
                                     "64 MB)."), size_text(lim, s1, sizeof(s1)));
        respond_error(c, 400, msg);
        return;
    }
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

/* ------------------------------------------------------------ Image-Sicherung (.img.zst) */

#define IMG_DIR    "usbfmt-backup"   /* Ordner der Sicherungen: /data/usbfmt-backup bzw. <USB-Laufwerk>/usbfmt-backup */
#define IMG_EXT    ".img.zst"
#define MAX_LOCS   (1 + MAX_DEVS * MAX_MOUNTS)
#define MAX_IMAGES 64                /* je Ordner */

typedef struct {
    char root[96];      /* Einhängepunkt bzw. /data */
    char dir[128];      /* root/usbfmt-backup */
    char device[16];    /* leer: intern */
    char fs[16], label[48];
} imgloc_t;

/* Orte für Sicherungen: intern neben dem Datenordner des Formatierers (/data/usb_format -> /data) und jedes
 * eingebundene USB-Laufwerk, das nicht vom System gesperrt ist */
static int img_locations(imgloc_t *loc, int max) {
    int n = 0;
    snprintf(loc[0].root, sizeof(loc[0].root), "%s", plat_data_dir());
    char *sl = strrchr(loc[0].root, '/');
    if (sl && sl != loc[0].root) *sl = 0;
    snprintf(loc[0].dir, sizeof(loc[0].dir), "%s/%s", loc[0].root, IMG_DIR);
    loc[0].device[0] = loc[0].fs[0] = loc[0].label[0] = 0;
    n = 1;
    usbdev_t *devs = calloc(MAX_DEVS, sizeof(*devs));
    int nd = devs ? plat_scan(devs, MAX_DEVS) : 0;
    for (int i = 0; i < nd; i++) {
        if (devs[i].locked) continue;
        for (int m = 0; m < devs[i].nmounts && n < max; m++, n++) {
            snprintf(loc[n].root, sizeof(loc[n].root), "%s", devs[i].mounts[m].path);
            snprintf(loc[n].dir, sizeof(loc[n].dir), "%s/%s", devs[i].mounts[m].path, IMG_DIR);
            snprintf(loc[n].device, sizeof(loc[n].device), "%s", devs[i].name);
            snprintf(loc[n].fs, sizeof(loc[n].fs), "%s", devs[i].mounts[m].fstype);
            snprintf(loc[n].label, sizeof(loc[n].label), "%s", devs[i].label);
        }
    }
    free(devs);
    return n;
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

/* Image-Datei aus einem der Orte (direkt im Ordner usbfmt-backup, keine Verknüpfung hinaus)? Liefert den Ort. */
static const imgloc_t *img_path_loc(const char *path, const imgloc_t *loc, int n) {
    const char *slash = strrchr(path, '/');
    if (!slash || !ends_with(path, ".zst") || strstr(path, "/../") || slash[1] == '.') return NULL;
    char *rp = realpath(path, NULL);
    const imgloc_t *hit = NULL;
    struct stat sb;
    for (int i = 0; rp && i < n && !hit; i++) {
        char *rd = realpath(loc[i].dir, NULL);
        size_t l = rd ? strlen(rd) : 0;
        if (rd && !strncmp(rp, rd, l) && rp[l] == '/' && !strchr(rp + l + 1, '/') && !stat(rp, &sb) && S_ISREG(sb.st_mode))
            hit = &loc[i];
        free(rd);
    }
    free(rp);
    return hit;
}

static long file_in(void *ctx, void *p, size_t n) {
    size_t r = fread(p, 1, n, ctx);
    return r ? (long)r : ferror((FILE *)ctx) ? -1 : 0;
}

typedef struct { FILE *f; int err; } fout_t;

static int file_out(void *ctx, const void *p, size_t n) {
    fout_t *o = ctx;
    if (fwrite(p, 1, n, o->f) == n) return 0;
    o->err = errno ? errno : EIO;
    return -1;
}

static void write_error(int e, char *err, size_t n) {
    if (e == ENOSPC) snprintf(err, n, "%s", T("Am Ziel ist nicht mehr genug Platz frei."));
    else if (e == EFBIG) snprintf(err, n, "%s", T("Die Datei ist zu groß für das Dateisystem des Ziels (FAT32: höchstens 4 GB)."));
    else snprintf(err, n, T("Schreiben der Sicherung fehlgeschlagen: %s"), strerror(e));
}

/* Angaben für den Kopf der Sicherung */
static void img_info_of(const usbdev_t *d, ib_info_t *info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->model, sizeof(info->model), "%s", d->model);
    snprintf(info->label, sizeof(info->label), "%s", d->label);
    snprintf(info->fs, sizeof(info->fs), "%s", d->format);
    snprintf(info->ident, sizeof(info->ident), "%s", d->ident);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(info->created, sizeof(info->created), "%Y-%m-%d %H:%M", &tmv);
}

static void img_result(sbuf_t *res, const char *file, const char *name, const ib_result_t *r) {
    sb_add(res, "{\"file\":");
    sb_str(res, file);
    sb_add(res, ",\"name\":");
    sb_str(res, name);
    sb_add(res, ",\"size\":%llu,\"used\":%llu,\"bytes\":%llu,\"sha256\":\"%s\",\"sha256_data\":\"%s\"}",
           (unsigned long long)r->size, (unsigned long long)r->used, (unsigned long long)r->file_bytes, r->sha_file,
           r->sha_img);
}


/* ------------------------------------------------------------ Fortsetzbare Image-Sicherung
 * Neben <Name>.img.zst.part liegt <Name>.img.zst.part.state: der letzte Merkpunkt (Dateilänge, Position, Zwischen-SHA-256, Kennung des
 * Laufwerks). Nach einem Abbruch ("behalten"), einem Fehler oder einem Neustart der PS5 setzt POST /api/backup/resume dort fort. */
#define PART_MAGIC "UFBKRES1"

typedef struct {
    char       magic[8];
    uint32_t   struct_size, nframes;
    char       ident[64], model[128], label[48], fs[16], created[24];
    uint64_t   dev_size;
    uint32_t   ss;
    int64_t    when;
    ib_ckpt_t  c;   /* c.frames zeigt ins Leere: die Rahmen stehen hinter dieser Struktur */
} part_state_t;

#define PART_FRAMES_MAX 100000u

static int part_state_write(const char *state_path, const part_state_t *ps, const ib_frame_t *frames) {
    char tmp[800];
    snprintf(tmp, sizeof(tmp), "%.780s.tmp", state_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(ps, sizeof(*ps), 1, f) == 1 && (!ps->nframes || fwrite(frames, sizeof(*frames), ps->nframes, f) == ps->nframes) &&
             fflush(f) == 0 && (fsync(fileno(f)) == 0 || errno == EINVAL);
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, state_path) != 0) ok = 0;
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}

/* Zustand lesen; *frames (malloc) hängt der Aufrufer an ps->c.frames. 0 = ok */
static int part_state_read(const char *state_path, part_state_t *ps, ib_frame_t **frames) {
    *frames = NULL;
    FILE *f = fopen(state_path, "rb");
    if (!f) return -1;
    int ok = fread(ps, sizeof(*ps), 1, f) == 1 && !memcmp(ps->magic, PART_MAGIC, 8) && ps->struct_size == sizeof(*ps) &&
             ps->nframes <= PART_FRAMES_MAX && ps->c.used > 0 && ps->c.file_bytes > 0;
    if (ok && ps->nframes) {
        *frames = malloc((size_t)ps->nframes * sizeof(**frames));
        ok = *frames && fread(*frames, sizeof(**frames), ps->nframes, f) == ps->nframes;
    }
    fclose(f);
    if (!ok) { free(*frames); *frames = NULL; return -1; }
    ps->c.frames = *frames;
    ps->c.nframes = (int)ps->nframes;
    return 0;
}

typedef struct {
    FILE *f;
    char  state[780];
    const ib_info_t *info;
    const usbdev_t  *dev;
    int   saved;
} part_ctx_t;

static int part_checkpoint(void *ctx, const ib_ckpt_t *c) {
    part_ctx_t *p = ctx;
    if (fflush(p->f) != 0 || (fsync(fileno(p->f)) != 0 && errno != EINVAL)) return -1;
    part_state_t ps;
    memset(&ps, 0, sizeof(ps));
    memcpy(ps.magic, PART_MAGIC, 8);
    ps.struct_size = sizeof(ps);
    ps.nframes = (uint32_t)c->nframes;
    if (ps.nframes > PART_FRAMES_MAX) return -1;
    snprintf(ps.ident, sizeof(ps.ident), "%s", p->dev->ident);
    snprintf(ps.model, sizeof(ps.model), "%s", p->info->model);
    snprintf(ps.label, sizeof(ps.label), "%s", p->info->label);
    snprintf(ps.fs, sizeof(ps.fs), "%s", p->info->fs);
    snprintf(ps.created, sizeof(ps.created), "%s", p->info->created);
    ps.dev_size = p->dev->size;
    ps.ss = p->dev->ss;
    ps.when = (int64_t)time(NULL);
    ps.c = *c;
    ps.c.frames = NULL;
    if (part_state_write(p->state, &ps, c->frames) != 0) return -1;
    p->saved = 1;
    return 0;
}


/* Gibt es zu dieser Image-Datei schon eine behaltene Teilsicherung (<Datei>.part)? */
static int part_taken(const char *file) {
    char p[1100];
    snprintf(p, sizeof(p), "%s.part", file);
    return access(p, F_OK) == 0;
}

static void *backup_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    char err[256] = "", title[160], name[160], file[700], part[720], side[720], s1[24], s2[24], statef[780];
    sbuf_t msg = {0}, res = {0};
    ib_info_t info;
    if (getenv("USBFMT_FRAME_MAX")) ib_frame_max = strtoull(getenv("USBFMT_FRAME_MAX"), NULL, 10);   /* Tests: kleine Rahmen, viele Merkpunkte */
    if (getenv("USBFMT_CKPT_EVERY")) { ib_ckpt_bytes = strtoull(getenv("USBFMT_CKPT_EVERY"), NULL, 10); ib_ckpt_secs = 0; }
    part_state_t ps;
    ib_frame_t *frames = NULL;
    const int resuming = j->resume_part[0] != 0;
    if (resuming) {
        /* fortsetzen: Pfade und Angaben aus dem Merkpunkt, nicht neu bilden */
        snprintf(part, sizeof(part), "%s", j->resume_part);
        snprintf(statef, sizeof(statef), "%s.state", part);
        snprintf(file, sizeof(file), "%.*s", (int)(strlen(part) - 5), part);   /* ohne ".part" */
        snprintf(name, sizeof(name), "%.159s", strrchr(file, '/') ? strrchr(file, '/') + 1 : file);
        snprintf(side, sizeof(side), "%s.sha256", file);
        if (part_state_read(statef, &ps, &frames) != 0) {
            sb_tr(&msg, "Der Merkpunkt der Teilsicherung ist nicht lesbar – bitte verwerfen und neu sichern.");
            job_done(j, 0, &msg, &res, 0);
            return NULL;
        }
        if ((ps.ident[0] && j->dev.ident[0] && strcmp(ps.ident, j->dev.ident) != 0) || ps.dev_size != j->dev.size || ps.ss != j->dev.ss) {
            free(frames);
            sb_tr(&msg, "Die Teilsicherung gehört zu einem anderen Datenträger.");
            job_done(j, 0, &msg, &res, 0);
            return NULL;
        }
        memset(&info, 0, sizeof(info));
        snprintf(info.model, sizeof(info.model), "%s", ps.model);
        snprintf(info.label, sizeof(info.label), "%s", ps.label);
        snprintf(info.fs, sizeof(info.fs), "%s", ps.fs);
        snprintf(info.ident, sizeof(info.ident), "%s", ps.ident);
        snprintf(info.created, sizeof(info.created), "%s", ps.created);
    } else {
        img_info_of(&j->dev, &info);
        ib_file_name(&info, name, sizeof(name));
        if (mkdir(j->path, 0777) != 0 && errno != EEXIST) {
            sb_tr(&msg, "Der Ordner %s lässt sich nicht anlegen: %s", j->path, strerror(errno));
            job_done(j, 0, &msg, &res, 0);
            return NULL;
        }
        /* derselbe Name in derselben Minute schon da: Nummer anhängen */
        snprintf(file, sizeof(file), "%s/%s", j->path, name);
        for (int k = 2; k < 100 && (access(file, F_OK) == 0 || part_taken(file)); k++)
            snprintf(file, sizeof(file), "%s/%.*s_%d%s", j->path, (int)(strlen(name) - strlen(IMG_EXT)), name, k, IMG_EXT);
        snprintf(name, sizeof(name), "%s", strrchr(file, '/') + 1);
        snprintf(part, sizeof(part), "%s.part", file);
        snprintf(side, sizeof(side), "%s.sha256", file);
        snprintf(statef, sizeof(statef), "%s.state", part);
    }

    int fd = job_acquire(j, err, sizeof(err));   /* eingebunden: kurz abmelden, sonst sicherte man halb Geschriebenes */
    if (fd < 0) { free(frames); sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    int ofd = open(part, resuming ? O_RDWR : (O_WRONLY | O_CREAT | O_EXCL), 0666);
    fout_t o = {ofd >= 0 ? fdopen(ofd, resuming ? "r+b" : "wb") : NULL, 0};
    if (!o.f) {
        free(frames);
        sb_tr(&msg, "Die Datei %s lässt sich nicht anlegen: %s", part, strerror(errno));
        if (ofd >= 0) close(ofd);
        job_release(fd);
        job_done(j, 0, &msg, &res, 1);
        return NULL;
    }
    if (resuming) {   /* auf den Merkpunkt zurückschneiden: danach geschriebene Bytes stammen von nach dem Merkpunkt */
        struct stat ob;
        if (fstat(fileno(o.f), &ob) != 0 || (uint64_t)ob.st_size < ps.c.file_bytes || ftruncate(fileno(o.f), (off_t)ps.c.file_bytes) != 0 ||
            fseeko(o.f, (off_t)ps.c.file_bytes, SEEK_SET) != 0) {
            free(frames);
            fclose(o.f);
            sb_tr(&msg, "Die Teilsicherung ist kürzer als ihr Merkpunkt – bitte verwerfen und neu sichern.");
            job_release(fd);
            job_done(j, 0, &msg, &res, 1);
            return NULL;
        }
    }
    setvbuf(o.f, NULL, _IOFBF, 1 << 20);
    set_flags(1, 0);
    st.total_ptr = &info.used;   /* ib_backup setzt info.used nach der Belegungssuche */
    ib_result_t r;
    part_ctx_t pc = {o.f, "", &info, &j->dev, 0};
    snprintf(pc.state, sizeof(pc.state), "%s", statef);
    ib_resume_t rs = {part_checkpoint, &pc, resuming ? &ps.c : NULL, j->thorough};
    int rc = ib_backup_ex(fd, j->dev.ss, j->dev.size, &info, file_out, &o, on_progress, &cancel_requested, &r, err, sizeof(err), &rs);
    free(frames);
    st.total_ptr = NULL;
    set_flags(0, 0);
    job_release(fd);   /* die PS5 bindet den Stick gleich wieder ein */
    if (fflush(o.f) != 0 && !o.err) o.err = errno ? errno : EIO;
    if (fsync(fileno(o.f)) != 0 && !o.err && errno != EINVAL) o.err = errno;
    if (fclose(o.f) != 0 && !o.err) o.err = errno ? errno : EIO;
    if (o.err && !r.cancelled) { rc = -1; write_error(o.err, err, sizeof(err)); }
    FILE *h = !rc && !r.cancelled ? fopen(side, "w") : NULL;
    int hok = h && fprintf(h, "%s  %s\n", r.sha_file, name) > 0;
    if (h && fclose(h) != 0) hok = 0;
    if (!rc && !r.cancelled && (!hok || rename(part, file) != 0)) {
        rc = -1;
        snprintf(err, sizeof(err), T("Die Datei %s lässt sich nicht anlegen: %s"), hok ? file : side, strerror(errno));
    }
    if (rc || r.cancelled) {
        /* Teilsicherung behalten, wenn es einen Merkpunkt gibt und der Nutzer es wollte (Abbrechen) oder ein Fehler sie unterbrach
         * (Platz voll, Laufwerk weg): dann lässt sich später fortsetzen. Sonst alles wegräumen. */
        const int keep = (pc.saved || resuming) && (rc != 0 || __atomic_load_n(&cancel_keep, __ATOMIC_RELAXED));
        if (!keep) { unlink(part); unlink(statef); }
        if (h) unlink(side);   /* auch, wenn erst das Umbenennen scheiterte: keine .sha256 ohne Sicherung */
        if (rc) sb_tr(&msg, "Sichern fehlgeschlagen: %s", err);
        else sb_tr(&msg, "Abgebrochen.");
        if (keep) sb_add(&msg, " %s", T("Die Teilsicherung bleibt erhalten und lässt sich fortsetzen."));
        job_done(j, 0, &msg, &res, 1);
        return NULL;
    }
    unlink(statef);   /* fertig: kein Merkpunkt mehr nötig */
    sb_tr(&msg, "%s gesichert: %s belegt, Datei %s.", dev_title(&j->dev, title, sizeof(title)),
          size_text(r.used, s1, sizeof(s1)), size_text(r.file_bytes, s2, sizeof(s2)));
    sb_add(&msg, " %s", file);
    img_result(&res, file, name, &r);
    job_done(j, 1, &msg, &res, 1);
    return NULL;
}

/* Sicherung als Download: POST /api/backup {"dest":"download"} legt ein Einmal-Zeichen an, GET
 * /api/backup/download?token=... liefert dann den Datenstrom. Eine fremde Webseite kann das Zeichen nicht lesen und
 * so keinen Stick abmelden lassen. */
static struct {
    char   token[33];
    job_t *job;
    time_t when;
} dl;

typedef struct {
    int  c, sent_head, broken;
    char name[160];
} chunk_t;

static int send_ok(int c, const void *p, size_t len) {
    const char *b = p;
    while (len) {
        ssize_t n = send(c, b, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        b += n;
        len -= (size_t)n;
    }
    return 0;
}

static int chunk_out(void *ctx, const void *p, size_t n) {
    chunk_t *k = ctx;
    char h[600];
    if (!k->sent_head) {
        int hl = snprintf(h, sizeof(h),
                          "HTTP/1.1 200 OK\r\nContent-Type: application/zstd\r\n"
                          "Content-Disposition: attachment; filename=\"%s\"\r\nTransfer-Encoding: chunked\r\n"
                          "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                          k->name);
        if (send_ok(k->c, h, (size_t)hl)) { k->broken = 1; return -1; }
        k->sent_head = 1;
    }
    int hl = snprintf(h, sizeof(h), "%zx\r\n", n);
    if (send_ok(k->c, h, (size_t)hl) || send_ok(k->c, p, n) || send_ok(k->c, "\r\n", 2)) { k->broken = 1; return -1; }
    return 0;
}

static void api_download(int c, const char *query) {
    char tok[40] = "";
    const char *t = query ? strstr(query, "token=") : NULL;
    if (t) snprintf(tok, sizeof(tok), "%.*s", (int)strcspn(t + 6, "&"), t + 6);
    pthread_mutex_lock(&lock);
    job_t *j = NULL;
    if (dl.job && tok[0] && !strcmp(tok, dl.token)) {
        if (time(NULL) - dl.when <= 120) j = dl.job;
        else job_free(dl.job);
        dl.job = NULL;
        dl.token[0] = 0;
    }
    pthread_mutex_unlock(&lock);
    if (!j) { respond_error(c, 404, "Der Download ist nicht mehr gültig – bitte neu starten."); return; }
    if (claim_job(c, j)) return;
    i18n_set(j->lang);
    struct timeval tv = {300, 0};   /* der Browser fragt womöglich erst nach dem Speicherort */
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    char err[256] = "", title[160], s1[24], s2[24];
    sbuf_t msg = {0}, res = {0};
    ib_info_t info;
    img_info_of(&j->dev, &info);
    chunk_t k = {c, 0, 0, ""};
    ib_file_name(&info, k.name, sizeof(k.name));
    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) {
        respond_error(c, 409, err);
        sb_add(&msg, "%s", err);
        job_done(j, 0, &msg, &res, 0);
        return;
    }
    set_flags(1, 0);
    st.total_ptr = &info.used;   /* ib_backup setzt info.used nach der Belegungssuche */
    ib_result_t r;
    int rc = ib_backup(fd, j->dev.ss, j->dev.size, &info, chunk_out, &k, on_progress, &cancel_requested, &r, err,
                       sizeof(err));
    st.total_ptr = NULL;
    set_flags(0, 0);
    job_release(fd);
    if (!rc && !r.cancelled && send_ok(c, "0\r\n\r\n", 5)) k.broken = 1;
    if (k.broken) {
        sb_tr(&msg, "Der Download wurde abgebrochen (Verbindung zum Browser getrennt).");
        job_done(j, 0, &msg, &res, 1);
        return;
    }
    if (!k.sent_head) respond_error(c, 500, rc ? err : T("Abgebrochen."));
    if (rc || r.cancelled) {
        if (rc) sb_tr(&msg, "Sichern fehlgeschlagen: %s", err);
        else sb_tr(&msg, "Abgebrochen.");
        job_done(j, 0, &msg, &res, 1);   /* angefangener Download bleibt unvollständig (ohne Endstück) */
        return;
    }
    sb_tr(&msg, "%s gesichert: %s belegt, Datei %s.", dev_title(&j->dev, title, sizeof(title)),
          size_text(r.used, s1, sizeof(s1)), size_text(r.file_bytes, s2, sizeof(s2)));
    img_result(&res, "", k.name, &r);
    job_done(j, 1, &msg, &res, 1);
}

static void api_backup(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX), replug;
    usbdev_t d;
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }
    const char *dest = kv_get(kv, n, "dest");
    if (!dest) { respond_error(c, 400, "dest ist erforderlich"); return; }
    if (request_device(c, kv, n, &d) || request_replug(c, kv, n, &d, &replug)) return;
    job_t *j = new_job(c, JOB_BACKUP);
    if (!j) return;
    j->dev = d;
    j->replug = replug;
    if (!strcmp(dest, "download")) {
        uint8_t rnd[16];
        arc4random_buf(rnd, sizeof(rnd));
        ib_info_t info;
        char name[160];
        img_info_of(&d, &info);
        ib_file_name(&info, name, sizeof(name));
        pthread_mutex_lock(&lock);
        job_free(dl.job);
        dl.job = j;
        dl.when = time(NULL);
        for (int i = 0; i < 16; i++) snprintf(dl.token + 2 * i, 3, "%02x", rnd[i]);
        sbuf_t b = {0};
        sb_add(&b, "{\"token\":\"%s\",\"name\":", dl.token);
        pthread_mutex_unlock(&lock);
        sb_str(&b, name);
        sb_add(&b, "}");
        respond_json(c, 200, &b);
        free(b.p);
        return;
    }
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    int nl = loc ? img_locations(loc, MAX_LOCS) : 0;
    const imgloc_t *at = NULL;
    for (int i = 0; i < nl && !at; i++)
        if (!strcmp(loc[i].dir, dest)) at = &loc[i];
    const char *bad = !at ? N_("Unbekanntes Ziel – Liste neu laden.")
                    : !strcmp(at->device, d.name) ? N_("Die Sicherung kann nicht auf dem Datenträger liegen, der gesichert wird.")
                    : NULL;
    struct statvfs vs;
    if (!bad && (statvfs(at->root, &vs) != 0 || (uint64_t)vs.f_bavail * vs.f_frsize < 64 * MIB))
        bad = N_("Am Ziel ist zu wenig Platz frei (mindestens 64 MB).");
    if (!bad) snprintf(j->path, sizeof(j->path), "%s", at->dir);
    free(loc);
    if (bad) { job_free(j); respond_error(c, 400, bad); return; }
    start_job(c, j, backup_worker);
}

/* Image zurückschreiben (j->rd ist geöffnet, der Kopf geprüft) - für Datei und Hochladen */
/* Ergebnis eines Wiederherstellens melden; out (falls gesetzt) bekommt die Meldung schon vorher - das Hochladen
 * antwortet mit ihr, ohne den Zustand zu lesen, den inzwischen ein anderer Auftrag überschrieben haben kann */
static int restore_done(job_t *j, int ok, sbuf_t *msg, sbuf_t *res, int refresh, char *out, size_t on) {
    if (out && on) snprintf(out, on, "%s", msg->p ? msg->p : "");
    job_done(j, ok, msg, res, refresh);
    return ok;
}

static int restore_run(job_t *j, char *out, size_t on) {
    char err[256] = "", title[160], s1[24];
    sbuf_t msg = {0}, res = {0};
    int fd = job_acquire(j, err, sizeof(err));
    if (fd < 0) {   /* Image und Leser gehören dem Auftrag: schließen, sonst bleiben Datei und Puffer offen */
        ib_close(j->rd);
        j->rd = NULL;
        if (j->img) fclose(j->img);
        j->img = NULL;
        sb_add(&msg, "%s", err);
        return restore_done(j, 0, &msg, &res, 0, out, on);
    }
    set_flags(1, 0);
    ib_result_t r;
    int rc = ib_restore(j->rd, fd, fd, j->dev.ss, j->dev.size, on_progress, &cancel_requested, &r, err, sizeof(err));
    set_flags(0, 0);
    job_release(fd);   /* die PS5 bindet ihn danach selbst ein */
    ib_close(j->rd);
    j->rd = NULL;
    if (j->img) fclose(j->img);
    j->img = NULL;
    if (!rc && r.cancelled) {
        if (r.used) sb_tr(&msg, "Abgebrochen – der Datenträger ist nur teilweise beschrieben.");
        else sb_tr(&msg, "Abgebrochen – der Datenträger wurde nicht verändert.");
        return restore_done(j, 0, &msg, &res, 1, out, on);
    }
    if (rc) {
        sb_tr(&msg, "Wiederherstellen fehlgeschlagen: %s", err);
        if (r.used) sb_add(&msg, " %s", T("Der Datenträger ist bereits teilweise überschrieben."));
        return restore_done(j, 0, &msg, &res, 1, out, on);
    }
    sb_tr(&msg, "Image auf %s wiederhergestellt und geprüft (%s geschrieben).", dev_title(&j->dev, title, sizeof(title)),
          size_text(r.used, s1, sizeof(s1)));
    sb_add(&msg, " %s", T("Die PS5 bindet ihn gleich selbst ein."));
    sb_add(&res, "{\"size\":%llu,\"written\":%llu,\"sha256_data\":\"%s\"}", (unsigned long long)r.size,
           (unsigned long long)r.used, r.sha_img);
    return restore_done(j, 1, &msg, &res, 1, out, on);
}

static void *restore_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    restore_run(j, NULL, 0);
    return NULL;
}

/* Kopf gelesen: passt das Image auf den Datenträger (auch auf den echten Teil eines gefälschten)? 0 = ja */
static int restore_fits(int c, const ib_info_t *info, const usbdev_t *d) {
    char m[256], a[24], b[24];
    uint64_t real = fake_get(d);
    if (info->size > d->size) {
        snprintf(m, sizeof(m), T("Das Image (%s) ist größer als der Datenträger (%s)."), size_text(info->size, a, sizeof(a)),
                 size_text(d->size, b, sizeof(b)));
    } else if (real && info->size > real) {
        snprintf(m, sizeof(m), T("Dieser Stick hat nur etwa %s echten Speicher – das Image (%s) passt nicht hinein."),
                 size_text(real, a, sizeof(a)), size_text(info->size, b, sizeof(b)));
    } else {
        return 0;
    }
    respond_error(c, 400, m);
    return -1;
}

static void api_restore(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX), replug;
    usbdev_t d;
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }
    const char *path = kv_get(kv, n, "path");
    if (!path) { respond_error(c, 400, "path ist erforderlich"); return; }
    if (request_device(c, kv, n, &d) || request_replug(c, kv, n, &d, &replug)) return;
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    int nl = loc ? img_locations(loc, MAX_LOCS) : 0;
    const imgloc_t *at = img_path_loc(path, loc, nl);
    const char *bad = !at ? N_("Image-Datei nicht gefunden – Liste neu laden.")
                    : !strcmp(at->device, d.name) ? N_("Die Image-Datei liegt auf dem Datenträger, der überschrieben werden soll.")
                    : NULL;
    free(loc);
    if (bad) { respond_error(c, 400, bad); return; }
    char err[256] = "";
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, sizeof(err), T("Die Datei %s lässt sich nicht öffnen: %s"), path, strerror(errno)); respond_error(c, 400, err); return; }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    ib_info_t info;
    ib_reader_t *rd = ib_open(file_in, f, &info, err, sizeof(err));
    if (!rd) { fclose(f); respond_error(c, 400, err); return; }
    if (restore_fits(c, &info, &d)) { ib_close(rd); fclose(f); return; }
    job_t *j = new_job(c, JOB_RESTORE);
    if (!j) { ib_close(rd); fclose(f); return; }
    j->dev = d;
    j->replug = replug;
    j->img = f;
    j->rd = rd;
    snprintf(j->path, sizeof(j->path), "%s", path);
    start_job(c, j, restore_worker);
}

/* Wert aus der Adresse (?a=b&c=d), %XX und + entschlüsselt */
static int query_get(const char *q, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        size_t i = 0;
        for (p += kl + 1; *p && *p != '&' && i + 1 < n; p++) {
            unsigned v;
            if (*p == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1) { out[i++] = (char)v; p += 2; }
            else out[i++] = *p == '+' ? ' ' : *p;
        }
        out[i] = 0;
        return 0;
    }
    return -1;
}

typedef struct {
    int                c;
    const char        *pre;      /* schon mit dem Kopf empfangene Bytes */
    size_t             pre_len, pre_pos;
    unsigned long long left;     /* noch zu empfangen */
} upin_t;

static long upload_in(void *ctx, void *p, size_t n) {
    upin_t *u = ctx;
    if (u->pre_pos < u->pre_len) {
        size_t k = u->pre_len - u->pre_pos < n ? u->pre_len - u->pre_pos : n;
        memcpy(p, u->pre + u->pre_pos, k);
        u->pre_pos += k;
        return (long)k;
    }
    if (!u->left) return 0;
    if (n > u->left) n = (size_t)u->left;
    ssize_t r;
    do r = recv(u->c, p, n, 0); while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;
    u->left -= (unsigned long long)r;
    return (long)r;
}

/* Image hochladen und dabei zurückschreiben (aus dem Browser am PC/Handy) - läuft in dieser Verbindung */
static void api_upload(int c, const char *query, const char *pre, size_t have, unsigned long long clen) {
    kv_t kv[4];
    int n = 0;
    static const char *const keys[] = {"device", "size", "ident", "replug"};
    for (int i = 0; i < 4; i++)
        if (!query_get(query, keys[i], kv[n].val, sizeof(kv[n].val))) {
            snprintf(kv[n].key, sizeof(kv[n].key), "%s", keys[i]);
            if (!strcmp(keys[i], "replug") && !strcmp(kv[n].val, "1")) strcpy(kv[n].val, "true");
            n++;
        }
    usbdev_t d;
    int replug;
    if (request_device(c, kv, n, &d) || request_replug(c, kv, n, &d, &replug)) return;
    struct timeval tv = {300, 0};   /* während des Abmeldens/Einsteckens kommen keine Daten */
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    upin_t u = {c, pre, have < clen ? have : (size_t)clen, 0, clen - (have < clen ? have : clen)};
    char err[256] = "";
    ib_info_t info;
    ib_reader_t *rd = ib_open(upload_in, &u, &info, err, sizeof(err));
    if (!rd) { respond_error(c, 400, err); return; }
    if (restore_fits(c, &info, &d)) { ib_close(rd); return; }
    job_t *j = new_job(c, JOB_RESTORE);
    if (!j) { ib_close(rd); return; }
    j->dev = d;
    j->replug = replug;
    j->rd = rd;
    if (claim_job(c, j)) return;
    char m[512];
    if (restore_run(j, m, sizeof(m))) respond_ok(c, 200);
    else respond_error(c, 500, m);
}

/* passt die Datei zu ihrer "<path>.sha256" daneben? 1 = ja, 0 = keine (verwertbare) da, -1 = andere Prüfsumme */
static int img_side_check(const char *path, const char *sha) {
    char side[600], line[160];
    if (snprintf(side, sizeof(side), "%s.sha256", path) >= (int)sizeof(side)) return 0;
    FILE *h = fopen(side, "r");
    if (!h) return 0;
    int r = 0;   /* nicht im Format "<hex>  <name>" (anderes Werkzeug, UTF-16, Kopfzeile): wie keine - nicht "beschädigt" */
    if (fgets(line, sizeof(line), h)) {
        const char *s = !strncmp(line, "\xEF\xBB\xBF", 3) ? line + 3 : line;   /* UTF-8-BOM */
        int hex = strlen(s) >= 64 && !isxdigit((unsigned char)s[64]);
        for (int i = 0; hex && i < 64; i++) hex = isxdigit((unsigned char)s[i]) != 0;
        if (hex) {   /* Windows-Werkzeuge schreiben groß */
            r = 1;
            for (int i = 0; i < 64; i++)
                if (tolower((unsigned char)s[i]) != sha[i]) r = -1;
        }
    }
    fclose(h);
    return r;
}

/* Sicherung nur prüfen (kein Datenträger beteiligt): ganz entpacken, Prüfsumme im Image und die .sha256 daneben */
static void *verify_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    char err[256] = "", s1[24], s2[24];
    sbuf_t msg = {0}, res = {0};
    set_flags(1, 0);
    ib_result_t r;
    int rc = ib_verify(j->rd, on_progress, &cancel_requested, &r, err, sizeof(err));
    set_flags(0, 0);
    ib_close(j->rd);
    j->rd = NULL;
    fclose(j->img);
    j->img = NULL;
    const char *name = strrchr(j->path, '/') ? strrchr(j->path, '/') + 1 : j->path;
    if (!rc && r.cancelled) { sb_tr(&msg, "Abgebrochen."); job_done(j, 0, &msg, &res, 0); return NULL; }
    if (rc) { sb_tr(&msg, "Das Image ist fehlerhaft: %s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    const int side = img_side_check(j->path, r.sha_file);
    sb_add(&res, "{\"name\":");
    sb_str(&res, name);
    sb_add(&res, ",\"size\":%llu,\"used\":%llu,\"bytes\":%llu,\"sha256\":\"%s\",\"sha256_data\":\"%s\",\"checked\":%s,"
           "\"side\":\"%s\"}", (unsigned long long)r.size, (unsigned long long)r.used, (unsigned long long)r.file_bytes,
           r.sha_file, r.sha_img, r.checked ? "true" : "false", side > 0 ? "ok" : side < 0 ? "bad" : "none");
    if (side < 0) {
        sb_tr(&msg, "Die Datei passt nicht zu ihrer .sha256-Datei – sie wurde verändert oder beschädigt.");
        job_done(j, 0, &msg, &res, 0);
        return NULL;
    }
    if (r.checked)
        sb_tr(&msg, "Image in Ordnung: %s belegt von %s geprüft, die Prüfsumme im Image stimmt.",
              size_text(r.used, s1, sizeof(s1)), size_text(r.size, s2, sizeof(s2)));
    else
        sb_tr(&msg, "Das Image lässt sich vollständig entpacken (%s). Es stammt nicht von diesem Programm und hat keine "
                    "eigene Prüfsumme.", size_text(r.size, s1, sizeof(s1)));
    if (side > 0) sb_add(&msg, " %s", T("Die .sha256-Datei passt dazu."));
    job_done(j, 1, &msg, &res, 0);
    return NULL;
}

/* Pfad aus der Anfrage: eine Sicherung direkt in einem der Ordner usbfmt-backup? 0 = ja (Antwort sonst gesendet) */
static int request_image(int c, const char *body, char *path, size_t n) {
    kv_t kv[KV_MAX];
    int k = json_parse(body, kv, KV_MAX);
    const char *p = k < 0 ? NULL : kv_get(kv, k, "path");
    if (!p) { respond_error(c, 400, "path ist erforderlich"); return -1; }
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    int nl = loc ? img_locations(loc, MAX_LOCS) : 0;
    const imgloc_t *at = img_path_loc(p, loc, nl);
    free(loc);
    /* weiter mit dem geprüften echten Pfad - ein Verweis (Symlink) zeigte sonst beim Löschen der .sha256 woandershin */
    char *rp = at ? realpath(p, NULL) : NULL;
    if (!rp || strlen(rp) >= n) {
        free(rp);
        respond_error(c, 400, "Image-Datei nicht gefunden – Liste neu laden.");
        return -1;
    }
    snprintf(path, n, "%s", rp);
    free(rp);
    return 0;
}

static void api_images_verify(int c, const char *body) {
    char path[512], err[256] = "";
    if (request_image(c, body, path, sizeof(path))) return;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, sizeof(err), T("Die Datei %s lässt sich nicht öffnen: %s"), path, strerror(errno)); respond_error(c, 400, err); return; }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    ib_info_t info;
    ib_reader_t *rd = ib_open(file_in, f, &info, err, sizeof(err));
    if (!rd) { fclose(f); respond_error(c, 400, err); return; }
    job_t *j = new_job(c, JOB_VERIFY);
    if (!j) { ib_close(rd); fclose(f); return; }
    j->img = f;
    j->rd = rd;
    snprintf(j->path, sizeof(j->path), "%s", path);
    start_job(c, j, verify_worker);
}

/* Sicherung samt .sha256 löschen - nicht während eines Auftrags (der könnte gerade diese Datei lesen oder schreiben) */
static void api_images_delete(int c, const char *body) {
    char path[512], side[600];
    if (request_image(c, body, path, sizeof(path))) return;
    pthread_mutex_lock(&lock);
    int busy = st.busy;
    pthread_mutex_unlock(&lock);
    if (busy) { respond_error(c, 409, "Erst den laufenden Auftrag abwarten."); return; }
    if (unlink(path) != 0) {
        char err[700];
        snprintf(err, sizeof(err), T("Die Datei %s lässt sich nicht löschen: %s"), path, strerror(errno));
        respond_error(c, 500, err);
        return;
    }
    snprintf(side, sizeof(side), "%s.sha256", path);
    unlink(side);   /* fehlt sie, ist das kein Fehler */
    respond_ok(c, 200);
}

/* Teilsicherung (<Name>.img.zst.part) direkt in einem der Ordner usbfmt-backup? Wie img_path_loc für ".part" */
static const imgloc_t *part_path_loc(const char *path, const imgloc_t *loc, int n) {
    const char *slash = strrchr(path, '/');
    if (!slash || !ends_with(path, ".img.zst.part") || strstr(path, "/../") || slash[1] == '.') return NULL;
    char *rp = realpath(path, NULL);
    const imgloc_t *hit = NULL;
    struct stat sb;
    for (int i = 0; rp && i < n && !hit; i++) {
        char *rd = realpath(loc[i].dir, NULL);
        size_t l = rd ? strlen(rd) : 0;
        if (rd && !strncmp(rp, rd, l) && rp[l] == '/' && !strchr(rp + l + 1, '/') && !stat(rp, &sb) && S_ISREG(sb.st_mode))
            hit = &loc[i];
        free(rd);
    }
    free(rp);
    return hit;
}

/* Pfad einer Teilsicherung aus der Anfrage (geprüft, echter Pfad); 0 = ok, sonst ist geantwortet */
static int request_part(int c, kv_t *kv, int n, char *path, size_t pn, imgloc_t **locs, const imgloc_t **at) {
    const char *p = kv_get(kv, n, "path");
    if (!p) { respond_error(c, 400, "path ist erforderlich"); return -1; }
    *locs = calloc(MAX_LOCS, sizeof(**locs));
    int nl = *locs ? img_locations(*locs, MAX_LOCS) : 0;
    *at = part_path_loc(p, *locs, nl);
    char *rp = *at ? realpath(p, NULL) : NULL;
    if (!rp || strlen(rp) >= pn) {
        free(rp);
        free(*locs);
        *locs = NULL;
        respond_error(c, 400, "Teilsicherung nicht gefunden – Liste neu laden.");
        return -1;
    }
    snprintf(path, pn, "%s", rp);
    free(rp);
    return 0;
}

/* POST /api/backup/resume {"path":"<...img.zst.part>","device":"da1","size":..,"ident":"..","replug":true,"thorough":true}:
 * Image-Sicherung ab dem letzten Merkpunkt fortsetzen */
static void api_backup_resume(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX), replug;
    usbdev_t d;
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }
    char path[720], statef[780];
    imgloc_t *locs = NULL;
    const imgloc_t *at = NULL;
    if (request_part(c, kv, n, path, sizeof(path), &locs, &at)) return;
    snprintf(statef, sizeof(statef), "%s.state", path);
    const char *bad = access(statef, F_OK) != 0 ? N_("Zur Teilsicherung fehlt der Merkpunkt – bitte verwerfen und neu sichern.") : NULL;
    char at_dev[sizeof(at->device)] = "";   /* at zeigt in locs: vor dem Freigeben den Namen merken */
    if (at) snprintf(at_dev, sizeof(at_dev), "%s", at->device);
    free(locs);
    if (bad) { respond_error(c, 404, bad); return; }
    if (request_device(c, kv, n, &d) || request_replug(c, kv, n, &d, &replug)) return;
    if (at && !strcmp(at_dev, d.name)) { respond_error(c, 400, N_("Die Sicherung kann nicht auf dem Datenträger liegen, der gesichert wird.")); return; }
    job_t *j = new_job(c, JOB_BACKUP);
    if (!j) return;
    j->dev = d;
    j->replug = replug;
    j->thorough = kv_true(kv, n, "thorough");
    snprintf(j->resume_part, sizeof(j->resume_part), "%s", path);
    start_job(c, j, backup_worker);
}

/* POST /api/images/discard {"path":"<...img.zst.part>"}: Teilsicherung mit Merkpunkt löschen (nicht während eines Auftrags) */
static void api_images_discard(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    if (n < 0) { respond_error(c, 400, "Ungültige Anfrage"); return; }
    char path[720], statef[780];
    imgloc_t *locs = NULL;
    const imgloc_t *at = NULL;
    if (request_part(c, kv, n, path, sizeof(path), &locs, &at)) return;
    free(locs);
    pthread_mutex_lock(&lock);
    int busy = st.busy;
    pthread_mutex_unlock(&lock);
    if (busy) { respond_error(c, 409, "Erst den laufenden Auftrag abwarten."); return; }
    snprintf(statef, sizeof(statef), "%s.state", path);
    if (unlink(path) != 0) {
        char err[800];
        snprintf(err, sizeof(err), T("Die Datei %s lässt sich nicht löschen: %s"), path, strerror(errno));
        respond_error(c, 500, err);
        return;
    }
    unlink(statef);
    respond_ok(c, 200);
}

/* Orte und vorhandene Sicherungen */
static void api_images(int c) {
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    if (!loc) { respond_error(c, 500, "Kein Speicher"); return; }
    int nl = img_locations(loc, MAX_LOCS);
    sbuf_t b = {0};
    sb_add(&b, "{\"locations\":[");
    for (int i = 0; i < nl; i++) {
        struct statvfs vs;
        sb_add(&b, "%s{\"dir\":", i ? "," : "");
        sb_str(&b, loc[i].dir);
        sb_add(&b, ",\"root\":");
        sb_str(&b, loc[i].root);
        sb_add(&b, ",\"device\":");
        sb_str(&b, loc[i].device);
        sb_add(&b, ",\"fs\":");
        sb_str(&b, loc[i].fs);
        sb_add(&b, ",\"label\":");
        sb_str(&b, loc[i].label);
        if (statvfs(loc[i].root, &vs) == 0)
            sb_add(&b, ",\"free\":%llu,\"total\":%llu", (unsigned long long)vs.f_bavail * vs.f_frsize,
                   (unsigned long long)vs.f_blocks * vs.f_frsize);
        sb_add(&b, "}");
    }
    sb_add(&b, "],\"images\":[");
    int first = 1;
    char *path = malloc(1024);
    for (int i = 0; path && i < nl; i++) {
        DIR *dir = opendir(loc[i].dir);
        struct dirent *e;
        int count = 0;
        while (dir && (e = readdir(dir)) && count < MAX_IMAGES) {
            if (e->d_name[0] == '.' || !ends_with(e->d_name, ".zst")) continue;
            if (snprintf(path, 1024, "%s/%s", loc[i].dir, e->d_name) >= 1024) continue;
            struct stat sb;
            if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
            FILE *f = fopen(path, "rb");
            char err[160] = "";
            ib_info_t info;
            ib_reader_t *rd = f ? ib_open(file_in, f, &info, err, sizeof(err)) : NULL;
            if (f) fclose(f);
            if (!rd) continue;   /* keine Sicherung (andere .zst) */
            ib_close(rd);
            count++;
            sb_add(&b, "%s{\"path\":", first ? "" : ",");
            first = 0;
            sb_str(&b, path);
            sb_add(&b, ",\"name\":");
            sb_str(&b, e->d_name);
            sb_add(&b, ",\"dir\":");
            sb_str(&b, loc[i].dir);
            sb_add(&b, ",\"device\":");
            sb_str(&b, loc[i].device);
            sb_add(&b, ",\"bytes\":%llu,\"size\":%llu,\"used\":%llu,\"own\":%s,\"model\":", (unsigned long long)sb.st_size,
                   (unsigned long long)info.size, (unsigned long long)info.used, info.has_map ? "true" : "false");
            sb_str(&b, info.model);
            sb_add(&b, ",\"label\":");
            sb_str(&b, info.label);
            sb_add(&b, ",\"fs\":");
            sb_str(&b, info.fs);
            sb_add(&b, ",\"created\":");
            sb_str(&b, info.created);
            snprintf(path + strlen(path), 1024 - strlen(path), ".sha256");
            sb_add(&b, ",\"sha256\":%s}", access(path, F_OK) == 0 ? "true" : "false");
        }
        if (dir) closedir(dir);
    }
    sb_add(&b, "],\"partials\":[");
    first = 1;
    for (int i = 0; path && i < nl; i++) {
        DIR *dir = opendir(loc[i].dir);
        struct dirent *e;
        int count = 0;
        while (dir && (e = readdir(dir)) && count < MAX_IMAGES) {
            if (e->d_name[0] == '.' || !ends_with(e->d_name, ".img.zst.part")) continue;
            char sp[1100];
            if (snprintf(path, 1024, "%s/%s", loc[i].dir, e->d_name) >= 1024 || snprintf(sp, sizeof(sp), "%s.state", path) >= (int)sizeof(sp)) continue;
            part_state_t ps;
            ib_frame_t *fr = NULL;
            if (part_state_read(sp, &ps, &fr) != 0) continue;
            free(fr);
            count++;
            sb_add(&b, "%s{\"path\":", first ? "" : ",");
            first = 0;
            sb_str(&b, path);
            sb_add(&b, ",\"name\":");
            sb_str(&b, e->d_name);
            sb_add(&b, ",\"dir\":");
            sb_str(&b, loc[i].dir);
            sb_add(&b, ",\"device\":");
            sb_str(&b, loc[i].device);
            sb_add(&b, ",\"ident\":");
            sb_str(&b, ps.ident);
            sb_add(&b, ",\"model\":");
            sb_str(&b, ps.model);
            sb_add(&b, ",\"label\":");
            sb_str(&b, ps.label);
            sb_add(&b, ",\"fs\":");
            sb_str(&b, ps.fs);
            sb_add(&b, ",\"created\":");
            sb_str(&b, ps.created);
            sb_add(&b, ",\"size\":%llu,\"used\":%llu,\"done\":%llu,\"bytes\":%llu,\"when\":%lld}", (unsigned long long)ps.dev_size,
                   (unsigned long long)ps.c.used, (unsigned long long)ps.c.used_done, (unsigned long long)ps.c.file_bytes, (long long)ps.when);
        }
        if (dir) closedir(dir);
    }
    free(path);
    free(loc);
    sb_add(&b, "]}");
    respond_json(c, 200, &b);
    free(b.p);
}

/* ------------------------------------------------------------ Spiele löschen */

/* Fehlertext einer ShadowMountPlus-Antwort ({"status":<errno>,"error":"..."}) */
static void smp_error(const char *resp, size_t len, int http, char *out, size_t n) {
    char e[160] = "";
    if (resp) lib_json_str(resp, len, "error", e, sizeof(e));
    if (http == 409) snprintf(out, n, "%s", T("ShadowMountPlus ist beschäftigt oder das Spiel läuft gerade – Spiel beenden und "
                                     "erneut versuchen"));
    else if (http == 400 || http == 501) snprintf(out, n, T("ShadowMountPlus kann das nicht (ab Version 1.7 nötig)%s%s"),
                                                  e[0] ? ": " : "", e);
    else snprintf(out, n, T("ShadowMountPlus meldet Fehler %d%s%s"), http, e[0] ? ": " : "", e);
}

/* Löschen über ShadowMountPlus: Auftrag starten und bis zum Ende verfolgen (es hält dabei Scanner und Startseite an
 * und gibt eingebundene Abbilder selbst frei). 0 = gelöscht. */
static char del_step[320];   /* Schrittname des laufenden Löschens: "Spiele löschen: Name (2 von 5)" */

static int smp_delete(const del_item_t *it, int base, int span, char *err, size_t n) {
    char body[96], resp_err[200];
    size_t len;
    int http;
    long long job = 0, v;
    snprintf(body, sizeof(body), "{\"title_id\":\"%s\",\"confirm\":true}", it->id);
    char *r = plat_smp_post("/api/v1/games/delete", body, &http, &len);
    if (!r) { snprintf(err, n, "%s", T("ShadowMountPlus nicht erreichbar")); return -1; }
    if (http != 202 || lib_json_num(r, len, "job_id", &job) != 0) {
        smp_error(r, len, http, err, n);
        free(r);
        return -1;
    }
    free(r);
    snprintf(body, sizeof(body), "{\"job_id\":%lld}", job);
    /* Grenzen: höchstens eine Stunde; aufgeben, wenn ShadowMountPlus 30 s nicht antwortet oder sich 10 Minuten
     * nichts tut (abgestürzt/hängt) - sonst wäre der Formatierer so lange für alles andere blockiert */
    time_t end = time(NULL) + 3600, last_change = time(NULL);
    int silent = 0, cancel_sent = 0;
    long long last_v = -1;
    char last_state[24] = "";
    while (time(NULL) < end) {
        usleep(500 * 1000);
        if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED) && !cancel_sent) {   /* geht nur, solange ShadowMountPlus noch nichts entfernt hat */
            char *cr = plat_smp_post("/api/v1/games/storage/cancel", body, &http, &len);
            free(cr);
            cancel_sent = 1;
        }
        if (!(r = plat_smp_post("/api/v1/games/storage/status", body, &http, &len))) {
            if (++silent >= 60) { snprintf(err, n, "%s", T("ShadowMountPlus antwortet nicht mehr")); return -1; }
            continue;
        }
        silent = 0;
        char state[24] = "";
        lib_json_str(r, len, "state", state, sizeof(state));
        if (lib_json_num(r, len, "progress_percent", &v) == 0 && v >= 0 && v <= 100)
            on_progress(del_step, base + (int)(v * span / 100));
        else v = -1;
        if (v != last_v || strcmp(state, last_state) != 0) {
            last_v = v;
            snprintf(last_state, sizeof(last_state), "%s", state);
            last_change = time(NULL);
        } else if (time(NULL) - last_change > 600) {
            free(r);
            snprintf(err, n, "%s", T("ShadowMountPlus meldet seit 10 Minuten keinen Fortschritt – abgebrochen"));
            return -1;
        }
        if (!strcmp(state, "completed")) { free(r); return 0; }
        if (!strcmp(state, "failed") || !strcmp(state, "cancelled") || http != 200) {
            resp_err[0] = 0;
            lib_json_str(r, len, "result_error", resp_err, sizeof(resp_err));
            snprintf(err, n, "ShadowMountPlus: %s", resp_err[0] ? resp_err : state[0] ? state : T("Fehler"));
            free(r);
            return -1;
        }
        free(r);
    }
    snprintf(err, n, "%s", T("ShadowMountPlus antwortet nicht mehr"));
    return -1;
}

/* dieselbe Datei? ShadowMountPlus schreibt einen Pfad womöglich anders (Schrägstrich am Ende, "//") - ein bloßer
 * Textvergleich hielte ein eingebundenes Spiel dann für eine zweite Kopie */
static int same_path(const char *a, const char *b) {
    struct stat x, y;
    return !strcmp(a, b) || (!stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino);
}

/* Liegt path wirklich (nach Auflösen von Verknüpfungen) innerhalb von root? */
static int path_inside(const char *path, const char *root) {
    char *rp = realpath(path, NULL), *rr = realpath(root, NULL);
    size_t rl = rr ? strlen(rr) : 0;
    int ok = rp && rr && rl > 1 && strncmp(rp, rr, rl) == 0 && rp[rl] == '/';
    free(rp);
    free(rr);
    return ok;
}

/* Rekursiv löschen (nur Kopien, die ShadowMountPlus nicht verwaltet, z. B. doppelte) */
/* Fortschritt beim Selbst-Löschen: Gewicht je Datei = Größe + 64 KiB (Freigeben großer Dateien dauert, viele kleine
 * auch). total wird vorher gezählt, damit die Anzeige gleichmäßig steigt statt bei 0 % zu stehen. */
typedef struct {
    uint64_t total, done;
    int      base, span, last;
} rm_prog_t;

#define RM_FILE_WEIGHT (64ull * 1024)

static uint64_t tree_weight(const char *path, int depth) {
    struct stat sb;
    if (depth > 32 || lstat(path, &sb) != 0) return 0;
    if (!S_ISDIR(sb.st_mode)) return (uint64_t)sb.st_size + RM_FILE_WEIGHT;
    DIR *d = opendir(path);
    if (!d) return 0;
    uint64_t w = RM_FILE_WEIGHT;
    char *sub = malloc(1024);
    struct dirent *e;
    while (sub && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, 1024, "%s/%s", path, e->d_name) < 1024) w += tree_weight(sub, depth + 1);
    }
    free(sub);
    closedir(d);
    return w;
}

static void rm_step(rm_prog_t *p, uint64_t w) {
    if (!p) return;
    p->done += w;
    int pct = p->base + (int)(p->total ? (p->done > p->total ? p->total : p->done) * (uint64_t)p->span / p->total : 0);
    if (pct != p->last) { p->last = pct; on_progress(del_step, pct); }
}

static int remove_tree(const char *path, int depth, rm_prog_t *p) {
    struct stat sb;
    if (depth > 32) { errno = ELOOP; return -1; }
    if (lstat(path, &sb) != 0) return -1;
    if (!S_ISDIR(sb.st_mode)) {
        int rc = unlink(path), saved = errno;
        rm_step(p, (uint64_t)sb.st_size + RM_FILE_WEIGHT);
        errno = saved;   /* Fehlertext gehört zum unlink, nicht zur Fortschrittsmeldung */
        return rc;
    }
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    char *sub = malloc(1024);
    while (sub && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, 1024, "%s/%s", path, e->d_name) >= 1024) { errno = ENAMETOOLONG; rc = -1; }
        else if (remove_tree(sub, depth + 1, p) != 0) rc = -1;
    }
    if (!sub) { errno = ENOMEM; rc = -1; }
    free(sub);
    closedir(d);
    rm_step(p, RM_FILE_WEIGHT);
    return rc == 0 ? rmdir(path) : -1;
}

static void *delete_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0}, done_names = {0}, errs = {0};
    char err[256];
    int ok_n = 0, fail_n = 0, tiles = 0, extras_n = 0;

    lib_hint_t *smp = NULL;
    int nsmp = 0, http, smp_ok = 0, stale = 1;
    size_t len;
    sbuf_t failed = {0};

    /* zweite Kopien zuerst: Wird die verwaltete zuerst gelöscht, bindet ShadowMountPlus womöglich gleich die andere
     * ein - und die gälte dann nicht mehr als sichere zweite Kopie */
    char *g0 = plat_smp_post("/api/v1/games", "{}", &http, &len);
    if (g0 && http == 200 && lib_parse_smp_games(g0, len, &smp, &nsmp) == 0) {
        for (int i = 0, front = 0; i < j->nitems; i++) {
            int copy = 0;
            for (int k = 0; k < nsmp; k++)
                copy |= !strcmp(smp[k].id, j->items[i].id) && !same_path(smp[k].path, j->items[i].path);
            if (copy) {   /* nach vorn holen, Reihenfolge sonst erhalten */
                del_item_t tmp = j->items[i];
                memmove(&j->items[front + 1], &j->items[front], (size_t)(i - front) * sizeof(tmp));
                j->items[front++] = tmp;
            }
        }
    }
    free(g0);

    for (int i = 0; i < j->nitems; i++) {
        const del_item_t *it = &j->items[i];
        int base = i * 100 / j->nitems, span = 100 / j->nitems, managed = 0, rc;
        snprintf(del_step, sizeof(del_step), T("%s: %s (%d von %d)"), T("Spiele löschen"), it->name, i + 1, j->nitems);
        on_progress(del_step, base > 0 ? base : 1);
        /* Wer verwaltet welchen Pfad? Nur die Kopie, die ShadowMountPlus selbst nutzt, löscht es auch. Nach jedem
         * Löschen neu fragen: Bei doppelten Spielen bindet es danach evtl. die andere Kopie ein. */
        if (stale) {
            free(smp);
            smp = NULL;
            nsmp = 0;
            char *g = plat_smp_post("/api/v1/games", "{}", &http, &len);
            smp_ok = g && http == 200 && lib_parse_smp_games(g, len, &smp, &nsmp) == 0;
            free(g);
            stale = 0;
        }
        int other_copy = 0;   /* ShadowMountPlus führt diese Titel-ID unter einem anderen Pfad: das hier ist die zweite Kopie */
        for (int k = 0; k < nsmp; k++)
            if (!strcmp(smp[k].id, it->id)) {
                if (same_path(smp[k].path, it->path)) managed = 1;
                else other_copy = 1;
            }
        err[0] = 0;
        if (root_dev(it->root) != it->dev || !lib_same_game(it->path, it->id, it->size)) {
            /* seit der Suche getauscht: anderer Stick am Anschluss oder ein anderes Spiel unter dem Pfad */
            rc = -1;
            snprintf(err, sizeof(err), "%s", T("Spiel hat sich seit der Suche geändert – nichts gelöscht. Liste neu laden."));
        } else if (managed) {
            rc = smp_delete(it, base, span, err, sizeof(err));
            stale = 1;
            if (rc == 0 && j->uninstall) {   /* Kachel entfernen; Quelle ist weg, ein neuer Suchlauf holt sie nicht zurück */
                char body[64];
                snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", it->id);
                char *r = plat_smp_post("/api/v1/games/uninstall", body, &http, &len);
                if (r && http == 200) tiles++;
                else if (!r || http != 404) {
                    char e2[200];
                    smp_error(r, len, http, e2, sizeof(e2));
                    sb_add(&errs, "%s", errs.len ? "; " : "");
                    sb_tr(&errs, "%s: gelöscht, aber Kachel nicht entfernt (%s)", it->name, e2);
                }
                free(r);
            }
        } else if (!smp_ok) {
            rc = -1;
            snprintf(err, sizeof(err), "%s", T("ShadowMountPlus nicht erreichbar – zum Schutz eingebundener Spiele nichts "
                                       "gelöscht"));
        } else if (!other_copy) {
            /* ShadowMountPlus kennt das Spiel (noch) nicht - etwa direkt nach einem Neustart, solange es sucht. Ob es
             * eingebunden ist, lässt sich dann nicht sicher sagen: lieber nicht selbst löschen. */
            rc = -1;
            snprintf(err, sizeof(err), "%s", T("ShadowMountPlus kennt dieses Spiel noch nicht – bitte kurz warten und erneut "
                                       "versuchen"));
        } else {
            /* nicht von ShadowMountPlus genutzt (z. B. zweite Kopie): selbst löschen - nur innerhalb der Wurzel des
             * Suchordners, in dem die Suche es fand */
            if (!path_inside(it->path, it->root)) {
                rc = -1;
                snprintf(err, sizeof(err), "%s", T("Pfad liegt nicht im durchsuchten Ordner"));
            } else {
                on_progress(del_step, base > 0 ? base : 1);
                rm_prog_t rp = {tree_weight(it->path, 0), 0, base, span, -1};
                rc = remove_tree(it->path, 0, &rp);
                if (rc != 0) {
                    const int e = errno;
                    struct stat ps;
                    /* ein Teil ist schon weg, der Rest blieb: das Spiel ist jetzt unvollständig - das gehört in die Meldung */
                    if (lstat(it->path, &ps) == 0 && tree_weight(it->path, 0) < rp.total) {
                        snprintf(err, sizeof(err), T("Nur teilweise gelöscht – der Rest bleibt, bitte erneut löschen (%s)"), strerror(e));
                    } else {
                        errno = e;
                    }
                }
            }
            if (rc != 0 && !err[0]) {
                snprintf(err, sizeof(err), T("Löschen fehlgeschlagen: %s"), strerror(errno));
            }
        }
        if (rc != 0 && fail_n < 8) {   /* nur die ersten Fehlschläge einzeln - st.result ist begrenzt */
            sb_add(&failed, "%s{\"id\":", fail_n ? "," : "");
            sb_str(&failed, it->id);
            sb_add(&failed, ",\"name\":");
            sb_str(&failed, it->name);
            sb_add(&failed, ",\"error\":");
            sb_str(&failed, err);
            sb_add(&failed, "}");
        }
        if (rc == 0) {
            ok_n++;
            sb_add(&done_names, "%s%s", done_names.len ? ", " : "", it->name);
            char xb[128];
            const char *xbase = move_base(it->root, xb, sizeof(xb));
            for (int x = 0; j->extras && x < it->nex; x++) {   /* Backport-Ordner, Prüfsummen: sonst gälten sie beim nächsten Mal wieder */
                if (!path_inside(it->ex[x], xbase)) {   /* Verknüpfung o. ä. führt aus dem Suchbereich hinaus: nichts anfassen */
                    sb_add(&errs, "%s", errs.len ? "; " : "");
                    sb_tr(&errs, "%s: Zugehöriges liegt außerhalb des Ordners – nicht gelöscht", it->name);
                    continue;
                }
                if (remove_tree(it->ex[x], 0, NULL) == 0) extras_n++;
                else if (errno != ENOENT) {
                    sb_add(&errs, "%s", errs.len ? "; " : "");
                    sb_tr(&errs, "%s: Zugehöriges nicht gelöscht (%s)", it->name, it->ex[x]);
                }
            }
        } else { fail_n++; sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, err); }
    }
    sb_add(&res, "{\"deleted\":%d,\"failed\":%d,\"tiles\":%d,\"extras\":%d,\"failed_items\":[%s]}", ok_n, fail_n, tiles,
           extras_n, failed.p ? failed.p : "");
    free(failed.p);
    free(smp);

    if (ok_n == 1) sb_tr(&msg, "1 Spiel gelöscht: %s.", done_names.p);
    else if (ok_n) sb_tr(&msg, "%d Spiele gelöscht: %s.", ok_n, done_names.p);
    if (extras_n == 1) sb_tr(&msg, " 1 zugehörige Datei bzw. Ordner mitgelöscht.");
    else if (extras_n) sb_tr(&msg, " %d zugehörige Dateien bzw. Ordner mitgelöscht.", extras_n);
    if (tiles == 1) sb_tr(&msg, " 1 Kachel von der Startseite entfernt.");
    else if (tiles) sb_tr(&msg, " %d Kacheln von der Startseite entfernt.", tiles);
    if (errs.len) sb_add(&msg, "%s%s.", ok_n ? " " : T("Nicht gelöscht – "), errs.p);
    free(done_names.p);
    free(errs.p);
    free(j->items);
    job_done(j, fail_n == 0, &msg, &res, 1);
    return NULL;
}

/* {"keys":"1a2b3c4d,5e6f7a8b","uninstall":true} - Schlüssel aus /api/library; nur Spiele auf USB-Laufwerken */
#define MAX_DELETE 256   /* Spiele je Auftrag (9 Zeichen je Schlüssel passen in BODY_MAX) */

/* ------------------------------------------------------------ App-Kachel (PPSA86920) über ShadowMountPlus */

/* Die PS5 liest die param.json einer Kachel nur beim Anmelden: Nach einem Update des App-Ordners zeigt sie unter
 * "Informationen" weiter die alte Version, bis ShadowMountPlus die App einmal ab- und wieder anmeldet. Das macht es
 * selbst nicht (es meldet nur Fehlendes an) - daher hier "Uninstall" (die Quelle bleibt, nur die Anmeldung geht) und
 * danach "Scan". Die Kachel steht danach womöglich an anderer Stelle auf dem Startbildschirm. */
#define APP_TITLE "PPSA86920"

typedef struct {
    int  smp;                /* ShadowMountPlus antwortet mit einer gültigen Liste */
    int  found, installed;   /* App in seiner Liste, angemeldet */
    char registered[24];     /* contentVersion laut Anmeldung */
    char folder[24];         /* contentVersion in sce_sys/param.json des App-Ordners */
    char path[512];          /* Ordner der App */
} tile_t;

static void tile_state(tile_t *t) {
    memset(t, 0, sizeof(*t));
    int http;
    size_t len;
    char *g = plat_smp_post("/api/v1/games", "{}", &http, &len);
    int rc = g && http == 200 ? lib_smp_title(g, len, APP_TITLE, t->registered, sizeof(t->registered), t->path,
                                              sizeof(t->path), &t->installed) : -1;
    free(g);
    t->smp = rc >= 0;
    t->found = rc == 1;
    char pj[600];
    FILE *f = t->path[0] && snprintf(pj, sizeof(pj), "%s/sce_sys/param.json", t->path) < (int)sizeof(pj)
              ? fopen(pj, "rb") : NULL;
    if (f) {
        char buf[16384];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        lib_json_str(buf, n, "contentVersion", t->folder, sizeof(t->folder));
    }
}

/* contentVersion dieses Formatierers: 2.5.7 -> 02.005.007 (wie dump/Program.cs) */
static void formatter_content_version(char *out, size_t n) {
    unsigned a = 0, b = 0, v = 0;
    if (sscanf(USBFMT_VERSION, "%u.%u.%u", &a, &b, &v) == 3) snprintf(out, n, "%02u.%03u.%03u", a, b, v);
    else snprintf(out, n, "%s", "");
}

/* bis die Kachel ab- (want = 0) bzw. angemeldet ist (want = 1, bei gesetztem version mit genau dieser Version).
 * Höchstens secs Sekunden nach der Uhr - eine Anfrage an ein hängendes ShadowMountPlus kann selbst bis zu 10 s dauern.
 * 0 = erreicht, -1 = Zeit um, 1 = abgebrochen */
static int tile_wait(int want, const char *version, const char *step, int from, int to, int secs) {
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        tile_t t;
        tile_state(&t);
        if (t.smp && (want ? t.found && t.installed && (!version[0] || !strcmp(t.registered, version))
                           : !(t.found && t.installed)))
            return 0;
        if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) return 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long ms = (long)(now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= secs * 1000L) return -1;
        on_progress(step, from + (int)((to - from) * ms / (secs * 1000L)));
        usleep(500 * 1000);
    }
}

static void *tile_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0};
    char err[200];
    int ok = 0, http;
    size_t len;
    tile_t t;
    on_progress("Kachel abmelden", 5);
    tile_state(&t);
    if (!t.smp) { sb_add(&msg, "%s", T("ShadowMountPlus nicht erreichbar")); goto out; }
    if (!t.found) {
        sb_tr(&msg, "ShadowMountPlus führt die App „USB-Formatierer“ (%s) nicht – liegt ihr Ordner in /data/homebrew?",
              APP_TITLE);
        goto out;
    }
    if (t.installed) {
        char *r = plat_smp_post("/api/v1/games/uninstall", "{\"title_id\":\"" APP_TITLE "\"}", &http, &len);
        if (!r || (http != 200 && http != 404)) {
            if (r && http == 409)
                sb_add(&msg, "%s", T("Die App „USB-Formatierer“ ist noch geöffnet oder ShadowMountPlus ist beschäftigt – die "
                                    "App schließen (der Formatierer läuft weiter) und erneut versuchen."));
            else {
                smp_error(r, len, http, err, sizeof(err));
                sb_add(&msg, "%s", r ? err : T("ShadowMountPlus nicht erreichbar"));
            }
            free(r);
            goto out;
        }
        free(r);
        if (tile_wait(0, "", "Kachel abmelden", 10, 40, 30)) {
            sb_add(&msg, "%s", T("ShadowMountPlus hat die Kachel nicht abgemeldet."));
            goto out;
        }
    }
    on_progress("Kachel anmelden", 45);
    char *r = plat_smp_post("/api/v1/scan", "{\"reset_attempts\":true}", &http, &len);
    int scanned = r && http == 200;
    free(r);
    set_flags(1, 0);   /* Warten auf die Neuanmeldung darf abgebrochen werden - sie kommt dann von selbst */
    const int waited = scanned ? tile_wait(1, t.folder, "Kachel anmelden", 50, 98, 90) : -1;
    set_flags(0, 0);
    if (waited == 1) {
        sb_add(&msg, "%s", T("Abgebrochen – ShadowMountPlus meldet die Kachel beim nächsten Suchlauf selbst wieder an."));
        goto out;
    }
    if (waited) {
        sb_add(&msg, "%s", T("Die Kachel ist abgemeldet, aber noch nicht wieder angemeldet. ShadowMountPlus meldet sie beim "
                            "nächsten Suchlauf an – sonst in seiner Weboberfläche „Scan“ drücken."));
        goto out;
    }
    tile_state(&t);
    ok = 1;
    sb_tr(&msg, "Kachel neu angemeldet – Version %s. Sie steht jetzt womöglich an anderer Stelle auf dem Startbildschirm.",
          t.registered[0] ? t.registered : "?");
    sb_add(&res, "{\"registered\":");
    sb_str(&res, t.registered);
    sb_add(&res, "}");
out:
    job_done(j, ok, &msg, &res, 0);
    return NULL;
}

/* ── Spielstand-API ─────────────────────────────────────────────────── */

/* Läuft ein Spiel oder eine App, kann es den Spielstand gerade benutzen: dann nichts daran ändern */
static int saves_game_check(int c) {
    char gid[16], err[320];
    if (!game_running(gid, sizeof(gid))) return 0;
    snprintf(err, sizeof(err),
             T("Es läuft gerade ein Spiel oder eine App (%s) – bitte zuerst beenden, sonst kann der Spielstand beschädigt werden."),
             gid);
    respond_error(c, 409, err);
    return 1;
}

/* GET /api/saves/users  → {"users":[{"uid","username"},…]} */
static void api_saves_users(int c) {
    save_user_t *users = NULL;
    int nu = 0;
    saves_scan_users(&users, &nu);
    sbuf_t b = {0};
    sb_add(&b, "{\"users\":[");
    for (int i = 0; i < nu; i++) {
        if (i) sb_add(&b, ",");
        sb_add(&b, "{\"uid\":");
        sb_str(&b, users[i].uid);
        sb_add(&b, ",\"username\":");
        sb_str(&b, users[i].username);
        sb_add(&b, "}");
    }
    sb_add(&b, "]}");
    free(users);
    respond_json(c, 200, &b);
    free(b.p);
}

/* GET /api/saves  → {"saves":[{"key","title_id","title_name","save_dir","uid","username","size"},…]} */
static void api_saves(int c) {
    save_info_t *saves = NULL;
    int ns = 0;
    saves_scan(&saves, &ns);
    sbuf_t b = {0};
    sb_add(&b, "{\"saves\":[");
    for (int i = 0; i < ns; i++) {
        if (i) sb_add(&b, ",");
        sb_add(&b, "{\"key\":");
        sb_str(&b, saves[i].key);
        sb_add(&b, ",\"title_id\":");
        sb_str(&b, saves[i].title_id);
        sb_add(&b, ",\"title_name\":");
        sb_str(&b, saves[i].title_name);
        sb_add(&b, ",\"save_dir\":");
        sb_str(&b, saves[i].save_dir);
        sb_add(&b, ",\"uid\":");
        sb_str(&b, saves[i].uid);
        sb_add(&b, ",\"username\":");
        sb_str(&b, saves[i].username);
        sb_add(&b, ",\"platform\":");
        sb_str(&b, saves[i].platform);
        sb_add(&b, ",\"size\":%llu,\"mtime\":%lld}", (unsigned long long)saves[i].size, (long long)saves[i].mtime);
    }
    sb_add(&b, "]}");
    free(saves);
    respond_json(c, 200, &b);
    free(b.p);
}

/* Spielstände-Ordner der Orte (wie die Image-Sicherung: intern /data und jedes eingebundene USB-Laufwerk) */
#define SAVES_DIR "game_saves"

/* Text eines JSON-Schlüssels der obersten Ebene, \" \\ \/ \n \t aufgelöst; malloc oder NULL */
static char *json_text(const char *body, const char *key) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return NULL;
    p = ws(p + strlen(pat));
    if (*p != ':') return NULL;
    p = ws(p + 1);
    if (*p != '"') return NULL;
    size_t cap = strlen(p) + 1, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            c = *++p;
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c;
        }
        out[n++] = c;
    }
    out[n] = 0;
    return out;
}

/* "a\nb\nc" -> Schlüssel in Teilen; liefert die Anzahl, -1 bei mehr als max; schneidet s an den Zeilenenden ab */
static int split_lines(char *s, char **part, int max) {
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(s, "\n", &save); t; t = strtok_r(NULL, "\n", &save)) {
        if (!*t) continue;
        if (n == max) return -1;   /* zu viele */
        part[n++] = t;
    }
    return n;
}

#define SAVES_SELMAX 64

/* GET /api/saves/download?keys=<Schlüssel>,<Schlüssel>,... (oder key=<Schlüssel>) → ZIP. Bricht der Strom mittendrin
 * ab, endet er ohne Abschluss-Chunk (der Aufrufer schließt die Verbindung) - so merkt der Empfänger den Fehler */
static void api_saves_download(int c, const char *q) {
    char *keys = malloc(SAVES_SELMAX * 330 + 1);
    save_info_t *sel = calloc(SAVES_SELMAX, sizeof(*sel));
    if (!keys || !sel) { free(keys); free(sel); respond_error(c, 500, "Kein Speicher"); return; }
    keys[0] = 0;
    if (!q || (query_get(q, "keys", keys, SAVES_SELMAX * 330) && query_get(q, "key", keys, 330))) {
        respond_error(c, 400, "key fehlt");
        goto out;
    }
    for (char *t = keys; *t; t++)
        if (*t == ',') *t = '\n';
    char *part[SAVES_SELMAX];
    int np = split_lines(keys, part, SAVES_SELMAX), ns = 0;
    save_info_t *all = NULL;
    int na = 0;
    if (np < 1 || saves_scan(&all, &na) < 0) { free(all); respond_error(c, 404, "Spielstand nicht gefunden"); goto out; }
    for (int i = 0; i < np; i++) {
        int hit = 0;
        for (int k = 0; k < na && !hit; k++)
            if (!strcmp(all[k].key, part[i])) { sel[ns++] = all[k]; hit = 1; }
        if (!hit) { free(all); respond_error(c, 404, "Spielstand nicht gefunden"); goto out; }
    }
    free(all);
    char err[256] = "";
    if (saves_download(c, sel, ns, err, sizeof(err)) == -1) respond_error(c, 500, err[0] ? err : "Sichern fehlgeschlagen");
out:
    free(keys);
    free(sel);
}

static void saves_loc_json(sbuf_t *b, const imgloc_t *l, int first) {
    struct statvfs vs;
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/" SAVES_DIR, l->root);
    sb_add(b, "%s{\"dir\":", first ? "" : ",");
    sb_str(b, dir);
    sb_add(b, ",\"root\":");
    sb_str(b, l->root);
    sb_add(b, ",\"device\":");
    sb_str(b, l->device);
    sb_add(b, ",\"fs\":");
    sb_str(b, l->fs);
    sb_add(b, ",\"label\":");
    sb_str(b, l->label);
    if (statvfs(l->root, &vs) == 0)
        sb_add(b, ",\"free\":%llu,\"total\":%llu", (unsigned long long)vs.f_bavail * vs.f_frsize,
               (unsigned long long)vs.f_blocks * vs.f_frsize);
    sb_add(b, "}");
}

/* GET /api/saves/locations  → {"locations":[{"dir","root","device","fs","label","free","total"},…]} */
static void api_saves_locations(int c) {
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    if (!loc) { respond_error(c, 500, "Kein Speicher"); return; }
    int nl = img_locations(loc, MAX_LOCS);
    sbuf_t b = {0};
    sb_add(&b, "{\"locations\":[");
    for (int i = 0; i < nl; i++) saves_loc_json(&b, &loc[i], !i);
    sb_add(&b, "]}");
    free(loc);
    respond_json(c, 200, &b);
    free(b.p);
}

/* GET /api/saves/backups  → {"locations":[…],"backups":[{"path","dir","uid","username","title_id","title_name","save_dir",
 * "size","time"},…]}: Sicherungen in <Ort>/game_saves/<Benutzer>_<uid>/<Datei>.zip */
static void api_saves_backups(int c) {
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    if (!loc) { respond_error(c, 500, "Kein Speicher"); return; }
    int nl = img_locations(loc, MAX_LOCS);
    sbuf_t b = {0};
    sb_add(&b, "{\"locations\":[");
    for (int i = 0; i < nl; i++) saves_loc_json(&b, &loc[i], !i);
    sb_add(&b, "],\"backups\":[");
    int first = 1;
    for (int i = 0; i < nl; i++) {
        char dir[300];
        save_backup_t *bk = NULL;
        int nb = 0;
        snprintf(dir, sizeof(dir), "%s/" SAVES_DIR, loc[i].root);
        if (saves_list_backups(dir, &bk, &nb) < 0) continue;
        for (int k = 0; k < nb; k++) {
            sb_add(&b, "%s{\"path\":", first ? "" : ",");
            first = 0;
            sb_str(&b, bk[k].path);
            sb_add(&b, ",\"dir\":");
            sb_str(&b, dir);
            sb_add(&b, ",\"uid\":");
            sb_str(&b, bk[k].uid);
            sb_add(&b, ",\"username\":");
            sb_str(&b, bk[k].username);
            sb_add(&b, ",\"title_id\":");
            sb_str(&b, bk[k].title_id);
            sb_add(&b, ",\"title_name\":");
            sb_str(&b, bk[k].title_name);
            sb_add(&b, ",\"save_dir\":");
            sb_str(&b, bk[k].save_dir);
            sb_add(&b, ",\"platform\":");
            sb_str(&b, bk[k].platform);
            sb_add(&b, ",\"size\":%llu,\"time\":%lld,\"save_mtime\":%lld}", (unsigned long long)bk[k].size, (long long)bk[k].mtime,
                   (long long)bk[k].save_mtime);
        }
        free(bk);
    }
    sb_add(&b, "]}");
    free(loc);
    respond_json(c, 200, &b);
    free(b.p);
}

/* Läuft ein Spiel oder eine App, kann es den Spielstand gerade benutzen: dann nichts daran ändern */
/* (saves_game_check steht weiter oben) */

/* Fortschritt eines Spielstand-Auftrags: Byte-genau über alle Spielstände, Schrittname mit Nummer und Spielname.
 * Mindestens 1 %, sobald es läuft (bei 0 % zeigen die Oberflächen nur einen laufenden Balken ohne Aussage). */
typedef struct {
    int      idx, n;       /* aktueller Spielstand (0-basiert) von n */
    uint64_t base, total;  /* schon erledigte Byte, Gesamtbyte */
    char     step[300];
    int      last;
    struct timespec t;
} sprog_t;

static int sprog_cb(void *ctx, uint64_t done, uint64_t total_file) {
    sprog_t *p = ctx;
    (void)total_file;
    if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) return 1;
    int pct = p->total ? (int)((p->base + done) * 100 / p->total) : 0;
    if (pct < 1) pct = 1;
    if (pct > 99) pct = 99;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (pct == p->last && (now.tv_sec - p->t.tv_sec) < 1) return 0;
    p->last = pct;
    p->t = now;
    on_progress(p->step, pct);
    return 0;
}

static void sprog_step(sprog_t *p, int idx, const char *what, const char *name) {
    p->idx = idx;
    snprintf(p->step, sizeof(p->step), T("%s: Spielstand %d von %d – %s"), T(what), idx + 1, p->n, name);
    p->last = -1;
    on_progress(p->step, p->total ? (int)(p->base * 100 / p->total) > 0 ? (int)(p->base * 100 / p->total) : 1 : 1);
}

/* Worker: Spielstände (Schlüssel in saves_list) nach saves_dest sichern */
static void *save_backup_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0};
    char *part[SAVES_SELMAX], err[256] = "", out[1300] = "";
    int np = split_lines(j->saves_list, part, SAVES_SELMAX), done = 0, ok = 0;
    save_info_t *all = NULL;
    int na = 0;
    sprog_t sp;
    memset(&sp, 0, sizeof(sp));
    if (np < 1 || saves_scan(&all, &na) < 0) {
        sb_add(&msg, "%s", T("Spielstand nicht gefunden"));
        goto fin;
    }
    sp.n = np;
    set_flags(1, 0);   /* "Abbrechen" zeigen */
    for (int i = 0; i < np; i++)   /* Gesamtgröße für den Fortschritt */
        for (int k = 0; k < na; k++)
            if (!strcmp(all[k].key, part[i])) sp.total += all[k].size;
    for (int i = 0; i < np; i++) {
        int hit = -1;
        for (int k = 0; k < na && hit < 0; k++)
            if (!strcmp(all[k].key, part[i])) hit = k;
        if (hit < 0) { sb_add(&msg, "%s", T("Spielstand nicht gefunden")); goto fin; }
        char shown[330];
        snprintf(shown, sizeof(shown), "%s (%s)", all[hit].title_name, all[hit].username[0] ? all[hit].username : all[hit].uid);
        sprog_step(&sp, i, N_("Sichern"), shown);
        if (saves_backup_file(&all[hit], j->saves_dest, out, sizeof(out), sprog_cb, &sp, err, sizeof(err)) != 0) {
            if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) {
                sb_tr(&msg, "Abgebrochen – gesichert: %d von %d.", done, np);
                goto fin;
            }
            if (done) sb_tr(&msg, "%d von %d Spielständen gesichert, dann Fehler: ", done, np);
            sb_add(&msg, "%s", err[0] ? err : T("Sichern fehlgeschlagen"));
            goto fin;
        }
        sp.base += all[hit].size;
        done++;
    }
    ok = 1;
    sb_tr(&msg, "Spielstände gesichert: %d (nach: %s)", done, j->saves_dest);
    sb_add(&res, "{\"count\":%d,\"dir\":", done);
    sb_str(&res, j->saves_dest);
    sb_add(&res, "}");
fin:
    free(all);
    job_done(j, ok, &msg, &res, 0);
    return NULL;
}

/* Ort per Pfad suchen: <root>/game_saves muss genau einer der Orte sein */
static const imgloc_t *saves_loc_by_dir(const char *dir, const imgloc_t *loc, int n) {
    for (int i = 0; i < n; i++) {
        char d[300];
        snprintf(d, sizeof(d), "%s/" SAVES_DIR, loc[i].root);
        if (!strcmp(d, dir)) return &loc[i];
    }
    return NULL;
}

/* POST /api/saves/backup  {"keys":"<Schlüssel>\n<Schlüssel>…","dest":"<Ort>/game_saves"} */
static void api_saves_backup(int c, const char *body) {
    char *keys = json_text(body, "keys"), *dest = json_text(body, "dest");
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    int nl = loc ? img_locations(loc, MAX_LOCS) : 0;
    if (!keys || !dest || !keys[0] || !loc || !saves_loc_by_dir(dest, loc, nl)) {
        respond_error(c, 400, "keys und dest erforderlich");
        goto out;
    }
    job_t *j = new_job(c, JOB_SAVE_BACKUP);
    if (!j) goto out;
    j->saves_list = keys;
    keys = NULL;
    snprintf(j->saves_dest, sizeof(j->saves_dest), "%.299s", dest);
    start_job(c, j, save_backup_worker);
out:
    free(keys);
    free(dest);
    free(loc);
}

typedef struct { int i, n; char step[300]; int last; struct timespec t; } rprog_t;

static int rprog_cb(void *ctx, uint64_t done, uint64_t total) {
    rprog_t *p = ctx;
    if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) return 1;
    int inner = total ? (int)(done * 100 / total) : 0;
    int pct = (p->i * 100 + inner) / p->n;
    if (pct < 1) pct = 1;
    if (pct > 99) pct = 99;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (pct == p->last && (now.tv_sec - p->t.tv_sec) < 1) return 0;
    p->last = pct;
    p->t = now;
    on_progress(p->step, pct);
    return 0;
}

/* Worker: Sicherungen (Pfade in saves_list) zurückschreiben */
static void *save_restore_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0};
    char *part[SAVES_SELMAX], err[256] = "";
    int np = split_lines(j->saves_list, part, SAVES_SELMAX), total = 0, ok = 0;
    rprog_t rp;
    memset(&rp, 0, sizeof(rp));
    rp.n = np > 0 ? np : 1;
    set_flags(1, 0);
    for (int i = 0; i < np; i++) {
        int cnt = 0;
        const char *fn = strrchr(part[i], '/') ? strrchr(part[i], '/') + 1 : part[i];
        rp.i = i;
        rp.last = -1;
        snprintf(rp.step, sizeof(rp.step), T("%s: Sicherung %d von %d – %s"), T(N_("Wiederherstellen")), i + 1, np, fn);
        on_progress(rp.step, (i * 100) / rp.n > 0 ? (i * 100) / rp.n : 1);
        if (saves_restore_file(part[i], &cnt, rprog_cb, &rp, err, sizeof(err)) != 0) {
            total += cnt;
            if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) {
                sb_tr(&msg, "Abgebrochen – wiederhergestellt: %d.", total);
                goto fin;
            }
            if (total && np > 1) sb_tr(&msg, "Spielstände wiederhergestellt: %d, dann Fehler: ", total);
            sb_add(&msg, "%s", err[0] ? err : T("Wiederherstellen fehlgeschlagen"));
            goto fin;
        }
        total += cnt;
    }
    ok = 1;
    sb_tr(&msg, "Spielstände wiederhergestellt: %d.", total);
fin:
    job_done(j, ok, &msg, &res, 0);
    return NULL;
}

/* Sicherung direkt in <Ort>/game_saves/<Benutzer>_<uid>/<Datei>.zip (keine Verknüpfung hinaus)? */
static int saves_path_ok(const char *path, const imgloc_t *loc, int n) {
    size_t l = strlen(path);
    const char *last = strrchr(path, '/');   /* NULL bei Pfaden ohne Schrägstrich: nie dereferenzieren */
    if (l < 5 || strcmp(path + l - 4, ".zip") != 0 || strstr(path, "/../") || !last || last[1] == '.') return 0;
    char *rp = realpath(path, NULL);
    int hit = 0;
    struct stat sb;
    for (int i = 0; rp && i < n && !hit; i++) {
        char d[300];
        snprintf(d, sizeof(d), "%s/" SAVES_DIR, loc[i].root);
        char *rd = realpath(d, NULL);
        size_t dl = rd ? strlen(rd) : 0;
        if (rd && !strncmp(rp, rd, dl) && rp[dl] == '/') {
            const char *rest = rp + dl + 1, *sl = strchr(rest, '/');
            hit = sl && !strchr(sl + 1, '/') && !stat(rp, &sb) && S_ISREG(sb.st_mode);
        }
        free(rd);
    }
    free(rp);
    return hit;
}

/* POST /api/saves/times  {"key":"<Schlüssel>","mtime":<Sekunden seit 1970>,"key_mtime":<Sekunden>}: Zeitstempel eines Spielstands
 * setzen (Reparatur: die PS5 führt die Änderungszeit in ihrer Datenbank mit, sie muss zur Datei passen) */
static void api_saves_times(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    const char *key = n > 0 ? kv_get(kv, n, "key") : NULL, *mt = n > 0 ? kv_get(kv, n, "mtime") : NULL;
    const char *kmt = n > 0 ? kv_get(kv, n, "key_mtime") : NULL;
    if (!key || !mt || strspn(mt, "0123456789") != strlen(mt) || (kmt && strspn(kmt, "0123456789") != strlen(kmt))) {
        respond_error(c, 400, "key und mtime erforderlich");
        return;
    }
    if (kmt && (!*kmt || strtoll(kmt, NULL, 10) < 946684800LL || strtoll(kmt, NULL, 10) > 4102444800LL)) {   /* 2000 bis 2100 */
        respond_error(c, 400, N_("key_mtime ungültig"));
        return;
    }
    if (saves_game_check(c)) return;
    char err[256] = "";
    if (saves_set_times(key, strtoll(mt, NULL, 10), kmt ? strtoll(kmt, NULL, 10) : -1, err, sizeof(err)) == 0) respond_ok(c, 200);
    else respond_error(c, 500, err[0] ? err : "Zeitstempel konnte nicht gesetzt werden");
}

/* POST /api/saves/restore  {"paths":"<Sicherung>\n<Sicherung>…"}: Sicherungen auf der PS5 zurückschreiben */
static void api_saves_restore_paths(int c, const char *body) {
    char *paths = json_text(body, "paths");
    imgloc_t *loc = calloc(MAX_LOCS, sizeof(*loc));
    int nl = loc ? img_locations(loc, MAX_LOCS) : 0;
    if (!paths || !paths[0] || !loc) { respond_error(c, 400, "paths erforderlich"); goto out; }
    {
        char *copy = strdup(paths), *part[SAVES_SELMAX];
        int np = copy ? split_lines(copy, part, SAVES_SELMAX) : 0, good = np > 0;
        for (int i = 0; i < np && good; i++) good = saves_path_ok(part[i], loc, nl);
        free(copy);
        if (!good) { respond_error(c, 400, "Ungültige Sicherung"); goto out; }
    }
    if (saves_game_check(c)) goto out;
    job_t *j = new_job(c, JOB_SAVE_RESTORE);
    if (!j) goto out;
    j->saves_list = paths;
    paths = NULL;
    start_job(c, j, save_restore_worker);
out:
    free(paths);
    free(loc);
}

/* POST /api/saves/restore/upload  Körper = ZIP: auf die Platte schreiben (nicht in den Speicher: Spielstände sind
 * mehrere zig MB groß) und zurückschreiben. Läuft in dieser Verbindung, wie das Hochladen eines Images. */
#define SAVES_UPLOAD_MAX (4ULL << 30)

/* Antwort steht schon (Fehler vor dem Lesen des Körpers): den Rest des Hochladens verwerfen, solange er flott kommt.
 * Schließt der Server mit ungelesenen Daten, setzt das TCP die Verbindung zurück und der Browser sieht statt der
 * Meldung nur "Verbindung abgebrochen". */
static void drain_body(int c, unsigned long long left) {
    struct timeval tv = {2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char *buf = malloc(65536);
    const time_t end = time(NULL) + 8;
    while (buf && left && time(NULL) < end) {
        ssize_t r = recv(c, buf, left < 65536 ? (size_t)left : 65536, 0);
        if (r <= 0) break;
        left -= (unsigned long long)r;
    }
    free(buf);
}

static void api_saves_restore(int c, const char *pre, size_t have, unsigned long long clen) {
    const unsigned long long rest = clen > have ? clen - have : 0;
    if (clen < 22 || clen > SAVES_UPLOAD_MAX) {
        respond_error(c, 413, "Content-Length fehlt oder zu groß (max 4 GiB)");
        if (clen <= SAVES_UPLOAD_MAX) drain_body(c, rest);
        return;
    }
    if (saves_game_check(c)) { drain_body(c, rest); return; }
    job_t *j = new_job(c, JOB_SAVE_RESTORE);
    if (!j) { drain_body(c, rest); return; }
    if (claim_job(c, j)) { drain_body(c, rest); return; }
    char zip[400], err[256] = "";
    sbuf_t msg = {0}, res = {0};
    int ok = 0, fd = -1, bad = 0;
    unsigned long long got = 0;
    char *buf = NULL;
    snprintf(zip, sizeof(zip), "%s/upload.zip", saves_workdir());
    struct statvfs sv;
    if (statvfs(saves_workdir(), &sv) == 0 && (unsigned long long)sv.f_bavail * sv.f_frsize < clen + (1ULL << 20)) {
        sb_add(&msg, "%s", T("Nicht genug freier Speicher auf der PS5"));
        goto fail;
    }
    unlink(zip);
    fd = open(zip, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        sb_add(&msg, T("Zwischendatei konnte nicht angelegt werden: %s"), strerror(errno));
        goto fail;
    }
    struct timeval tv = {60, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    on_progress("Spielstand wird hochgeladen …", 0);
    buf = malloc(65536);
    if (!buf) { sb_add(&msg, "%s", T("Kein Speicher")); goto fail; }
    while (got < clen && !bad) {
        size_t n;
        if (have) {
            n = have < clen ? have : (size_t)clen;
            memcpy(buf, pre, n < 65536 ? n : 65536);
            if (n > 65536) n = 65536;
            pre += n;
            have -= n;
        } else {
            size_t want = clen - got < 65536 ? (size_t)(clen - got) : 65536;
            ssize_t r;
            do r = recv(c, buf, want, 0); while (r < 0 && errno == EINTR);
            if (r <= 0) { bad = 1; break; }
            n = (size_t)r;
        }
        for (size_t o = 0; o < n && !bad;) {
            ssize_t w = write(fd, buf + o, n - o);
            if (w > 0) o += (size_t)w;
            else if (w == 0 || errno != EINTR) bad = 1;
        }
        got += n;
        if (!bad && (got & 0xFFFFF) < n) on_progress("Spielstand wird hochgeladen …", (int)(got * 90 / clen));
    }
    if (bad || close(fd) != 0) {
        fd = -1;
        sb_add(&msg, "%s", T("Das Hochladen wurde unterbrochen"));
        goto fail;
    }
    fd = -1;
    on_progress("Spielstand wird zurückgeschrieben …", 92);
    int cnt = 0;
    if (saves_restore_file(zip, &cnt, NULL, NULL, err, sizeof(err)) == 0) {
        ok = 1;
        if (cnt > 1) sb_tr(&msg, "Spielstände wiederhergestellt: %d.", cnt);
        else sb_tr(&msg, "Spielstand wiederhergestellt.");
    } else {
        sb_add(&msg, "%s", err[0] ? err : T("Wiederherstellen fehlgeschlagen"));
    }
fail:
    free(buf);
    if (fd >= 0) close(fd);
    unlink(zip);
    char m[512];
    snprintf(m, sizeof(m), "%s", msg.p ? msg.p : "");
    job_done(j, ok, &msg, &res, 0);
    if (ok) respond_ok(c, 200);
    else respond_error(c, 500, m);
    if (!bad && got < clen) drain_body(c, clen - got);
}

static void api_tile(int c, int is_post) {
    if (is_post) {
        char gid[16], err[320];
        if (game_running(gid, sizeof(gid))) {
            game_error(gid, err, sizeof(err));
            respond_error(c, 409, err);
            return;
        }
        job_t *j = new_job(c, JOB_TILE);
        if (j) start_job(c, j, tile_worker);
        return;
    }
    tile_t t;
    char mine[24];
    tile_state(&t);
    formatter_content_version(mine, sizeof(mine));
    sbuf_t b = {0};
    sb_add(&b, "{\"smp\":%s,\"found\":%s,\"installed\":%s,\"registered\":", t.smp ? "true" : "false",
           t.found ? "true" : "false", t.installed ? "true" : "false");
    sb_str(&b, t.registered);
    sb_add(&b, ",\"folder\":");
    sb_str(&b, t.folder);
    sb_add(&b, ",\"formatter\":");
    sb_str(&b, mine);
    sb_add(&b, "}");
    respond_json(c, 200, &b);
    free(b.p);
}

static void api_delete(int c, const char *body) {
    char keys[MAX_DELETE * 9 + 16];   /* ~2,3 KiB auf dem Stapel (Verbindungs-Threads haben 512 KiB) */
    keys[0] = 0;
    const char *k = strstr(body, "\"keys\"");
    if (k) {
        k = ws(k + 6);
        if (*k == ':' && !json_string(ws(k + 1), keys, sizeof(keys))) keys[0] = 0;
    }
    if (!keys[0]) { respond_error(c, 400, "keys ist erforderlich (höchstens 256 Spiele auf einmal)"); return; }
    int nkeys = 1;
    for (const char *p = keys; *p; p++) nkeys += *p == ',';
    if (nkeys > MAX_DELETE) { respond_error(c, 400, "Höchstens 256 Spiele auf einmal"); return; }
    const char *u = strstr(body, "\"uninstall\"");
    int uninstall = 0;
    if (u) { u = ws(u + 11); if (*u == ':' && !strncmp(ws(u + 1), "true", 4)) uninstall = 1; }
    const char *xe = strstr(body, "\"extras\"");
    int with_extras = 0;
    if (xe) { xe = ws(xe + 8); if (*xe == ':' && !strncmp(ws(xe + 1), "true", 4)) with_extras = 1; }

    job_t *j = new_job(c, JOB_DELETE);
    if (!j) return;
    j->uninstall = uninstall;
    j->extras = with_extras;
    j->items = calloc(MAX_DELETE, sizeof(*j->items));
    if (!j->items) { free(j); respond_error(c, 500, "Kein Speicher"); return; }
    int ambiguous = 0, badkey = 0, own = 0;
    pthread_mutex_lock(&lib_lock);
    for (char *p = keys, *save = NULL, *tok; (tok = strtok_r(p, ",", &save)); p = NULL) {
        char *kend;
        uint32_t want = (uint32_t)strtoul(tok, &kend, 16);
        if (kend == tok || *kend) { badkey = 1; break; }   /* kein reiner Schlüssel: lieber gar nichts löschen */
        /* Schlüssel sind 32-Bit-Prüfsummen: passt einer zu mehreren Spielen (Zufall oder ein präparierter Stick),
         * wird nichts gelöscht statt womöglich ein anderes Spiel */
        int hits = 0;
        for (int i = 0; i < lib_n; i++)
            for (int t = 0; t < lib_snap[i].r.ntitles; t++) hits += path_key(lib_snap[i].r.titles[t].path) == want;
        if (hits > 1) { ambiguous = 1; break; }
        for (int i = 0; i < lib_n && j->nitems < MAX_DELETE; i++) {
            if (!lroot_name(&lib_snap[i])[0]) continue;
            for (int t = 0; t < lib_snap[i].r.ntitles && j->nitems < MAX_DELETE; t++) {
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
                snprintf(it->device, sizeof(it->device), "%s", lroot_name(&lib_snap[i]));
                it->size = tt->size;
                it->dev = lib_snap[i].dev;
                if (with_extras && lib_snap[i].xtra) item_extras(it, lib_snap[i].xtra[t]);
                if (!strcmp(tt->id, APP_TITLE)) own = 1;
            }
        }
    }
    if (with_extras) drop_shared_extras(j);
    pthread_mutex_unlock(&lib_lock);
    if (own) {   /* der Formatierer läuft aus diesem Ordner */
        free(j->items);
        free(j);
        respond_error(c, 409, N_("Der Formatierer kann sich nicht selbst löschen."));
        return;
    }
    if (ambiguous || badkey || !j->nitems) {
        free(j->items);
        free(j);
        respond_error(c, ambiguous ? 409 : 404, ambiguous ? N_("Auswahl nicht eindeutig – nichts gelöscht. Liste neu laden.")
                                                          : N_("Spiele nicht gefunden – Liste neu laden"));
        return;
    }
    snprintf(j->dev.name, sizeof(j->dev.name), "%s", j->items[0].device);
    start_job(c, j, delete_worker);
}

/* ------------------------------------------------------------ Spiele verschieben: kopieren und prüfen
 * Ein Spiel (Ordner oder Abbild) wird in einen versteckten Ordner des Ziel-Laufwerks kopiert (.usbfmt-move: ShadowMountPlus sucht dort
 * nicht), mit SHA-256 je Datei während des Kopierens, auf Wunsch danach von dort zurückgelesen und verglichen. Erst dann wird es an
 * dieselbe Stelle wie auf dem Quell-Laufwerk umbenannt. Das Original bleibt unberührt - gelöscht wird in einem eigenen Schritt. */

#define MOVE_RESERVE (256ull << 20)   /* so viel bleibt nach dem Kopieren auf dem Ziel frei */

typedef struct { char *rel; uint8_t sha[32]; uint64_t size; } mfile_t;

/* Größe je Eintrag (Ordner: alle Dateien ansehen, dazu die zugehörigen Dateien) und Summe */
static uint64_t move_sizes(const job_t *j, uint64_t *sizes) {
    uint64_t total = 0;
    for (int i = 0; i < j->nitems; i++) {
        const del_item_t *it = &j->items[i];
        sizes[i] = it->size ? it->size : tree_bytes(it->path, 0);
        for (int x = 0; j->extras && x < it->nex; x++) sizes[i] += tree_bytes(it->ex[x], 0);
        total += sizes[i];
    }
    return total;
}

/* Dateien über 4 GB (FAT32 nimmt sie nicht auf): Namen in b als JSON-Liste, höchstens 5 */
static void big_walk(const char *path, int depth, sbuf_t *b, int *cnt) {
    struct stat sb;
    if (depth > 32 || *cnt >= 5 || lstat(path, &sb) != 0) return;
    if (S_ISREG(sb.st_mode)) {
        if ((uint64_t)sb.st_size > 0xFFFFFFFFull) {
            const char *nm = strrchr(path, '/');
            sb_add(b, "%s", *cnt ? "," : "");
            sb_str(b, nm ? nm + 1 : path);
            (*cnt)++;
        }
        return;
    }
    if (!S_ISDIR(sb.st_mode)) return;
    DIR *d = opendir(path);
    if (!d) return;
    char *sub = malloc(1024);
    struct dirent *e;
    while (sub && *cnt < 5 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(sub, 1024, "%s/%s", path, e->d_name) < 1024) big_walk(sub, depth + 1, b, cnt);
    }
    free(sub);
    closedir(d);
}

typedef struct {
    mfile_t *f;
    int      n, cap;
    uint64_t total;       /* alle Bytes aller Spiele dieses Auftrags */
    uint64_t done;        /* davon kopiert (und bei verify: zusätzlich gelesen) */
    uint64_t weight;      /* total oder 2 * total */
    int      verify;
    char    *buf;
    char     step[320];
    char     err[256];
    time_t   last;
} mv_t;

static void mv_progress(mv_t *m, int force) {
    time_t now = time(NULL);
    if (!force && now == m->last) return;
    m->last = now;
    const uint64_t dn = m->done > m->weight ? m->weight : m->done;
    int pct = m->weight ? (int)(dn * 98 / m->weight) : 1;
    on_progress(m->step, pct < 1 ? 1 : pct);
}

static void mv_set_times(const char *path, const struct stat *sb) {
    struct timeval tv[2];
    tv[0].tv_sec = sb->st_atime; tv[0].tv_usec = 0;
    tv[1].tv_sec = sb->st_mtime; tv[1].tv_usec = 0;
    if (utimes(path, tv) != 0) { struct timespec ts[2] = {{sb->st_atime, 0}, {sb->st_mtime, 0}}; (void)utimensat(AT_FDCWD, path, ts, 0); }
}

static int mv_add(mv_t *m, const char *rel, const uint8_t sha[32], uint64_t size) {
    if (m->n == m->cap) {
        mfile_t *p = realloc(m->f, (size_t)(m->cap = m->cap ? m->cap * 2 : 256) * sizeof(*p));
        if (!p) return -1;
        m->f = p;
    }
    m->f[m->n].rel = strdup(rel);
    if (!m->f[m->n].rel) return -1;
    memcpy(m->f[m->n].sha, sha, 32);
    m->f[m->n].size = size;
    m->n++;
    return 0;
}

static void mv_free_list(mv_t *m) {
    for (int i = 0; i < m->n; i++) free(m->f[i].rel);
    free(m->f);
    m->f = NULL;
    m->n = m->cap = 0;
}

#define MV_BUF (1 << 20)

/* SHA-256 einer ganzen Datei; 0 = ok, -1 = Lesefehler, -2 = abgebrochen */
static int mv_hash_file(mv_t *m, const char *path, uint8_t out[32], uint64_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    sha256_t sh;
    sha256_init(&sh);
    *n = 0;
    for (;;) {
        if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) { close(fd); return -2; }
        ssize_t r = read(fd, m->buf, MV_BUF);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        sha256_update(&sh, m->buf, (size_t)r);
        *n += (uint64_t)r;
    }
    close(fd);
    sha256_final(&sh, out);
    return 0;
}

static int mv_copy_file(mv_t *m, const char *src, const char *dst, const char *rel, const struct stat *sb) {
    int in, out = -1, rc = -1;
    struct stat ds;
    if (lstat(dst, &ds) == 0) {   /* Rest eines früheren Laufs: nur eine gleiche Datei (Größe und Prüfsumme) wird übernommen, sonst neu kopiert */
        if (S_ISREG(ds.st_mode) && ds.st_size == sb->st_size) {
            uint8_t a[32], b[32];
            uint64_t na = 0, nb = 0;
            int ra = mv_hash_file(m, src, a, &na), rb = ra == 0 ? mv_hash_file(m, dst, b, &nb) : -1;
            if (ra == -2 || rb == -2) { snprintf(m->err, sizeof(m->err), "%s", T("Abgebrochen.")); return -1; }
            if (ra == 0 && rb == 0 && na == (uint64_t)sb->st_size && nb == na && memcmp(a, b, 32) == 0) {
                if (mv_add(m, rel, a, na)) { snprintf(m->err, sizeof(m->err), "%s", T("Kein Speicher")); return -1; }
                m->done += na;
                mv_progress(m, 0);
                mv_set_times(dst, sb);
                return 0;
            }
        }
        if (unlink(dst) != 0) { snprintf(m->err, sizeof(m->err), T("Die Datei %s lässt sich nicht anlegen: %s"), dst, strerror(errno)); return -1; }
    }
    in = open(src, O_RDONLY);
    if (in < 0) { snprintf(m->err, sizeof(m->err), T("%s lässt sich nicht lesen: %s"), src, strerror(errno)); return -1; }
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL, sb->st_mode & 0777 ? sb->st_mode & 0777 : 0644);
    if (out < 0) { snprintf(m->err, sizeof(m->err), T("Die Datei %s lässt sich nicht anlegen: %s"), dst, strerror(errno)); close(in); return -1; }
    sha256_t sh;
    uint64_t copied = 0;
    sha256_init(&sh);
    for (;;) {
        if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) { snprintf(m->err, sizeof(m->err), "%s", T("Abgebrochen.")); goto out; }
        ssize_t r = read(in, m->buf, MV_BUF);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { snprintf(m->err, sizeof(m->err), T("%s lässt sich nicht lesen: %s"), src, strerror(errno)); goto out; }
        if (r == 0) break;
        sha256_update(&sh, m->buf, (size_t)r);
        for (ssize_t off = 0; off < r;) {
            ssize_t w = write(out, m->buf + off, (size_t)(r - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { write_error(w < 0 ? errno : EIO, m->err, sizeof(m->err)); goto out; }
            off += w;
        }
        m->done += (uint64_t)r;
        copied += (uint64_t)r;
        mv_progress(m, 0);
        plat_throttle();   /* Tests (USBFMT_SLOW): Kopieren bremsen, damit "Abbrechen" sicher mitten drin ankommt */
    }
    if (copied != (uint64_t)sb->st_size) {   /* Datei wuchs oder schrumpfte beim Kopieren (z. B. noch beschrieben): Kopie wäre unvollständig */
        snprintf(m->err, sizeof(m->err), T("Die Quelle hat sich während des Kopierens geändert: %s"), src);
        goto out;
    }
    if (fsync(out) != 0 && errno != EINVAL) { write_error(errno, m->err, sizeof(m->err)); goto out; }
    if (sb->st_size > 0 && getenv("USBFMT_MOVE_CORRUPT")) {   /* Test: ein beschädigtes Byte in der Kopie muss die Prüfung bemerken */
        uint8_t bad = 0x55;
        (void)!pwrite(out, &bad, 1, 0);
    }
    uint8_t d[32];
    sha256_final(&sh, d);
    rc = mv_add(m, rel, d, (uint64_t)sb->st_size);
    if (rc) snprintf(m->err, sizeof(m->err), "%s", T("Kein Speicher"));
out:
    if (close(out) != 0 && rc == 0) { write_error(errno, m->err, sizeof(m->err)); rc = -1; }
    close(in);
    if (rc == 0) mv_set_times(dst, sb);
    return rc;
}

/* rel: Pfad des Eintrags unterhalb des Spiels ("" für das Spiel selbst) */
/* Liegt ep direkt in einem Ordner "backports" bzw. "backport"? (nur der Elternordnername zählt, nicht irgendein Pfadteil) */
static int is_backport_dir(const char *ep) {
    const char *sl = strrchr(ep, 47);
    if (!sl || sl == ep) return 0;
    const char *q = sl - 1;
    while (q > ep && *q != 47) q--;
    if (*q == 47) q++;
    const size_t l = (size_t)(sl - q);
    return (l == 9 && !strncmp(q, "backports", 9)) || (l == 8 && !strncmp(q, "backport", 8));
}

static int mv_copy_tree(mv_t *m, const char *src, const char *dst, const char *rel, int depth) {
    struct stat sb;
    if (depth > 32 ? (errno = ELOOP, 1) : lstat(src, &sb) != 0) { snprintf(m->err, sizeof(m->err), T("%s lässt sich nicht lesen: %s"), src, strerror(errno)); return -1; }
    if (S_ISREG(sb.st_mode)) return mv_copy_file(m, src, dst, rel, &sb);
    if (!S_ISDIR(sb.st_mode)) { snprintf(m->err, sizeof(m->err), T("Nicht unterstützte Datei (Verknüpfung oder Sonderdatei): %s"), src); return -1; }
    if (mkdir(dst, 0777) != 0 && errno != EEXIST) { snprintf(m->err, sizeof(m->err), T("Der Ordner %s lässt sich nicht anlegen: %s"), dst, strerror(errno)); return -1; }
    DIR *d = opendir(src);
    if (!d) { snprintf(m->err, sizeof(m->err), T("%s lässt sich nicht lesen: %s"), src, strerror(errno)); return -1; }
    char *s2 = malloc(1024), *d2 = malloc(1024), *r2 = malloc(1024);
    int rc = 0;
    struct dirent *e;
    if (!s2 || !d2 || !r2) { snprintf(m->err, sizeof(m->err), "%s", T("Kein Speicher")); rc = -1; }
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(s2, 1024, "%s/%s", src, e->d_name) >= 1024 || snprintf(d2, 1024, "%s/%s", dst, e->d_name) >= 1024 ||
            snprintf(r2, 1024, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >= 1024) {
            snprintf(m->err, sizeof(m->err), "%s", T("Pfad zu lang"));
            rc = -1;
            break;
        }
        rc = mv_copy_tree(m, s2, d2, r2, depth + 1);
    }
    closedir(d);
    free(s2); free(d2); free(r2);
    if (rc == 0) mv_set_times(dst, &sb);
    return rc;
}

/* Kopie zurücklesen und mit den beim Kopieren gerechneten Prüfsummen vergleichen */
static int mv_verify(mv_t *m, const char *dst_root, int first) {
    for (int i = first; i < m->n; i++) {
        char p[1024];
        if (snprintf(p, sizeof(p), "%s%s%s", dst_root, m->f[i].rel[0] ? "/" : "", m->f[i].rel) >= (int)sizeof(p)) return -1;
        int fd = open(p, O_RDONLY);
        if (fd < 0) { snprintf(m->err, sizeof(m->err), T("%s lässt sich nicht lesen: %s"), p, strerror(errno)); return -1; }
        sha256_t sh;
        sha256_init(&sh);
        uint64_t got = 0;
        for (;;) {
            if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) { close(fd); snprintf(m->err, sizeof(m->err), "%s", T("Abgebrochen.")); return -1; }
            ssize_t r = read(fd, m->buf, MV_BUF);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            sha256_update(&sh, m->buf, (size_t)r);
            got += (uint64_t)r;
            m->done += (uint64_t)r;
            mv_progress(m, 0);
        }
        close(fd);
        uint8_t d[32];
        sha256_final(&sh, d);
        if (got != m->f[i].size || memcmp(d, m->f[i].sha, 32) != 0) {
            snprintf(m->err, sizeof(m->err), T("Die Kopie weicht vom Original ab (%s) – nichts übernommen."), m->f[i].rel);
            return -1;
        }
    }
    return 0;
}

static int mkdirs_to(const char *path) {   /* alle Ordner bis einschließlich path (mode 0777) */
    char t[1024];
    snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++)
        if (*p == '/') { *p = 0; if (mkdir(t, 0777) != 0 && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(t, 0777) != 0 && errno != EEXIST ? -1 : 0;
}

/* Rest-Ordner früherer Läufe im Zwischenordner: Name = Schlüssel des Spiels (8 Hex-Ziffern), ggf. "_x<N>" für Zugehöriges */
static int stage_name_key(const char *name, uint32_t *key) {
    if (strlen(name) < 8) return 0;
    for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)name[i])) return 0;
    if (name[8] && name[8] != '_') return 0;
    char hex[9];
    memcpy(hex, name, 8);
    hex[8] = 0;
    *key = (uint32_t)strtoul(hex, NULL, 16);
    return 1;
}

/* Alles im Zwischenordner entfernen, was nicht zu den Spielen dieses Auftrags gehört */
static void stage_cleanup(const char *stage, const job_t *j) {
    DIR *d = opendir(stage);
    if (!d) return;
    struct dirent *e;
    char p[1100];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        uint32_t key = 0;
        int mine = 0;
        if (stage_name_key(e->d_name, &key))
            for (int i = 0; i < j->nitems; i++) mine |= path_key(j->items[i].path) == key;
        if (mine && strchr(e->d_name, '_')) mine = 0;   /* Zugehöriges wird immer neu kopiert */
        if (!mine && snprintf(p, sizeof(p), "%s/%s", stage, e->d_name) < (int)sizeof(p)) (void)remove_tree(p, 0, NULL);
    }
    closedir(d);
}

/* Bytes, die ein früherer Lauf für diese Spiele schon am Ziel liegen hat (zählen beim Platzbedarf nicht doppelt) */
static uint64_t staged_bytes(const char *troot, const job_t *j) {
    uint64_t sum = 0;
    char p[1100];
    for (int i = 0; i < j->nitems; i++)
        if (snprintf(p, sizeof(p), "%s/%s/%08x", troot, MOVE_DIR, path_key(j->items[i].path)) < (int)sizeof(p)) sum += tree_bytes(p, 0);
    return sum;
}

static void *move_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0}, items = {0}, errs = {0}, xwarn = {0};
    char s1[24], s2[24], stage[640];
    int ok_n = 0, fail_n = 0, was_cancel = 0, kept = 0;
    mv_t m;
    memset(&m, 0, sizeof(m));
    m.verify = j->verify;
    m.buf = malloc(MV_BUF);
    const char *troot = j->path;   /* Wurzel des Ziel-Laufwerks */
    snprintf(stage, sizeof(stage), "%s/%s", troot, MOVE_DIR);
    if (!m.buf) { sb_tr(&msg, "Kein Speicher"); free(j->items); j->items = NULL; job_done(j, 0, &msg, &res, 0); return NULL; }
    set_flags(1, 0);

    /* Gesamtgröße (Ordner: alle Dateien ansehen) und Platz am Ziel */
    on_progress(N_("Größe ermitteln"), 1);
    uint64_t sizes[MAX_DELETE];
    m.total = move_sizes(j, sizes);
    m.weight = m.verify ? m.total * 2 : m.total;
    pthread_mutex_lock(&lock);
    st.total_ptr = &m.weight;   /* Menge und Tempo unter dem Balken */
    pthread_mutex_unlock(&lock);
    struct statvfs vf;
    const uint64_t have = staged_bytes(troot, j);
    if (statvfs(troot, &vf) != 0 || (uint64_t)vf.f_bavail * vf.f_frsize + have < m.total + MOVE_RESERVE) {
        sb_tr(&msg, "Auf dem Ziel ist nicht genug Platz: gebraucht werden %s, frei sind %s.", size_text(m.total + MOVE_RESERVE, s1, sizeof(s1)),
              size_text(statvfs(troot, &vf) == 0 ? (uint64_t)vf.f_bavail * vf.f_frsize : 0, s2, sizeof(s2)));
        goto fin;
    }
    if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) { sb_tr(&msg, "Abgebrochen – nichts kopiert."); goto fin; }
    if (root_dev(troot) != j->tdev) { sb_tr(&msg, "Das Ziel hat sich seit der Auswahl geändert – nichts kopiert. Liste neu laden."); goto fin; }
    stage_cleanup(stage, j);   /* Reste anderer Spiele weg; die der jetzigen Auswahl bleiben und werden fortgesetzt */
    if (mkdirs_to(stage) != 0) { sb_tr(&msg, "Der Ordner %s lässt sich nicht anlegen: %s", stage, strerror(errno)); goto fin; }

    for (int i = 0; i < j->nitems; i++) {
        const del_item_t *it = &j->items[i];
        char gstage[720], final_path[1024], rel[300];
        char bbuf[128];
        const char *mbase = move_base(it->root, bbuf, sizeof(bbuf));
        const size_t rl = strlen(mbase);
        if (root_dev(it->root) != it->dev || (!j->pkgmode && !lib_same_game(it->path, it->id, it->size))) {
            sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, T("Spiel hat sich seit der Suche geändert – nichts kopiert. Liste neu laden."));
            fail_n++;
            continue;
        }
        if (strncmp(it->path, mbase, rl) != 0 || it->path[rl] != '/') { sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, T("Pfad liegt nicht im durchsuchten Ordner")); fail_n++; continue; }
        snprintf(rel, sizeof(rel), "%s", it->path + rl + 1);
        if (!j->pkgmode && !strcmp(troot, internal_base()) && strncmp(rel, "homebrew/", 9) != 0 && strncmp(rel, "etaHEN/games/", 13) != 0) {
            const char *nm = strrchr(it->path, '/');   /* intern liegen Spiele nur in homebrew/ bzw. etaHEN/games/ */
            snprintf(rel, sizeof(rel), "homebrew/%s", nm ? nm + 1 : it->path);
        }
        snprintf(final_path, sizeof(final_path), "%s/%s", troot, rel);
        snprintf(gstage, sizeof(gstage), "%s/%08x", stage, path_key(it->path));
        struct stat sb;
        if (lstat(final_path, &sb) == 0) {
            sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, T("liegt am Ziel schon vor – nichts überschrieben"));
            fail_n++;
            continue;
        }
        snprintf(m.step, sizeof(m.step), T("%s: %s (%d von %d)"), j->pkgmode ? T("Paket kopieren") : T("Spiel kopieren"), it->name, i + 1, j->nitems);
        mv_progress(&m, 1);
        const uint64_t done0 = m.done;
        const int first = m.n;
        int drop = 0;
        m.err[0] = 0;
        int rc = mv_copy_tree(&m, it->path, gstage, "", 0);
        if (rc == 0 && m.verify) {
            snprintf(m.step, sizeof(m.step), T("%s: %s (%d von %d)"), T("Kopie prüfen"), it->name, i + 1, j->nitems);
            mv_progress(&m, 1);
            rc = mv_verify(&m, gstage, first);
            if (rc != 0 && !__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) drop = 1;   /* abweichende Kopie: nicht aufheben */
        }
        if (rc == 0) {
            char parent[1024];
            snprintf(parent, sizeof(parent), "%s", final_path);
            char *sl = strrchr(parent, '/');
            if (sl) *sl = 0;
            struct stat late;
            if (lstat(final_path, &late) == 0) {   /* erst während des Kopierens entstanden: rename würde eine Datei still ersetzen */
                snprintf(m.err, sizeof(m.err), "%s", T("liegt am Ziel schon vor – nichts überschrieben"));
                rc = -1;
            } else if (mkdirs_to(parent) != 0 || rename(gstage, final_path) != 0) {
                snprintf(m.err, sizeof(m.err), T("Übernehmen am Ziel fehlgeschlagen: %s"), strerror(errno));
                rc = -1;
            }
        }
        mv_free_list(&m);
        /* zugehörige Dateien (Backport-Ordner, Prüfsummen): was am Ziel schon liegt, bleibt unberührt */
        for (int x = 0; rc == 0 && j->extras && x < it->nex; x++) {
            const char *ep = it->ex[x], *en = strrchr(ep, '/');
            char xfinal[1024], xstage[800];
            en = en ? en + 1 : ep;
            struct stat xs;
            if (is_backport_dir(ep)) {   /* .../backports/<Name>: neben dem Spiel, sonst im homebrew-Ordner des Ziels */
                char par[1024], gpar[1024];
                snprintf(par, sizeof(par), "%s", ep);
                for (int up = 0; up < 2; up++) { char *s2 = strrchr(par, '/'); if (s2) *s2 = 0; }
                snprintf(gpar, sizeof(gpar), "%s", it->path);
                char *s3 = strrchr(gpar, '/');
                if (s3) *s3 = 0;
                if (!strcmp(par, gpar)) {
                    snprintf(xfinal, sizeof(xfinal), "%s", final_path);
                    char *s4 = strrchr(xfinal, '/');
                    if (s4) *s4 = 0;
                    size_t xl = strlen(xfinal);
                    snprintf(xfinal + xl, sizeof(xfinal) - xl, "/backports/%s", en);
                } else snprintf(xfinal, sizeof(xfinal), "%s/homebrew/backports/%s", troot, en);
            } else {
                snprintf(xfinal, sizeof(xfinal), "%s", final_path);
                char *sl2 = strrchr(xfinal, '/');
                if (sl2) snprintf(sl2 + 1, sizeof(xfinal) - (size_t)(sl2 + 1 - xfinal), "%s", en);
            }
            if (lstat(xfinal, &xs) == 0) continue;
            snprintf(xstage, sizeof(xstage), "%s_x%d", gstage, x);
            snprintf(m.step, sizeof(m.step), T("%s: %s (%d von %d)"), T("Zugehörige Dateien kopieren"), it->name, i + 1, j->nitems);
            mv_progress(&m, 1);
            const int xfirst = m.n;
            m.err[0] = 0;
            int xrc = mv_copy_tree(&m, ep, xstage, "", 0);
            if (xrc == 0 && m.verify) xrc = mv_verify(&m, xstage, xfirst);
            if (xrc == 0) {
                char xparent[1024];
                snprintf(xparent, sizeof(xparent), "%s", xfinal);
                char *sl3 = strrchr(xparent, '/');
                if (sl3) *sl3 = 0;
                struct stat xlate;
                if (lstat(xfinal, &xlate) == 0) {
                    snprintf(m.err, sizeof(m.err), "%s", T("liegt am Ziel schon vor – nichts überschrieben"));
                    xrc = -1;
                } else if (mkdirs_to(xparent) != 0 || rename(xstage, xfinal) != 0) {
                    snprintf(m.err, sizeof(m.err), T("Übernehmen am Ziel fehlgeschlagen: %s"), strerror(errno));
                    xrc = -1;
                }
            }
            mv_free_list(&m);
            if (xrc != 0) {
                (void)remove_tree(xstage, 0, NULL);
                sb_add(&xwarn, "%s", xwarn.len ? "; " : "");   /* das Spiel selbst ist da: nur melden */
                sb_tr(&xwarn, "%s: Zugehöriges nicht kopiert (%s)", it->name, m.err[0] ? m.err : en);
            }
        }
        if (rc != 0) {
            if (drop) (void)remove_tree(gstage, 0, NULL);
            else kept = 1;   /* die Teilkopie bleibt liegen: derselbe Auftrag setzt dort fort */
            if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) {
                sb_tr(&msg, "Abgebrochen – kopiert und geprüft: %d von %d.", ok_n, j->nitems);
                sb_add(&msg, " %s", T("Die Teilkopie bleibt erhalten: dieselbe Auswahl erneut kopieren setzt fort."));
                was_cancel = 1;
                goto fin;
            }
            sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, m.err[0] ? m.err : T("Kopieren fehlgeschlagen"));
            fail_n++;
            m.done = done0 + sizes[i] * (m.verify ? 2 : 1);   /* Balken läuft über das fehlgeschlagene Spiel hinweg weiter */
            continue;
        }
        if (!m.verify) m.done = done0 + sizes[i];
        ok_n++;
        sb_add(&items, "%s%08x", items.len ? "," : "", path_key(it->path));   /* Schlüssel der kopierten Originale (für "Originale löschen") */
    }
    if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) was_cancel = 1;   /* auch ein Abbruch beim Kopieren der zugehörigen Dateien */
    if (was_cancel && ok_n) {
        sb_tr(&msg, "Abgebrochen – kopiert und geprüft: %d von %d.", ok_n, j->nitems);
    } else if (ok_n) {
        if (j->pkgmode) sb_tr(&msg, "Pakete kopiert%s: %d (nach %s). Die Originale sind noch vorhanden.", m.verify ? T(" und geprüft") : "", ok_n, j->dev.name);
        else sb_tr(&msg, "Spiele kopiert%s: %d (nach %s). Die Originale sind noch vorhanden.", m.verify ? T(" und geprüft") : "", ok_n, j->dev.name);
        if (fail_n) sb_tr(&msg, " Nicht kopiert: %d – %s", fail_n, errs.p ? errs.p : "");
        if (xwarn.len) sb_add(&msg, " %s.", xwarn.p);
    } else {
        sb_tr(&msg, "Nichts kopiert: %s", errs.p ? errs.p : "");
    }
    if (kept && !was_cancel) sb_add(&msg, " %s", T("Die Teilkopie bleibt erhalten: dieselbe Auswahl erneut kopieren setzt fort."));
fin:
    (void)rmdir(stage);   /* nur, wenn leer */
    pthread_mutex_lock(&lock);
    st.total_ptr = NULL;   /* m lebt nur in diesem Thread */
    pthread_mutex_unlock(&lock);
    set_flags(0, 0);
    sb_add(&res, "{\"copied\":%d,\"failed\":%d,\"keys\":\"%s\"}", ok_n, fail_n, items.p ? items.p : "");
    free(m.buf);
    mv_free_list(&m);
    free(items.p);
    free(errs.p);
    free(xwarn.p);
    free(j->items);
    j->items = NULL;
    job_done(j, ok_n > 0 && fail_n == 0 && !was_cancel, &msg, &res, 1);
    return NULL;
}

/* "check":true: nichts kopieren, nur sagen, ob es passt (Platz am Ziel, FAT32 und Dateien über 4 GB) */
static void move_check(int c, const job_t *j) {
    uint64_t sizes[MAX_DELETE];
    const uint64_t total = move_sizes(j, sizes);
    struct statvfs vf;
    const uint64_t freeb = (statvfs(j->path, &vf) == 0 ? (uint64_t)vf.f_bavail * vf.f_frsize : 0) + staged_bytes(j->path, j);   /* Reste eines früheren Laufs werden weiterverwendet */
    const int fat = plat_path_fat32(j->path);
    sbuf_t b = {0};
    sb_add(&b, "{\"need\":%llu,\"free\":%llu,\"reserve\":%llu,\"fits\":%s,\"fat32\":%s,\"big\":[", (unsigned long long)total,
           (unsigned long long)freeb, (unsigned long long)MOVE_RESERVE, total + MOVE_RESERVE <= freeb ? "true" : "false", fat ? "true" : "false");
    int cnt = 0;
    for (int i = 0; fat && i < j->nitems; i++) {
        big_walk(j->items[i].path, 0, &b, &cnt);
        for (int x = 0; j->extras && x < j->items[i].nex; x++) big_walk(j->items[i].ex[x], 0, &b, &cnt);
    }
    sb_add(&b, "]}");
    respond_json(c, 200, &b);
    free(b.p);
}

/* POST /api/move {"keys":"1a2b3c4d,…","device":"da2","verify":true} - Schlüssel aus /api/library; kopiert nach <Ziel-Wurzel>/<gleicher Pfad> */
static void api_move(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    const char *keys0 = n > 0 ? kv_get(kv, n, "keys") : NULL, *dev = n > 0 ? kv_get(kv, n, "device") : NULL;
    const char *pkgs0 = n > 0 ? kv_get(kv, n, "pkgs") : NULL;   /* Schlüssel von .pkg-Dateien (/api/library: pkgfiles) statt von Spielen */
    const int pkgmode = pkgs0 && pkgs0[0] && (!keys0 || !keys0[0]);
    if (pkgmode) keys0 = pkgs0;
    if (!keys0 || !keys0[0] || !dev) { respond_error(c, 400, "keys und device sind erforderlich"); return; }
    char keys[MAX_DELETE * 9 + 16];
    snprintf(keys, sizeof(keys), "%s", keys0);
    int nkeys = 1;
    for (const char *p = keys; *p; p++) nkeys += *p == ',';
    if (nkeys > MAX_DELETE) { respond_error(c, 400, "Höchstens 256 Spiele auf einmal"); return; }
    char gid[16], gerr[320];
    if (game_running(gid, sizeof(gid))) { game_error(gid, gerr, sizeof(gerr)); respond_error(c, 409, gerr); return; }

    job_t *j = new_job(c, JOB_MOVE);
    if (!j) return;
    j->verify = kv_true(kv, n, "verify");
    j->pkgmode = pkgmode;
    j->extras = kv_true(kv, n, "extras");
    j->items = calloc(MAX_DELETE, sizeof(*j->items));
    if (!j->items) { job_free(j); respond_error(c, 500, "Kein Speicher"); return; }
    int ambiguous = 0, badkey = 0, same_dev = 0, have_target = 0;
    pthread_mutex_lock(&lib_lock);
    if (!strcmp(dev, "intern")) {   /* interner Speicher der PS5 */
        snprintf(j->path, sizeof(j->path), "%s", internal_base());
        snprintf(j->dev.name, sizeof(j->dev.name), "%s", "intern");
        have_target = 1;
    } else if (!strncmp(dev, "ext", 3) && dev[3] >= '0' && dev[3] <= '9' && !dev[4]) {   /* M.2-Erweiterung */
        struct stat xs;
        snprintf(j->path, sizeof(j->path), "/mnt/%.8s", dev);
        snprintf(j->dev.name, sizeof(j->dev.name), "%.8s", dev);
        have_target = stat(j->path, &xs) == 0 && S_ISDIR(xs.st_mode) && root_dev(j->path) != root_dev("/mnt");   /* eingehängt, nicht nur ein leerer Ordner */
    }
    for (int i = 0; i < lib_n; i++)   /* Ziel: eingebundenes USB-Laufwerk dieses Namens */
        if (lib_snap[i].usb && !strcmp(lib_snap[i].device, dev) && !have_target) {
            snprintf(j->path, sizeof(j->path), "%s", lib_snap[i].r.root);
            snprintf(j->dev.name, sizeof(j->dev.name), "%.15s", dev);
            have_target = 1;
        }
    for (char *p = keys, *save = NULL, *tok; have_target && (tok = strtok_r(p, ",", &save)); p = NULL) {
        char *kend;
        uint32_t want = (uint32_t)strtoul(tok, &kend, 16);
        if (kend == tok || *kend) { badkey = 1; break; }
        int hits = 0;
        for (int i = 0; i < lib_n; i++) {
            if (pkgmode) for (int t = 0; t < lib_snap[i].r.npkglist; t++) hits += path_key(lib_snap[i].r.pkglist[t].path) == want;
            else for (int t = 0; t < lib_snap[i].r.ntitles; t++) hits += path_key(lib_snap[i].r.titles[t].path) == want;
        }
        if (hits > 1) { ambiguous = 1; break; }
        for (int i = 0; i < lib_n && j->nitems < MAX_DELETE; i++) {
            if (!lroot_name(&lib_snap[i])[0]) continue;
            if (pkgmode) {
                for (int t = 0; t < lib_snap[i].r.npkglist && j->nitems < MAX_DELETE; t++) {
                    const lib_pkg_t *pk = &lib_snap[i].r.pkglist[t];
                    if (path_key(pk->path) != want) continue;
                    if (!strcmp(lroot_name(&lib_snap[i]), dev)) { same_dev = 1; continue; }
                    const char *nm = strrchr(pk->path, '/');
                    int dup = 0;
                    for (int d2 = 0; d2 < j->nitems; d2++) dup |= !strcmp(j->items[d2].path, pk->path);
                    if (dup) continue;
                    del_item_t *it = &j->items[j->nitems++];
                    snprintf(it->id, sizeof(it->id), "%s", pk->title_id);
                    snprintf(it->name, sizeof(it->name), "%.127s", nm ? nm + 1 : pk->path);
                    snprintf(it->path, sizeof(it->path), "%s", pk->path);
                    snprintf(it->root, sizeof(it->root), "%s", lib_snap[i].r.root);
                    snprintf(it->device, sizeof(it->device), "%s", lroot_name(&lib_snap[i]));
                    it->size = pk->size;
                    it->dev = lib_snap[i].dev;
                }
                continue;
            }
            for (int t = 0; t < lib_snap[i].r.ntitles && j->nitems < MAX_DELETE; t++) {
                const lib_title_t *tt = &lib_snap[i].r.titles[t];
                if (path_key(tt->path) != want) continue;
                if (!strcmp(lroot_name(&lib_snap[i]), dev)) { same_dev = 1; continue; }
                int dup = 0;
                for (int d2 = 0; d2 < j->nitems; d2++) dup |= !strcmp(j->items[d2].path, tt->path);
                if (dup) continue;
                del_item_t *it = &j->items[j->nitems++];
                if (j->extras && lib_snap[i].xtra) item_extras(it, lib_snap[i].xtra[t]);
                snprintf(it->id, sizeof(it->id), "%s", tt->id);
                snprintf(it->name, sizeof(it->name), "%s", tt->name[0] ? tt->name : tt->id);
                snprintf(it->path, sizeof(it->path), "%s", tt->path);
                snprintf(it->root, sizeof(it->root), "%s", lib_snap[i].r.root);
                snprintf(it->device, sizeof(it->device), "%s", lroot_name(&lib_snap[i]));
                it->size = tt->size;
                it->dev = lib_snap[i].dev;
            }
        }
    }
    j->tdev = have_target ? root_dev(j->path) : (dev_t)-1;
    pthread_mutex_unlock(&lib_lock);
    if (!have_target || ambiguous || badkey || same_dev || !j->nitems) {
        job_free(j);
        respond_error(c, ambiguous ? 409 : 404,
                      !have_target ? N_("Ziel-Laufwerk nicht eingebunden – Liste neu laden")
                      : same_dev ? N_("Quelle und Ziel sind dasselbe Laufwerk")
                      : ambiguous ? N_("Auswahl nicht eindeutig – nichts kopiert. Liste neu laden.")
                                  : N_("Spiele nicht gefunden – Liste neu laden"));
        return;
    }
    if (kv_true(kv, n, "check")) {
        move_check(c, j);
        job_free(j);
        return;
    }
    start_job(c, j, move_worker);
}


/* ------------------------------------------------------------ .pkg-Dateien löschen
 * POST /api/pkg/delete {"keys":"1a2b3c4d,…"} - Schlüssel aus /api/library (pkgfiles); gelöscht wird nur, was die Suche fand, nur auf USB-Laufwerken.
 * Ein danach leerer Titel-Ordner (ps4/<CUSA…>, PS5-Updates/<Titel>, pkg/<Name>) wird mit entfernt. */
static void *pkgdel_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0}, errs = {0};
    int ok_n = 0, fail_n = 0;
    set_flags(1, 0);
    for (int i = 0; i < j->nitems; i++) {
        const del_item_t *it = &j->items[i];
        char step[360];
        if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) {   /* Rest bleibt unangetastet */
            sb_add(&errs, "%s%s", errs.len ? "; " : "", T("Abgebrochen."));
            fail_n += j->nitems - i;
            break;
        }
        snprintf(step, sizeof(step), T("%s: %s (%d von %d)"), T("Pakete löschen"), it->name, i + 1, j->nitems);
        on_progress(step, 1 + i * 98 / j->nitems);
        struct stat sb;
        const size_t rl = strlen(it->root);
        if (root_dev(it->root) != it->dev || strncmp(it->path, it->root, rl) != 0 || it->path[rl] != '/' || lstat(it->path, &sb) != 0 || !S_ISREG(sb.st_mode) ||
            (uint64_t)sb.st_size != it->size) {   /* Stick getauscht oder Datei seit der Suche verändert */
            sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, T("hat sich seit der Suche geändert – nichts gelöscht. Liste neu laden."));
            fail_n++;
            continue;
        }
        if (unlink(it->path) != 0) {
            sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, strerror(errno));
            fail_n++;
            continue;
        }
        ok_n++;
        char dir[300];
        snprintf(dir, sizeof(dir), "%s", it->path);
        char *sl = strrchr(dir, '/');
        if (sl) {
            *sl = 0;
            const char *rel = dir + rl + 1;
            if (strlen(dir) > rl && (!strncmp(rel, "ps4/", 4) || !strncmp(rel, "PS5-Updates/", 12) || !strncmp(rel, "pkg/", 4))) (void)rmdir(dir);   /* nur, wenn leer */
        }
    }
    if (ok_n) {
        sb_tr(&msg, "Pakete gelöscht: %d.", ok_n);
        if (fail_n) sb_tr(&msg, " Nicht gelöscht: %d – %s", fail_n, errs.p ? errs.p : "");
    } else {
        sb_tr(&msg, "Nichts gelöscht: %s", errs.p ? errs.p : "");
    }
    sb_add(&res, "{\"deleted\":%d,\"failed\":%d}", ok_n, fail_n);
    set_flags(0, 0);
    free(errs.p);
    free(j->items);
    j->items = NULL;
    job_done(j, ok_n > 0 && fail_n == 0, &msg, &res, 1);
    return NULL;
}

static void api_pkg_delete(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    const char *keys0 = n > 0 ? kv_get(kv, n, "keys") : NULL;
    if (!keys0 || !keys0[0]) { respond_error(c, 400, "keys ist erforderlich"); return; }
    char keys[MAX_DELETE * 9 + 16];
    snprintf(keys, sizeof(keys), "%s", keys0);
    int nkeys = 1;
    for (const char *p = keys; *p; p++) nkeys += *p == ',';
    if (nkeys > MAX_DELETE) { respond_error(c, 400, "Höchstens 256 Dateien auf einmal"); return; }
    char gid[16], gerr[320];
    if (game_running(gid, sizeof(gid))) { game_error(gid, gerr, sizeof(gerr)); respond_error(c, 409, gerr); return; }
    job_t *j = new_job(c, JOB_PKGDEL);
    if (!j) return;
    j->pkgmode = 1;
    j->items = calloc(MAX_DELETE, sizeof(*j->items));
    if (!j->items) { job_free(j); respond_error(c, 500, "Kein Speicher"); return; }
    int ambiguous = 0, badkey = 0;
    pthread_mutex_lock(&lib_lock);
    for (char *p = keys, *save = NULL, *tok; (tok = strtok_r(p, ",", &save)); p = NULL) {
        char *kend;
        uint32_t want = (uint32_t)strtoul(tok, &kend, 16);
        if (kend == tok || *kend) { badkey = 1; break; }
        int hits = 0;
        for (int i = 0; i < lib_n; i++)
            for (int t = 0; t < lib_snap[i].r.npkglist; t++) hits += path_key(lib_snap[i].r.pkglist[t].path) == want;
        if (hits > 1) { ambiguous = 1; break; }
        for (int i = 0; i < lib_n && j->nitems < MAX_DELETE; i++) {
            if (!lroot_name(&lib_snap[i])[0]) continue;
            for (int t = 0; t < lib_snap[i].r.npkglist && j->nitems < MAX_DELETE; t++) {
                const lib_pkg_t *pk = &lib_snap[i].r.pkglist[t];
                if (path_key(pk->path) != want) continue;
                const char *nm = strrchr(pk->path, '/');
                del_item_t *it = &j->items[j->nitems++];
                snprintf(it->id, sizeof(it->id), "%s", pk->title_id);
                snprintf(it->name, sizeof(it->name), "%.127s", nm ? nm + 1 : pk->path);
                snprintf(it->path, sizeof(it->path), "%s", pk->path);
                snprintf(it->root, sizeof(it->root), "%s", lib_snap[i].r.root);
                snprintf(it->device, sizeof(it->device), "%s", lroot_name(&lib_snap[i]));
                it->size = pk->size;
                it->dev = lib_snap[i].dev;
            }
        }
    }
    pthread_mutex_unlock(&lib_lock);
    if (ambiguous || badkey || !j->nitems) {
        job_free(j);
        respond_error(c, ambiguous ? 409 : 404, ambiguous ? N_("Auswahl nicht eindeutig – nichts gelöscht. Liste neu laden.")
                                                          : N_("Pakete nicht gefunden – Liste neu laden"));
        return;
    }
    snprintf(j->dev.name, sizeof(j->dev.name), "%s", j->items[0].device);
    start_job(c, j, pkgdel_worker);
}

/* ------------------------------------------------------------ Spiele prüfen
 * POST /api/game/verify {"keys":"1a2b3c4d,…"} - Schlüssel aus /api/library (titles). Liest jedes Abbild ganz und vergleicht die SHA-256
 * mit der Prüfsummen-Datei daneben (<Datei>.sha256 oder <Name>.sha256, erste 64 Hex-Zeichen). Ordner-Spiele haben keine; nichts wird verändert. */
static int hexv(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }

static int read_sha_file(const char *path, uint8_t out[32]) {
    char buf[512];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    for (int i = 0; i < 32; i++) {
        int a = hexv(p[2 * i]), b = a < 0 ? -1 : hexv(p[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return p[64] == 0 || p[64] == ' ' || p[64] == '\t' || p[64] == '\n' || p[64] == '\r' || p[64] == '*' ? 0 : -1;
}

/* Eine Datei ganz lesen und hashen; 0 = ok, -1 = nicht lesbar, -2 = abgebrochen */
static int gv_hash(const del_item_t *it, char *buf, uint8_t out[32], uint64_t *done, uint64_t total, time_t *last, const char *step) {
    struct stat sb;
    int fd = root_dev(it->root) == it->dev && lstat(it->path, &sb) == 0 && S_ISREG(sb.st_mode) ? open(it->path, O_RDONLY) : -1;
    if (fd < 0) return -1;
    sha256_t sh;
    sha256_init(&sh);
    int rc = 0;
    for (;;) {
        if (__atomic_load_n(&cancel_requested, __ATOMIC_RELAXED)) { rc = -2; break; }
        ssize_t r = read(fd, buf, MV_BUF);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { rc = -1; break; }
        if (r == 0) break;
        sha256_update(&sh, buf, (size_t)r);
        *done += (uint64_t)r;
        time_t now = time(NULL);
        if (now != *last) { *last = now; on_progress(step, total ? 1 + (int)((*done > total ? total : *done) * 97 / total) : 1); }
        plat_throttle();
    }
    close(fd);
    if (rc == 0) sha256_final(&sh, out);
    return rc;
}

/* <Abbild>.sha256 im Format von sha256sum ("<hex>  <Dateiname>") anlegen; 0 = angelegt, 1 = gibt es schon, -1 = Schreibfehler */
static int write_sidecar(const char *img, const uint8_t h[32]) {
    char sc[320], tmp[330], hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    snprintf(sc, sizeof(sc), "%s.sha256", img);
    snprintf(tmp, sizeof(tmp), "%s.tmp", sc);
    const char *nm = strrchr(img, '/');
    if (access(sc, F_OK) == 0) return 1;
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    int bad = fprintf(f, "%s  %s\n", hex, nm ? nm + 1 : img) < 0 || fflush(f) != 0 || (fsync(fileno(f)) != 0 && errno != EINVAL);
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, sc) != 0) { unlink(tmp); return -1; }
    return 0;
}

static void *gverify_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0}, bad = {0};
    int ok_n = 0, bad_n = 0, none_n = 0, have_n = 0, was_cancel = 0, done_n = 0;
    uint64_t total = 0, done = 0;
    char *buf = malloc(MV_BUF);
    for (int i = 0; i < j->nitems; i++) total += (j->sumgen ? !j->items[i].nex : j->items[i].nex != 0) ? j->items[i].size : 0;
    if (!buf) { sb_tr(&msg, "Kein Speicher"); goto fin; }
    set_flags(1, 0);
    pthread_mutex_lock(&lock);
    st.total_ptr = &total;   /* Menge und Tempo unter dem Balken */
    pthread_mutex_unlock(&lock);
    time_t last = 0;
    for (int i = 0; i < j->nitems && !was_cancel; i++) {
        const del_item_t *it = &j->items[i];
        char step[360];
        uint8_t want[32], got[32];
        snprintf(step, sizeof(step), T("%s: %s (%d von %d)"), j->sumgen ? T("Prüfsumme anlegen") : T("Spiel prüfen"), it->name, i + 1, j->nitems);
        on_progress(step, total ? 1 + (int)(done * 97 / total) : 1);
        if (j->sumgen) {
            if (it->nex) { have_n++; continue; }
            if (!it->size) { none_n++; continue; }   /* Ordner-Spiele haben keine einzelne Datei zum Hashen */
        } else if (!it->nex || read_sha_file(it->ex[0], want) != 0) { none_n++; continue; }
        const int rc = gv_hash(it, buf, got, &done, total, &last, step);
        if (rc == -2) { was_cancel = 1; break; }
        done_n++;
        if (j->sumgen) {
            if (rc == 0 && write_sidecar(it->path, got) == 0) ok_n++;
            else { bad_n++; sb_add(&bad, "%s%s", bad.len ? ", " : "", it->name); }
        } else if (rc != 0 || memcmp(want, got, 32) != 0) { bad_n++; sb_add(&bad, "%s%s", bad.len ? ", " : "", it->name); }
        else ok_n++;
    }
    if (was_cancel) {
        if (j->sumgen) sb_tr(&msg, "Abgebrochen – Prüfsumme angelegt: %d von %d.", ok_n, j->nitems);
        else sb_tr(&msg, "Abgebrochen – geprüft: %d von %d.", done_n, j->nitems);
    } else if (j->sumgen) {
        if (ok_n) sb_tr(&msg, "Prüfsumme angelegt: %d.", ok_n);
        if (have_n) sb_tr(&msg, " Schon vorhanden: %d.", have_n);
        if (none_n) sb_tr(&msg, " Kein Abbild (Ordner-Spiele haben keine Prüfsumme): %d.", none_n);
        if (bad_n) sb_tr(&msg, " Nicht angelegt: %d – %s.", bad_n, bad.p ? bad.p : "");
    } else {
        if (ok_n) sb_tr(&msg, "Prüfsumme stimmt: %d.", ok_n);
        if (bad_n) sb_tr(&msg, " Weicht ab oder nicht lesbar: %d – %s.", bad_n, bad.p ? bad.p : "");
        if (none_n) sb_tr(&msg, " Keine Prüfsummen-Datei (.sha256) gefunden: %d.", none_n);
    }
    pthread_mutex_lock(&lock);
    st.total_ptr = NULL;   /* total lebt nur in diesem Thread */
    pthread_mutex_unlock(&lock);
    set_flags(0, 0);
fin:
    sb_add(&res, "{\"ok\":%d,\"bad\":%d,\"none\":%d,\"have\":%d}", ok_n, bad_n, none_n, have_n);
    free(buf);
    free(bad.p);
    free(j->items);
    j->items = NULL;
    job_done(j, ok_n > 0 && !bad_n && !was_cancel, &msg, &res, 0);
    return NULL;
}

static void api_game_verify_ex(int c, const char *body, int sumgen) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    const char *keys0 = n > 0 ? kv_get(kv, n, "keys") : NULL;
    if (!keys0 || !keys0[0]) { respond_error(c, 400, "keys ist erforderlich"); return; }
    char keys[MAX_DELETE * 9 + 16];
    snprintf(keys, sizeof(keys), "%s", keys0);
    int nkeys = 1;
    for (const char *p = keys; *p; p++) nkeys += *p == ',';
    if (nkeys > MAX_DELETE) { respond_error(c, 400, "Höchstens 256 Spiele auf einmal"); return; }
    job_t *j = new_job(c, sumgen ? JOB_GSUM : JOB_GVERIFY);
    if (!j) return;
    j->sumgen = sumgen;
    j->items = calloc(MAX_DELETE, sizeof(*j->items));
    if (!j->items) { job_free(j); respond_error(c, 500, "Kein Speicher"); return; }
    int ambiguous = 0, badkey = 0;
    pthread_mutex_lock(&lib_lock);
    for (char *p = keys, *save = NULL, *tok; (tok = strtok_r(p, ",", &save)); p = NULL) {
        char *kend;
        uint32_t want = (uint32_t)strtoul(tok, &kend, 16);
        if (kend == tok || *kend) { badkey = 1; break; }
        int hits = 0;
        for (int i = 0; i < lib_n; i++)
            for (int t = 0; t < lib_snap[i].r.ntitles; t++) hits += path_key(lib_snap[i].r.titles[t].path) == want;
        if (hits > 1) { ambiguous = 1; break; }
        for (int i = 0; i < lib_n && j->nitems < MAX_DELETE; i++) {
            if (!lroot_name(&lib_snap[i])[0]) continue;
            for (int t = 0; t < lib_snap[i].r.ntitles && j->nitems < MAX_DELETE; t++) {
                const lib_title_t *tt = &lib_snap[i].r.titles[t];
                if (path_key(tt->path) != want) continue;
                del_item_t *it = &j->items[j->nitems++];
                snprintf(it->id, sizeof(it->id), "%s", tt->id);
                snprintf(it->name, sizeof(it->name), "%s", tt->name[0] ? tt->name : tt->id);
                snprintf(it->path, sizeof(it->path), "%s", tt->path);
                snprintf(it->root, sizeof(it->root), "%s", lib_snap[i].r.root);
                snprintf(it->device, sizeof(it->device), "%s", lroot_name(&lib_snap[i]));
                it->dev = lib_snap[i].dev;
                struct stat fs;
                if (tt->ext[0] && lstat(tt->path, &fs) == 0 && S_ISREG(fs.st_mode)) {   /* nur Abbilder haben eine Prüfsumme daneben */
                    it->size = (uint64_t)fs.st_size;
                    char sc[320], bare[300];
                    snprintf(sc, sizeof(sc), "%s.sha256", tt->path);
                    snprintf(bare, sizeof(bare), "%s", tt->path);
                    char *dot = strrchr(bare, '.');
                    if (dot && dot > strrchr(bare, '/')) *dot = 0;
                    struct stat xs;
                    if (stat(sc, &xs) == 0 && S_ISREG(xs.st_mode)) { snprintf(it->ex[0], sizeof(it->ex[0]), "%.299s", sc); it->nex = 1; }
                    else {
                        snprintf(sc, sizeof(sc), "%s.sha256", bare);
                        if (stat(sc, &xs) == 0 && S_ISREG(xs.st_mode)) { snprintf(it->ex[0], sizeof(it->ex[0]), "%.299s", sc); it->nex = 1; }
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&lib_lock);
    if (ambiguous || badkey || !j->nitems) {
        job_free(j);
        respond_error(c, ambiguous ? 409 : 404, ambiguous ? N_("Auswahl nicht eindeutig – Liste neu laden.") : N_("Spiele nicht gefunden – Liste neu laden"));
        return;
    }
    snprintf(j->dev.name, sizeof(j->dev.name), "%s", j->items[0].device);
    start_job(c, j, gverify_worker);
}

static void api_game_verify(int c, const char *body) { api_game_verify_ex(c, body, 0); }
static void api_game_checksum(int c, const char *body) { api_game_verify_ex(c, body, 1); }

/* ------------------------------------------------------------ Spiel-Einzelheiten
 * POST /api/game/info {"key":"1a2b3c4d"} - Name, Titel-ID, Version, Art, Pfad, Größe, die größten Einträge im Spielordner, zugehörige
 * Dateien und ob eine Prüfsummen-Datei daneben liegt (nur lesen). */
static void api_game_info(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    const char *k0 = n > 0 ? kv_get(kv, n, "key") : NULL;
    char *kend = NULL;
    uint32_t want = k0 ? (uint32_t)strtoul(k0, &kend, 16) : 0;
    if (!k0 || !*k0 || *kend) { respond_error(c, 400, "key ist erforderlich"); return; }
    lib_title_t t;
    char xtra[2048] = "";
    int hits = 0;
    pthread_mutex_lock(&lib_lock);
    for (int i = 0; i < lib_n; i++)
        for (int k = 0; k < lib_snap[i].r.ntitles; k++)
            if (path_key(lib_snap[i].r.titles[k].path) == want) {
                if (!hits) {
                    t = lib_snap[i].r.titles[k];
                    snprintf(xtra, sizeof(xtra), "%s", lib_snap[i].xtra && lib_snap[i].xtra[k] ? lib_snap[i].xtra[k] : "");
                }
                hits++;
            }
    pthread_mutex_unlock(&lib_lock);
    if (hits != 1) { respond_error(c, hits ? 409 : 404, hits ? N_("Auswahl nicht eindeutig – Liste neu laden.") : N_("Spiele nicht gefunden – Liste neu laden")); return; }
    struct stat sb;
    sbuf_t b = {0};
    sb_add(&b, "{\"name\":");
    sb_str(&b, t.name);
    sb_add(&b, ",\"id\":");
    sb_str(&b, t.id);
    sb_add(&b, ",\"version\":");
    sb_str(&b, t.version);
    sb_add(&b, ",\"kind\":");
    sb_str(&b, t.kind);
    sb_add(&b, ",\"ext\":");
    sb_str(&b, t.ext);
    sb_add(&b, ",\"path\":");
    sb_str(&b, t.path);
    const int is_dir = lstat(t.path, &sb) == 0 && S_ISDIR(sb.st_mode);
    uint64_t total = 0;
    int more = 0;
    sb_add(&b, ",\"entries\":[");
    if (is_dir) {   /* die größten Einträge der obersten Ebene */
        DIR *d = opendir(t.path);
        typedef struct { uint64_t size; char name[100]; int dir; } ent_t;
        ent_t *ents = calloc(300, sizeof(ent_t));
        int ne = 0;
        struct dirent *e;
        while (d && ents && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[400];
            struct stat es;
            if (snprintf(p, sizeof(p), "%s/%s", t.path, e->d_name) >= (int)sizeof(p) || lstat(p, &es) != 0) continue;
            if (ne >= 300) { more++; continue; }
            ents[ne].dir = S_ISDIR(es.st_mode);
            ents[ne].size = ents[ne].dir ? tree_bytes(p, 0) : (uint64_t)es.st_size;
            snprintf(ents[ne].name, sizeof(ents[ne].name), "%.99s", e->d_name);
            total += ents[ne].size;
            ne++;
        }
        if (d) closedir(d);
        if (ents) {
            /* nach Größe absteigend (size an den Anfang gestellt: qsort über ent_t) */
            for (int x = 0; x < ne; x++)
                for (int y = x + 1; y < ne; y++)
                    if (ents[y].size > ents[x].size) { ent_t tmp = ents[x]; ents[x] = ents[y]; ents[y] = tmp; }
            const int shown = ne < 12 ? ne : 12;
            for (int x = 0; x < shown; x++) {
                sb_add(&b, "%s{\"name\":", x ? "," : "");
                sb_str(&b, ents[x].name);
                sb_add(&b, ",\"size\":%llu,\"dir\":%s}", (unsigned long long)ents[x].size, ents[x].dir ? "true" : "false");
            }
            more += ne - shown;
            free(ents);
        }
    } else if (lstat(t.path, &sb) == 0) {
        total = (uint64_t)sb.st_size;
    }
    sb_add(&b, "],\"more\":%d,\"size\":%llu,\"extras\":[", more, (unsigned long long)total);
    int xf = 1;
    for (const char *a = xtra; *a;) {
        const char *e = strchr(a, '\n');
        char one[300];
        snprintf(one, sizeof(one), "%.*s", (int)(e ? (size_t)(e - a) : strlen(a)), a);
        sb_add(&b, "%s", xf ? "" : ",");
        xf = 0;
        sb_str(&b, one);
        if (!e) break;
        a = e + 1;
    }
    char sc[320];
    snprintf(sc, sizeof(sc), "%s.sha256", t.path);
    struct stat xs;
    int has_sum = stat(sc, &xs) == 0;
    if (!has_sum && t.ext[0]) {
        char bare[300];
        snprintf(bare, sizeof(bare), "%s", t.path);
        char *dot = strrchr(bare, '.');
        if (dot && dot > strrchr(bare, '/')) { *dot = 0; snprintf(sc, sizeof(sc), "%s.sha256", bare); has_sum = stat(sc, &xs) == 0; }
    }
    sb_add(&b, "],\"checksum\":%s}", has_sum ? "true" : "false");
    respond_json(c, 200, &b);
    free(b.p);
}

/* ------------------------------------------------------------ Teilkopien verwerfen
 * POST /api/partials/discard {"device":"da1"} - entfernt den Zwischenordner eines abgebrochenen Kopierens auf diesem Laufwerk. */
static void *partdel_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    sbuf_t msg = {0}, res = {0};
    char s1[24];
    const uint64_t bytes = tree_bytes(j->path, 0);
    on_progress(N_("Teilkopie verwerfen"), 10);
    const int rc = remove_tree(j->path, 0, NULL);
    (void)rmdir(j->path);
    if (rc == 0 || access(j->path, F_OK) != 0) sb_tr(&msg, "Teilkopie verworfen: %s wieder frei.", size_text(bytes, s1, sizeof(s1)));
    else sb_tr(&msg, "Teilkopie nicht ganz entfernt: %s", strerror(errno));
    sb_add(&res, "{\"freed\":%llu}", (unsigned long long)bytes);
    job_done(j, rc == 0 || access(j->path, F_OK) != 0, &msg, &res, 1);
    return NULL;
}

static void api_partials_discard(int c, const char *body) {
    kv_t kv[KV_MAX];
    int n = json_parse(body, kv, KV_MAX);
    const char *dev = n > 0 ? kv_get(kv, n, "device") : NULL;
    if (!dev || !dev[0]) { respond_error(c, 400, "device ist erforderlich"); return; }
    proot_t pr[24];
    pthread_mutex_lock(&lib_lock);
    const int np = partial_roots(pr, 24);
    pthread_mutex_unlock(&lib_lock);
    int idx = -1;
    for (int k = 0; k < np; k++) if (!strcmp(pr[k].dev, dev)) idx = k;
    char stage[400];
    struct stat ps;
    if (idx >= 0) snprintf(stage, sizeof(stage), "%.299s/%s", pr[idx].path, MOVE_DIR);
    if (idx < 0 || lstat(stage, &ps) != 0 || !S_ISDIR(ps.st_mode)) { respond_error(c, 404, N_("Keine Teilkopie auf diesem Laufwerk")); return; }
    job_t *j = new_job(c, JOB_PARTDEL);
    if (!j) return;
    snprintf(j->path, sizeof(j->path), "%s", stage);
    snprintf(j->dev.name, sizeof(j->dev.name), "%.15s", dev);
    start_job(c, j, partdel_worker);
}


static void handle(int c) {
    char *req = malloc(REQ_MAX + 1);
    size_t len = 0;
    char *body = NULL;
    long clen = 0;

    if (!req) return;
    /* eine Anfrage darf insgesamt höchstens 10 s dauern: ein Client, der tröpfchenweise sendet, blockiert sonst den
     * Server (und damit Fortschritt und "Abbrechen") beliebig lange */
    const time_t deadline = time(NULL) + 10;
    /* Header lesen */
    while (len < REQ_MAX) {
        if (time(NULL) > deadline) goto out;
        ssize_t n = recv(c, req + len, REQ_MAX - len, 0);
        if (n <= 0) goto out;
        len += n;
        req[len] = 0;
        if ((body = strstr(req, "\r\n\r\n"))) break;
    }
    if (!body) { respond_error(c, 400, "Anfrage zu groß"); goto out; }
    *body = 0;
    body += 4;

    char method[8], path[512], host[96] = "", tmp[48];
    if (sscanf(req, "%7s %511s", method, path) != 2) { respond_error(c, 400, "Ungültige Anfrage"); goto out; }
    i18n_set(request_lang(path, req));   /* Antworten und Aufträge dieser Verbindung in dieser Sprache */
    if (!header(req, "Host", host, sizeof(host)) || !host_ok(host)) {
        respond_error(c, 403, "Ungültiger Host. Bitte die PS5 über ihre IP-Adresse aufrufen.");
        goto out;
    }
    /* Spielstand-ZIP hochladen: Körper beliebig groß, wird beim Lesen gleich geschrieben */
    if (!strncmp(method, "POST", 5) && !strncmp(path, "/api/saves/restore/upload", 25) && (!path[25] || path[25] == '?')) {
        unsigned long long up = header(req, "Content-Length", tmp, sizeof(tmp)) ? strtoull(tmp, NULL, 10) : 0;
        const size_t have = len - (size_t)(body - req);
        if (!origin_ok(req, host)) {
            respond_error(c, 403, "Fremder Ursprung abgelehnt");
            drain_body(c, up > have ? up - have : 0);
        } else {
            api_saves_restore(c, body, have, up);
        }
        goto out;
    }
    /* Image hochladen: Körper beliebig groß, wird beim Lesen gleich geschrieben */
    if (!strncmp(method, "POST", 5) && !strncmp(path, "/api/restore/upload?", 20)) {
        unsigned long long up = header(req, "Content-Length", tmp, sizeof(tmp)) ? strtoull(tmp, NULL, 10) : 0;
        if (!origin_ok(req, host)) respond_error(c, 403, "Fremder Ursprung abgelehnt");
        else if (!header(req, "Content-Type", tmp, sizeof(tmp)) || strcasecmp(tmp, "application/octet-stream") != 0)
            respond_error(c, 415, "Content-Type muss application/octet-stream sein");
        else if (!up) respond_error(c, 411, "Content-Length fehlt");
        else api_upload(c, path + 20, body, len - (size_t)(body - req), up);
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
        if (time(NULL) > deadline) goto out;
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
    else if (strcmp(path, "/help") == 0 && is_get)   /* Anleitung in fünf Sprachen (Sprache: ?lang=xx) */
        respond(c, 200, "text/html; charset=utf-8", web_help_html, strlen(web_help_html));
    else if (strcmp(path, "/avatar.png") == 0 && is_get)
        respond_ex(c, 200, "image/png", web_avatar_png, (size_t)(web_avatar_png_end - web_avatar_png), "max-age=86400");
    else if (strcmp(path, "/bg.jpg") == 0 && is_get)
        respond_ex(c, 200, "image/jpeg", web_bg_jpg, (size_t)(web_bg_jpg_end - web_bg_jpg), "max-age=86400");
    else if (strcmp(path, "/page-dark.jpg") == 0 && is_get)
        respond_ex(c, 200, "image/jpeg", web_page_dark, (size_t)(web_page_dark_end - web_page_dark), "max-age=86400");
    else if (strcmp(path, "/page-light.jpg") == 0 && is_get)
        respond_ex(c, 200, "image/jpeg", web_page_light, (size_t)(web_page_light_end - web_page_light), "max-age=86400");
    else if (strcmp(path, "/api/devices") == 0 && is_get)
        api_devices(c);
    else if (strcmp(path, "/api/move") == 0 && is_post)
        api_move(c, body);
    else if (strcmp(path, "/api/pkg/delete") == 0 && is_post)
        api_pkg_delete(c, body);
    else if (strcmp(path, "/api/game/verify") == 0 && is_post)
        api_game_verify(c, body);
    else if (strcmp(path, "/api/game/checksum") == 0 && is_post)
        api_game_checksum(c, body);
    else if (strcmp(path, "/api/game/info") == 0 && is_post)
        api_game_info(c, body);
    else if (strcmp(path, "/api/partials/discard") == 0 && is_post)
        api_partials_discard(c, body);
    else if (strcmp(path, "/api/library/sizes") == 0 && is_get)
        api_library_sizes(c);
    else if (strcmp(path, "/api/library/sizes") == 0 && is_post)
        api_library_sizes_start(c, body);
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
    else if (strcmp(path, "/api/tile") == 0 && (is_get || is_post))
        api_tile(c, is_post);
    else if (strcmp(path, "/api/history") == 0 && is_get)
        api_history(c, 0);
    else if (strcmp(path, "/api/history/clear") == 0 && is_post)
        api_history(c, 1);
    else if (strcmp(path, "/api/delete") == 0 && is_post)
        api_delete(c, body);
    else if (strcmp(path, "/api/images") == 0 && is_get)
        api_images(c);
    else if (strcmp(path, "/api/images/verify") == 0 && is_post)
        api_images_verify(c, body);
    else if (strcmp(path, "/api/backup/resume") == 0 && is_post)
        api_backup_resume(c, body);
    else if (strcmp(path, "/api/images/discard") == 0 && is_post)
        api_images_discard(c, body);
    else if (strcmp(path, "/api/images/delete") == 0 && is_post)
        api_images_delete(c, body);
    else if (strcmp(path, "/api/backup") == 0 && is_post)
        api_backup(c, body);
    else if (strcmp(path, "/api/backup/download") == 0 && is_get)
        api_download(c, q);
    else if (strcmp(path, "/api/restore") == 0 && is_post)
        api_restore(c, body);
    else if (strcmp(path, "/api/saves") == 0 && is_get)
        api_saves(c);
    else if (strcmp(path, "/api/saves/users") == 0 && is_get)
        api_saves_users(c);
    else if (strcmp(path, "/api/saves/download") == 0 && is_get)
        api_saves_download(c, q);
    else if (strcmp(path, "/api/saves/locations") == 0 && is_get)
        api_saves_locations(c);
    else if (strcmp(path, "/api/saves/backups") == 0 && is_get)
        api_saves_backups(c);
    else if (strcmp(path, "/api/saves/backup") == 0 && is_post)
        api_saves_backup(c, body);
    else if (strcmp(path, "/api/saves/times") == 0 && is_post)
        api_saves_times(c, body);
    else if (strcmp(path, "/api/saves/restore") == 0 && is_post)
        api_saves_restore_paths(c, body);
    else if (strcmp(path, "/api/cancel") == 0 && is_post) {
        /* Warten auf Abziehen/Einstecken, Prüfen und Beschreiben brechen ab; das Formatieren selbst nicht */
        __atomic_store_n(&cancel_keep, body && strstr(body, "\"keep\"") && strstr(body, "true") ? 1 : 0, __ATOMIC_RELAXED);
        __atomic_store_n(&cancel_requested, 1, __ATOMIC_RELAXED);
        respond_ok(c, 200);
    }
    else if (strcmp(path, "/api/quit") == 0 && is_post) {
        pthread_mutex_lock(&lock);
        int busy = st.busy;
        if (!busy) quitting = 1;   /* unter der Sperre: kein neuer Auftrag kann mehr dazwischenkommen (start_job) */
        pthread_mutex_unlock(&lock);
        if (busy) respond_error(c, 409, "Es läuft noch ein Auftrag");
        else {
            respond_ok(c, 200);
            __atomic_store_n(&quit_requested, 1, __ATOMIC_RELAXED);
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
    if (lang_setting >= 0) i18n_set(lang_setting);   /* Benachrichtigungen der PS5 (main.c) in der gewählten Sprache */
    lib_refresh();
    return s;
}

/* Jede Verbindung in einem eigenen Thread: ein langsames Gerät im Netz (oder ein hängender Browser) hält dann nicht
 * alle anderen auf - Fortschritt und "Abbrechen" bleiben erreichbar. Höchstens MAX_CONN gleichzeitig. */
#define MAX_CONN   8
#define CONN_STACK (512 * 1024)   /* PS5-Standard wären nur 64 KiB; api_devices legt ~16 KiB Geräteliste an */

static volatile int active_conns;

static void *conn_thread(void *arg) {
    int c = (int)(intptr_t)arg;
    handle(c);
    close(c);
    __sync_fetch_and_sub(&active_conns, 1);
    return NULL;
}

void server_serve(int s) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, CONN_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    while (!__atomic_load_n(&quit_requested, __ATOMIC_RELAXED)) {
        /* mit Zeitlimit warten, damit "Beenden" (aus einem Verbindungs-Thread) sofort wirkt */
        struct pollfd pfd = {s, POLLIN, 0};
        if (poll(&pfd, 1, 500) <= 0) continue;
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            /* Nie wegen eines einzelnen Fehlers aufhören (z. B. ECONNABORTED, wenn ein Client abbricht): das beendete
             * sonst den Formatierer - auch mitten in einem laufenden Auftrag. Bei Mangel an Ressourcen kurz warten. */
            if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN) usleep(100 * 1000);
            continue;
        }
        struct timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (__sync_fetch_and_add(&active_conns, 1) >= MAX_CONN) {
            __sync_fetch_and_sub(&active_conns, 1);
            respond_error(c, 503, "Zu viele gleichzeitige Anfragen – bitte gleich noch einmal versuchen");
            close(c);
            continue;
        }
        pthread_t t;
        if (pthread_create(&t, &attr, conn_thread, (void *)(intptr_t)c) != 0) {
            handle(c);   /* kein Thread verfügbar: wie früher direkt bearbeiten */
            close(c);
            __sync_fetch_and_sub(&active_conns, 1);
        }
    }
    pthread_attr_destroy(&attr);
    for (int i = 0; i < 50 && __atomic_load_n(&active_conns, __ATOMIC_RELAXED) > 0; i++) usleep(100 * 1000);   /* laufende Antworten noch zu Ende senden */
    close(s);
}
