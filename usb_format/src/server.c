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
        snprintf(part, sizeof(part), T("noch etwa %s"), eta_text((long)(el * (100 - pct) / dp), eta, sizeof(eta)));
        snprintf(st.detail + at, sizeof(st.detail) - at, "%s%s", at ? " · " : "", part);
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
       JOB_SAVE_RESTORE, JOB_SAVE_BACKUP };
static const char *const JOB_KIND[] = {"", "format", "speed", "check", "label", "delete", "backup", "restore",
                                       "tile", "verify", "save_restore", "save_backup"};

/* Spiel zum Löschen (aus der Bibliothek übernommen) */
typedef struct {
    char id[16], name[128], path[256], root[96], device[16];
    uint64_t size;   /* Abbild: Größe laut Suche */
    dev_t    dev;    /* st_dev der Wurzel laut Suche: noch derselbe eingehängte Datenträger? */
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
    /* Image sichern (path: Zielordner) bzw. wiederherstellen (path: Image-Datei, schon geöffnet in img/rd) */
    char     path[512];
    FILE    *img;
    ib_reader_t *rd;
    char    *saves_list; /* Spielstände: Schlüssel bzw. Pfade, getrennt durch Zeilenumbruch */
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
    if (native) {
        rc = format_disk(&d, j->fs, j->label, arc4random(), j->cluster, &o);
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
    int        usb;
    lib_root_t r;
    /* welcher Datenträger dort eingehängt war: steckt am selben Anschluss ein anderer (oder wurde neu eingehängt),
       gilt die Liste als veraltet */
    dev_t      dev;          /* st_dev der Wurzel - jede Einhängung bekommt eine neue */
    uint64_t   size;
    char       ident[64];
} libroot_t;

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

static void lib_free_all(libroot_t *r, int n) {
    for (int i = 0; i < n; i++) lib_free(&r[i].r);
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
                n++;
            }
        }
        for (int k = 0; roots && extra && extra[k] && extra[k + 1] && n < cap; k += 2) {
            roots[n].usb = !strcmp(extra[k + 1], "usb");
            roots[n].dev = root_dev(extra[k]);
            lib_progress(-1, total, extra[k]);
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
                    lib_free(&r->r);   /* veraltet: Eintrag entfernen */
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
static void api_library(int c, int refresh) {
    if (refresh) lib_refresh();
    sbuf_t b = {0};
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

static void *backup_worker(void *arg) {
    job_t *j = arg;
    i18n_set(j->lang);
    char err[256] = "", title[160], name[160], file[700], part[720], side[720], s1[24], s2[24];
    sbuf_t msg = {0}, res = {0};
    ib_info_t info;
    img_info_of(&j->dev, &info);
    ib_file_name(&info, name, sizeof(name));
    if (mkdir(j->path, 0777) != 0 && errno != EEXIST) {
        sb_tr(&msg, "Der Ordner %s lässt sich nicht anlegen: %s", j->path, strerror(errno));
        job_done(j, 0, &msg, &res, 0);
        return NULL;
    }
    /* derselbe Name in derselben Minute schon da: Nummer anhängen */
    snprintf(file, sizeof(file), "%s/%s", j->path, name);
    for (int k = 2; k < 100 && access(file, F_OK) == 0; k++)
        snprintf(file, sizeof(file), "%s/%.*s_%d%s", j->path, (int)(strlen(name) - strlen(IMG_EXT)), name, k, IMG_EXT);
    snprintf(name, sizeof(name), "%s", strrchr(file, '/') + 1);
    snprintf(part, sizeof(part), "%s.part", file);
    snprintf(side, sizeof(side), "%s.sha256", file);

    int fd = job_acquire(j, err, sizeof(err));   /* eingebunden: kurz abmelden, sonst sicherte man halb Geschriebenes */
    if (fd < 0) { sb_add(&msg, "%s", err); job_done(j, 0, &msg, &res, 0); return NULL; }
    int ofd = open(part, O_WRONLY | O_CREAT | O_EXCL, 0666);
    fout_t o = {ofd >= 0 ? fdopen(ofd, "wb") : NULL, 0};
    if (!o.f) {
        sb_tr(&msg, "Die Datei %s lässt sich nicht anlegen: %s", part, strerror(errno));
        if (ofd >= 0) close(ofd);
        job_release(fd);
        job_done(j, 0, &msg, &res, 1);
        return NULL;
    }
    setvbuf(o.f, NULL, _IOFBF, 1 << 20);
    set_flags(1, 0);
    st.total_ptr = &info.used;   /* ib_backup setzt info.used nach der Belegungssuche */
    ib_result_t r;
    int rc = ib_backup(fd, j->dev.ss, j->dev.size, &info, file_out, &o, on_progress, &cancel_requested, &r, err,
                       sizeof(err));
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
        unlink(part);
        if (h) unlink(side);   /* auch, wenn erst das Umbenennen scheiterte: keine .sha256 ohne Sicherung */
        if (rc) sb_tr(&msg, "Sichern fehlgeschlagen: %s", err);
        else sb_tr(&msg, "Abgebrochen.");
        job_done(j, 0, &msg, &res, 1);
        return NULL;
    }
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
    int ok_n = 0, fail_n = 0, tiles = 0;

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
             * USB-Laufwerks, auf dem die Suche es fand (api_delete übernimmt nur Spiele von USB-Laufwerken) */
            if (!path_inside(it->path, it->root)) {
                rc = -1;
                snprintf(err, sizeof(err), "%s", T("Pfad liegt nicht auf dem USB-Laufwerk"));
            } else {
                on_progress(del_step, base > 0 ? base : 1);
                rm_prog_t rp = {tree_weight(it->path, 0), 0, base, span, -1};
                rc = remove_tree(it->path, 0, &rp);
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
        if (rc == 0) { ok_n++; sb_add(&done_names, "%s%s", done_names.len ? ", " : "", it->name); }
        else { fail_n++; sb_add(&errs, "%s%s: %s", errs.len ? "; " : "", it->name, err); }
    }
    sb_add(&res, "{\"deleted\":%d,\"failed\":%d,\"tiles\":%d,\"failed_items\":[%s]}", ok_n, fail_n, tiles,
           failed.p ? failed.p : "");
    free(failed.p);
    free(smp);

    if (ok_n == 1) sb_tr(&msg, "1 Spiel gelöscht: %s.", done_names.p);
    else if (ok_n) sb_tr(&msg, "%d Spiele gelöscht: %s.", ok_n, done_names.p);
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
        sb_add(&b, ",\"size\":%llu}", (unsigned long long)saves[i].size);
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
    for (char *t = strtok(s, "\n"); t; t = strtok(NULL, "\n")) {
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
            sb_add(&b, ",\"size\":%llu,\"time\":%lld}", (unsigned long long)bk[k].size, (long long)bk[k].mtime);
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
    if (l < 5 || strcmp(path + l - 4, ".zip") != 0 || strstr(path, "/../") || strrchr(path, '/')[1] == '.') return 0;
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

    job_t *j = new_job(c, JOB_DELETE);
    if (!j) return;
    j->uninstall = uninstall;
    j->items = calloc(MAX_DELETE, sizeof(*j->items));
    if (!j->items) { free(j); respond_error(c, 500, "Kein Speicher"); return; }
    int ambiguous = 0, badkey = 0;
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
            if (!lib_snap[i].usb || !lib_snap[i].device[0]) continue;
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
                snprintf(it->device, sizeof(it->device), "%s", lib_snap[i].device);
                it->size = tt->size;
                it->dev = lib_snap[i].dev;
            }
        }
    }
    pthread_mutex_unlock(&lib_lock);
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
