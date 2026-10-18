/* Gemeinsamer Teil der PC-Helfer (siehe helper.h): formatiert USB-Datenträger am PC mit demselben Code wie der
 * PS5-Payload (exFAT 64 KB / FAT32, MBR, ab 2 TiB GPT), misst das Tempo, prüft die Echtheit und benennt um. Für
 * "Am PC – frei" zusätzlich jede Zuordnungseinheit, MBR/GPT nach Wahl, mehrere Partitionen, HFS+ sowie NTFS,
 * FAT16 und ext2/3/4 mit den Mitteln des Systems.
 *
 *   list                                    USB-Datenträger als JSON (ohne Administratorrechte)
 *   format <disk> <size> <fs> <label> [verify] [full] [folders] [plain] [limit=B] [cluster=B] [scheme=auto|mbr|gpt]
 *   layout <disk> <size> [scheme=..] [plain] [verify|full] [limit=B] part=<fs>,<bytes|rest>,<cluster>,<typ>,<0|1>,<label> ...
 *   speed  <disk> <size> [plain]
 *   label  <disk> <size> <label>
 *   backup <disk> <size> <datei.img.zst>    Image sichern (dazu <datei>.sha256)
 *   restore <disk> <size> <datei.img.zst>   Image zurückschreiben und nachlesen
 *
 * <size> ist die Größe aus "list": Passt sie nicht mehr (Stick getauscht), wird nichts angefasst. fs: exfat, fat32,
 * ntfs, fat, hfsplus, ext2, ext3, ext4 (layout auch none). cluster: Zuordnungseinheit in Bytes (fehlt/0 =
 * Standard). plain: Meldungen ohne PS5-Bezug. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../devtest.h"
#include "../fsdetect.h"
#include "../i18n.h"
#include "../imgbak.h"
#include "../mkfs.h"
#include "../mkfs_int.h"
#include "../vollabel.h"
#include "helper.h"

FILE *out;
volatile int cancel_requested;
static const char *const TEMPLATE_DIRS[] = {"homebrew", "etaHEN/games", "pkg", NULL};

/* ------------------------------------------------------------------ Ausgabe */

/* Länge einer gültigen UTF-8-Folge ab s (1-4), 0 = ungültig */
static int utf8_len(const unsigned char *s) {
    if (s[0] < 0x80) return 1;
    int n = s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3 : s[0] >= 0xC2 && s[0] <= 0xDF ? 2 : 0;
    if (!n) return 0;
    for (int i = 1; i < n; i++)
        if ((s[i] & 0xC0) != 0x80) return 0;
    if (n == 3 && ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0))) return 0;   /* zu lang, Surrogat */
    if (n == 4 && ((s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90))) return 0;
    return n;
}

/* JSON-Zeichenkette; ungültiges UTF-8 (Herstellernamen billiger Sticks, FAT-Bezeichnungen in alter Codepage) wird
 * zu U+FFFD - sonst lehnte das Programm die ganze Liste ab */
void json_str(FILE *f, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    while (*p) {
        int n = utf8_len(p);
        if (!n) { fputs("\\ufffd", f); p++; continue; }
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fwrite(p, 1, (size_t)n, f);
        p += n;
    }
    fputc('"', f);
}

const char *size_text(uint64_t b, char *buf, size_t n) {
    static const char *const unit[] = {N_("Bytes"), N_("KB"), N_("MB"), N_("GB"), N_("TB")};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    snprintf(buf, n, i ? "%.1f %s" : "%.0f %s", v, T(unit[i]));
    char *dot = strchr(buf, '.');
    if (dot && i18n_get() != LANG_EN) *dot = ',';   /* Dezimalkomma außer im Englischen */
    return buf;
}

static const char *rating_text(double read_mbps) {
    const char *r = dt_rating(read_mbps);
    return !strcmp(r, "schnell") ? T("schnell genug für Spiele") : !strcmp(r, "ausreichend")
           ? T("für Spiele ausreichend") : T("langsam – Spiele laden länger");
}

/* Text als ein Feld des Zeilenprotokolls ausgeben: Tab, Zeilenumbruch und andere Steuerzeichen (etwa aus der Ausgabe
 * von FormatEx oder mkntfs) würden die Zeile sonst zerlegen - das Programm fände dann kein Ergebnis */
static void field(const char *s) {
    for (; *s; s++) fputc((unsigned char)*s < 0x20 || *s == 0x7f ? ' ' : *s, out);
}

/* Zeile "P<TAB>prozent[<TAB>Modell]..." auf stderr: Beim Lesen der Laufwerke laufen schlafende Festplatten erst an
 * (Sekunden je Platte) - so sieht das Programm, dass es weitergeht und worauf es wartet */
void list_progress(int pct, const char *const *pending, int np) {
    fprintf(stderr, "P\t%d", pct);
    for (int i = 0; i < np; i++) {
        fputc('\t', stderr);
        for (const char *p = pending[i]; *p; p++) fputc(*p == '\t' || *p == '\n' || *p == '\r' ? ' ' : *p, stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

/* step ist der deutsche Schrittname (oder schon übersetzt) - ausgegeben wird er in der gewählten Sprache */
void on_progress(const char *step, int pct) {
    static char last_step[96];
    step = T(step);
    static int last_pct = -1;
    if (pct == last_pct && !strcmp(step, last_step)) return;
    last_pct = pct;
    snprintf(last_step, sizeof(last_step), "%s", step);
    fprintf(out, "P\t%d\t", pct);
    field(step);
    fputc('\n', out);
    fflush(out);
}

void finish(int ok, const char *msg, const char *json) {
    fprintf(out, "R\t%d\t", ok);
    field(T(msg));
    fprintf(out, "\t%s\n", json && *json ? json : "{}");   /* das JSON ist schon maskiert (json_str) */
    fflush(out);
}

static const char *title(const session_t *s) { return s->info.model[0] ? s->info.model : T("USB-Datenträger"); }

/* ------------------------------------------------------------------ Aufträge */

typedef struct {
    int      fs;          /* FS_EXFAT, FS_FAT32, FS_NTFS, FS_EXT4, FS_HFSPLUS, WFS_FAT16, WFS_EXT2, WFS_EXT3, WFS_NONE */
    uint64_t bytes;       /* 0 = Rest */
    uint32_t cluster;     /* 0 = Standard */
    int      ptype, active;
    char     label[40];
} wpart_t;

typedef struct {
    int      verify, full, folders, plain;
    int      scheme;      /* LAYOUT_* */
    uint64_t limit;
    int      n;
    wpart_t  p[MAX_PARTS];
} fmtreq_t;

static int is_ext(int fs) { return fs == FS_EXT4 || fs == WFS_EXT2 || fs == WFS_EXT3; }
/* NTFS und FAT16 schreibt das System (Windows: FormatEx, Linux: mkntfs/mkfs.fat) */
static int by_os(int fs) { return fs == FS_NTFS || fs == WFS_FAT16; }
static const char *ext_name(int fs) { return fs == WFS_EXT2 ? "ext2" : fs == WFS_EXT3 ? "ext3" : "ext4"; }
static const char *fs_title(int fs) {
    return fs == FS_EXFAT ? "exFAT" : fs == FS_FAT32 ? "FAT32" : fs == FS_NTFS ? "NTFS" : fs == WFS_FAT16 ? "FAT"
         : fs == FS_HFSPLUS ? "HFS+" : is_ext(fs) ? ext_name(fs) : T("ohne Dateisystem");
}

/* Bezeichnung und Zuordnungseinheit passend zum Dateisystem? (exFAT/FAT32/HFS+ prüft mkfs selbst genauer) */
static const char *check_part(const wpart_t *p) {
    /* ohne Dateisystem wird die Bezeichnung zum GPT-Partitionsnamen: dieselben Regeln wie NTFS (bis 32 Zeichen) -
     * sonst könnten Tab/Zeilenumbruch das Ausgabeprotokoll zerlegen */
    int lfs = p->fs == FS_NTFS || p->fs == WFS_NONE ? FS_NTFS : is_ext(p->fs) ? FS_EXT4
            : p->fs == FS_HFSPLUS ? FS_HFSPLUS : FS_FAT32;
    if (!label_valid(p->label, lfs)) return N_("Ungültige Bezeichnung.");
    uint32_t c = p->cluster;
    if (!c) return NULL;
    if (c & (c - 1)) return N_("Ungültige Zuordnungseinheit.");
    if (is_ext(p->fs) && (c < 1024 || c > 4096)) return N_("Blockgröße für ext: 1, 2 oder 4 KB.");
    if (by_os(p->fs) && (c < 512 || c > (p->fs == FS_NTFS ? 2 * MIB : 64 * 1024))) return N_("Ungültige Zuordnungseinheit.");
    return NULL;
}

/* Lage aller Partitionen vorab berechnen und prüfen, ob jedes Dateisystem an seine Stelle passt - sonst würde erst der
 * Datenträger neu aufgeteilt (alles gelöscht) und dann scheiterte das Formatieren. 0 = passt. */
static int plan_check(const session_t *s, uint64_t size, const fmtreq_t *q, char *err, size_t n) {
    blkdev_t pd = {0};
    pd.ss = s->info.ss;
    pd.sectors = size / s->info.ss;
    part_t pp[MAX_PARTS];
    memset(pp, 0, sizeof(pp));
    for (int i = 0; i < q->n; i++) pp[i].bytes = q->p[i].bytes;
    if (layout_plan(&pd, q->scheme, q->limit, pp, q->n, NULL)) {
        snprintf(err, n, "%s", mkfs_error());
        return -1;
    }
    for (int i = 0; i < q->n; i++) {
        const wpart_t *w = &q->p[i];
        const uint64_t bytes = pp[i].count * pd.ss;
        char lim[24];
        /* FAT32/FAT16: Beginn und Größe in 32-Bit-Feldern des Bootsektors */
        if ((w->fs == FS_FAT32 || w->fs == WFS_FAT16) && (pp[i].start > 0xFFFFFFFFull || pp[i].count > 0xFFFFFFFFull)) {
            snprintf(err, n, T("Partition %d: %s geht nur in den ersten %s des Datenträgers – bitte weiter nach vorn "
                               "legen oder exFAT wählen."), i + 1, fs_title(w->fs),
                     size_text(0xFFFFFFFFull * pd.ss, lim, sizeof(lim)));
            return -1;
        }
        /* NTFS: mkntfs scheitert unter etwa 16 Clustern bzw. 2 MB (bei 1 Cluster läuft es endlos) */
        if (w->fs == FS_NTFS && (bytes < 2 * MIB || (w->cluster && bytes / w->cluster < 16))) {
            snprintf(err, n, T("Partition %d: Für NTFS zu klein bzw. Zuordnungseinheit zu groß (mindestens 16 "
                               "Einheiten und 2 MB)."), i + 1);
            return -1;
        }
    }
    return 0;
}

/* Baustein der Ergebnismeldung anhängen (fmt: deutscher Text, ausgegeben in der gewählten Sprache) */
static int add_text(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, T(fmt), ap);
    va_end(ap);
    return r < 0 ? 0 : r;
}

/* Fortschritt eines Einzelschritts (mkfs_*) in den Bereich prog_lo..prog_hi einpassen */
static int prog_lo, prog_hi;
static const char *prog_name;
static void on_scaled(const char *step, int pct) {
    on_progress(prog_name ? prog_name : step, prog_lo + pct * (prog_hi - prog_lo) / 100);
}

static int cmd_format(const char *disk, uint64_t size, fmtreq_t *q) {
    char err[512] = "", msg[1536], json[512], s1[24], s2[24];
    static char summary[MAX_PARTS][160];
    for (int i = 0; i < q->n; i++) {
        const char *e = check_part(&q->p[i]);
        if (e) {
            if (q->n > 1) snprintf(msg, sizeof(msg), T("Partition %d: %s"), i + 1, T(e));
            else snprintf(msg, sizeof(msg), "%s", T(e));
            finish(0, msg, NULL);
            return 1;
        }
        if (by_os(q->p[i].fs) && plat_image_mode()) {
            finish(0, "NTFS und FAT16 sind im Abbild-Test nicht möglich.", NULL);
            return 1;
        }
    }
    if (q->limit && (q->limit < 64 * MIB || q->limit > size)) {
        finish(0, "Ungültige Grenze für den echten Bereich.", NULL);
        return 1;
    }
    int actives = 0;   /* MBR: höchstens eine startfähige Partition, sonst starten manche Rechner gar nicht */
    for (int i = 0; i < q->n; i++) actives += q->p[i].active != 0;
    if (actives > 1) {
        finish(0, "Nur eine Partition darf startfähig sein.", NULL);
        return 1;
    }
    /* eine exFAT-/FAT32-Partition: genau der Weg der PS5 (auch die Ordnervorlage) */
    const int classic = q->n == 1 && q->p[0].ptype == PT_AUTO && !q->p[0].active && !q->p[0].bytes &&
                        (q->p[0].fs == FS_EXFAT || q->p[0].fs == FS_FAT32);

    session_t s;
    on_progress("Datenträger vorbereiten", 0);
    if (plat_session_open(&s, disk, size, 1, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    const uint64_t use = q->limit ? q->limit : size;
    if (!classic && plan_check(&s, size, q, err, sizeof(err))) {   /* vor Prüfung und Aufteilen: noch nichts geändert */
        plat_session_close(&s, 0);
        snprintf(msg, sizeof(msg), T("Formatieren fehlgeschlagen: %s"), err);
        finish(0, msg, NULL);
        return 1;
    }

    devtest_t t;
    memset(&t, 0, sizeof(t));
    int tested = 0, test_cancelled = 0;
    if (q->verify || q->full) {
        tested = q->full ? 2 : 1;
        int rc = q->full ? dt_full(s.tfd, s.info.ss, use, on_progress, &cancel_requested, &t, err, sizeof(err))
                         : dt_verify(s.tfd, s.info.ss, use, on_progress, &cancel_requested, &t, err, sizeof(err));
        if (rc == 0 && t.cancelled && !dt_wrote()) {   /* noch nichts überschrieben: gar nichts anfassen */
            plat_session_close(&s, 0);
            finish(0, T("Abgebrochen – der Datenträger wurde nicht verändert."), NULL);
            return 1;
        }
        if (rc == 0 && t.cancelled) {
            test_cancelled = 1;
        } else if (rc != 0 || t.fake || t.bad) {
            if (dt_wrote()) dt_scrub(s.tfd, size);   /* kein altes Dateisystem mit teils überschriebenen Dateien */
            plat_session_close(&s, 1);
            snprintf(json, sizeof(json), "{\"test\":\"%s\",\"fake\":%s,\"bad\":%s,\"reported\":%llu,\"real\":%llu,"
                     "\"bad_at\":%llu}", tested == 2 ? "full" : "quick", t.fake ? "true" : "false",
                     t.bad ? "true" : "false", (unsigned long long)size, (unsigned long long)t.real_bytes,
                     (unsigned long long)t.bad_at);
            if (rc != 0)
                snprintf(msg, sizeof(msg), T("Prüfung fehlgeschlagen: %s Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben."), err);
            else if (t.fake)
                snprintf(msg, sizeof(msg), T("%s meldet %s, hat aber nur etwa %s echten Speicher. Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben – "
                         "%s, höchstens den echten Bereich nutzen."), title(&s), size_text(size, s1, sizeof(s1)),
                         size_text(t.real_bytes, s2, sizeof(s2)),
                         q->plain ? T("für wichtige Daten nicht empfohlen") : T("für Spiele an der PS5 nicht empfohlen"));
            else
                snprintf(msg, sizeof(msg), T("Fehlerhafte Bereiche ab %llu MB: %s arbeitet nicht zuverlässig. "
                         "Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben."), (unsigned long long)(t.bad_at / MIB), title(&s));
            finish(0, msg, json);
            return 1;
        }
    }

    blkdev_t d = {0};
    d.fd = s.fd;
    d.ss = s.info.ss;
    d.sectors = size / s.info.ss;
    d.progress = on_progress;
    mkfs_opts_t o = {0};
    o.limit = q->limit;
    o.scheme = q->scheme;
    o.dirs = q->folders && classic ? TEMPLATE_DIRS : NULL;
    part_t parts[MAX_PARTS];
    uint32_t serial[MAX_PARTS];
    int rc = 0, gpt = 0, later = 0;    /* later: Partitionen, die nach dem Freigeben das System/mke2fs schreibt */
    memset(parts, 0, sizeof(parts));
    for (int i = 0; i < q->n; i++) { serial[i] = arc4random(); summary[i][0] = 0; }

    if (classic) {
        rc = format_disk(&d, q->p[0].fs, q->p[0].label, serial[0], q->p[0].cluster, &o);
        if (!rc) snprintf(summary[0], sizeof(summary[0]), "%s", mkfs_summary());
        gpt = o.is_gpt;
    } else {
        for (int i = 0; i < q->n; i++) {
            const wpart_t *w = &q->p[i];
            parts[i].fs = w->fs == WFS_FAT16 ? FS_EXFAT : is_ext(w->fs) ? FS_EXT4 : w->fs == WFS_NONE ? 0 : w->fs;
            /* ohne Dateisystem: Typ Linux, sonst zeigte Windows eine leere Partition mit "Formatieren?"-Frage */
            parts[i].ptype = w->ptype != PT_AUTO ? w->ptype : w->fs == WFS_NONE ? PT_LINUX : PT_AUTO;
            parts[i].mbr_type = w->fs == WFS_FAT16 && w->ptype == PT_AUTO ? 0x0E : 0;
            parts[i].active = w->active;
            parts[i].bytes = w->bytes;
            parts[i].name = w->label;
            later += by_os(w->fs) || is_ext(w->fs);
        }
        rc = layout_disk(&d, q->scheme, q->limit, parts, q->n, serial[0], &gpt);
        /* eigene Dateisysteme jetzt (unter Windows bei NTFS/FAT ein vorläufiges exFAT, an dem es das Volume
         * wiedererkennt), Fortschritt 10 .. 40 bzw. 95 % */
        const int span_hi = later ? 40 : 95;
        for (int i = 0; i < q->n && !rc; i++) {
            const wpart_t *w = &q->p[i];
            if (is_ext(w->fs) || w->fs == WFS_NONE) continue;
#ifndef _WIN32
            if (by_os(w->fs)) continue;   /* Linux schreibt NTFS/FAT direkt in die (leere) Partition */
#endif
            prog_lo = 10 + (span_hi - 10) * i / q->n;
            prog_hi = 10 + (span_hi - 10) * (i + 1) / q->n;
            prog_name = by_os(w->fs) ? N_("Partition anlegen") : NULL;
            d.progress = on_scaled;
            d.base = parts[i].start * d.ss;
            mkfs_progress_reset();
            if (by_os(w->fs)) rc = mkfs_exfat(&d, parts[i].start, parts[i].count, "", serial[i], 0, NULL);
            else if (w->fs == FS_HFSPLUS) rc = mkfs_hfsplus(&d, parts[i].count, w->label, serial[i], w->cluster);
            else if (w->fs == FS_EXFAT) rc = mkfs_exfat(&d, parts[i].start, parts[i].count, w->label, serial[i], w->cluster, NULL);
            else rc = mkfs_fat32(&d, parts[i].start, parts[i].count, w->label, serial[i], w->cluster, NULL);
            d.base = 0;
            if (!rc && !by_os(w->fs)) snprintf(summary[i], sizeof(summary[i]), "%s", mkfs_summary());
            if (rc && q->n > 1) {
                char e2[300];
                snprintf(e2, sizeof(e2), "%s", mkfs_error());
                mk_fail("Partition %d: %s", i + 1, e2);
            }
        }
        mkfs_progress_reset();
        prog_name = NULL;
    }
    plat_session_close(&s, 1);
    if (rc != 0) {
        snprintf(msg, sizeof(msg), T("Formatieren fehlgeschlagen: %s"), mkfs_error());
        finish(0, msg, NULL);
        return 1;
    }

    /* das System (NTFS/FAT) und mke2fs (ext) schreiben erst jetzt - der Datenträger ist wieder freigegeben */
    int done_later = 0, failed = 0;
    char failmsg[700] = "";
    for (int i = 0; i < q->n && !classic; i++) {
        const wpart_t *w = &q->p[i];
        if (!by_os(w->fs) && !is_ext(w->fs)) continue;
        const int lo = 40 + 55 * done_later / later, hi = 40 + 55 * (done_later + 1) / later;
        done_later++;
        int r = by_os(w->fs)
              ? plat_os_format(&s.info, w->fs, i, parts[i].start, parts[i].count, w->label, w->cluster, serial[i],
                               lo, hi, summary[i], sizeof(summary[i]), err, sizeof(err))
              : plat_mke2fs(&s.info, i, parts[i].start, parts[i].count, ext_name(w->fs), w->cluster, w->label, lo, hi,
                            summary[i], sizeof(summary[i]), err, sizeof(err));
        if (r) {
            failed++;
            size_t fl = strlen(failmsg);
            if (q->n > 1) snprintf(failmsg + fl, sizeof(failmsg) - fl, T("%sPartition %d (%s): %s"), fl ? " " : "", i + 1,
                                   fs_title(w->fs), err);
            else snprintf(failmsg + fl, sizeof(failmsg) - fl, T("%s konnte nicht angelegt werden: %s"), fs_title(w->fs), err);
        }
    }
    if (failed) {
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "%s%s", failmsg, q->n == 1 && by_os(q->p[0].fs)
                 ? T(" Der Datenträger ist jetzt leer und vorläufig mit exFAT formatiert.") : "");
#else
        snprintf(msg, sizeof(msg), "%s", failmsg);
#endif
        finish(0, msg, NULL);
        return 1;
    }
    on_progress("Fertig", 100);

    int l;
#define ADD(...) do { if (l < (int)sizeof(msg)) l += add_text(msg + l, sizeof(msg) - l, __VA_ARGS__); } while (0)
    if (q->n == 1) {
        l = snprintf(msg, sizeof(msg), T("%s erfolgreich formatiert: %s"), title(&s),
                     q->p[0].fs == WFS_NONE ? T("Partition ohne Dateisystem") : summary[0]);
        if (q->p[0].label[0]) ADD(", Bezeichnung %s", q->p[0].label);
        if (q->plain) ADD(", Partitionstabelle %s", gpt ? "GPT" : "MBR");
        else if (gpt) ADD(", Partitionstabelle GPT");
    } else {
        l = snprintf(msg, sizeof(msg), T("%s: %d Partitionen angelegt (%s):"), title(&s), q->n, gpt ? "GPT" : "MBR");
        for (int i = 0; i < q->n; i++) {
            ADD(" %d) %s, %s", i + 1, q->p[i].fs == WFS_NONE ? T("ohne Dateisystem") : summary[i],
                size_text(parts[i].count * s.info.ss, s1, sizeof(s1)));
            if (q->p[i].label[0]) ADD(", „%s“", q->p[i].label);
            if (i + 1 < q->n) ADD(";");
        }
    }
    if (q->limit) ADD(", nur die echten %s genutzt", size_text(q->limit, s1, sizeof(s1)));
    if (o.dirs) ADD(", Ordner homebrew, etaHEN/games und pkg angelegt");
    ADD(".");
#ifdef _WIN32
    int has_ext = 0, has_hfs = 0, first_ext = 0;
    for (int i = q->n - 1; i >= 0; i--) {
        if (is_ext(q->p[i].fs)) { has_ext = 1; first_ext = q->p[i].fs; }
        has_hfs |= q->p[i].fs == FS_HFSPLUS;
    }
    if (has_ext || has_hfs)   /* nur, was wirklich angelegt wurde */
        ADD(" Windows selbst kann %s nicht öffnen – dafür erscheint kein Laufwerksbuchstabe.",
            has_ext && has_hfs ? T("ext und HFS+") : has_hfs ? "HFS+" : q->n > 1 ? "ext" : fs_title(first_ext));
#endif
    if (tested && !test_cancelled && t.read_mbps > 0) {
        ADD(" %s: echt%s, lesen %.0f MB/s, schreiben %.0f MB/s", tested == 2 ? T("Jede Stelle geprüft") : T("Prüfung"),
            tested == 2 ? T(", keine Fehler") : "", t.read_mbps, t.write_mbps);
        if (q->plain) ADD(".");
        else ADD(" – %s.", rating_text(t.read_mbps));
    }
    if (test_cancelled) ADD(" Die Prüfung wurde abgebrochen – formatiert wurde trotzdem.");
    if (!q->plain) ADD(" Jetzt einfach abziehen und an die PS5 stecken.");
#undef ADD
    snprintf(json, sizeof(json), "{\"scheme\":\"%s\",\"read\":%.1f,\"write\":%.1f,\"mbr_type\":%d}",
             gpt ? "GPT" : "MBR", t.read_mbps, t.write_mbps, plat_read_mbr_type(&s.info));
    finish(1, msg, json);
    return 0;
}

static int cmd_speed(const char *disk, uint64_t size, int plain) {
    char err[512] = "", msg[512], json[128];
    session_t s;
    on_progress("Lesetempo messen", 0);
    if (plat_session_open(&s, disk, size, 0, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    devtest_t t;
    int rc = dt_read_speed(s.tfd, s.info.ss, size, on_progress, &cancel_requested, &t, err, sizeof(err));
    plat_session_close(&s, 0);
    if (rc != 0) { snprintf(msg, sizeof(msg), T("Tempo messen fehlgeschlagen: %s"), err); finish(0, msg, NULL); return 1; }
    if (t.cancelled) { finish(0, "Abgebrochen.", NULL); return 1; }
    if (plain) snprintf(msg, sizeof(msg), T("Lesetempo von %s: %.0f MB/s."), title(&s), t.read_mbps);
    else snprintf(msg, sizeof(msg), T("Lesetempo von %s: %.0f MB/s – %s."), title(&s), t.read_mbps, rating_text(t.read_mbps));
    snprintf(json, sizeof(json), "{\"read\":%.1f,\"rating\":\"%s\"}", t.read_mbps, dt_rating(t.read_mbps));
    finish(1, msg, json);
    return 0;
}

/* NTFS/FAT16 benennt das System um, exFAT/FAT32 der gemeinsame Code (vollabel) */
static int cmd_label(const char *disk, uint64_t size, const char *label) {
    char err[512] = "", msg[512];
    session_t s;
    memset(&s, 0, sizeof(s));
    on_progress("Bezeichnung schreiben", 30);
    int handled = 0, rc = plat_image_mode() ? 0 : plat_os_label(disk, size, label, &s.info, &handled, err, sizeof(err));
    if (!handled && !rc) {
        if (plat_session_open(&s, disk, size, 1, err, sizeof(err))) { finish(0, err, NULL); return 1; }
        rc = vol_set_label(s.fd, s.info.ss, s.info.size, label, err, sizeof(err));
        plat_session_close(&s, 1);
    }
    if (rc != 0) { snprintf(msg, sizeof(msg), T("Umbenennen fehlgeschlagen: %s"), err); finish(0, msg, NULL); return 1; }
    if (label[0]) snprintf(msg, sizeof(msg), T("Neue Bezeichnung für %s: „%s“."), title(&s), label);
    else snprintf(msg, sizeof(msg), T("Bezeichnung von %s entfernt."), title(&s));
    finish(1, msg, NULL);
    return 0;
}

/* ------------------------------------------------------------------ Image-Sicherung */

static int out_errno;
static int out_file(void *ctx, const void *p, size_t n) {
    if (fwrite(p, 1, n, ctx) == n) return 0;
    out_errno = errno;
    return -1;
}

static void write_error(int e, char *err, size_t n) {
    if (e == ENOSPC) snprintf(err, n, "%s", T("Am Ziel ist nicht mehr genug Platz frei."));
    else if (e == EFBIG) snprintf(err, n, "%s", T("Die Datei ist zu groß für das Dateisystem des Ziels (FAT32: höchstens 4 GB)."));
    else snprintf(err, n, T("Schreiben der Sicherung fehlgeschlagen: %s"), strerror(e ? e : EIO));
}
static long in_file(void *ctx, void *p, size_t n) {
    size_t r = fread(p, 1, n, ctx);
    return r ? (long)r : ferror((FILE *)ctx) ? -1 : 0;
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* Datenträger in die Datei path sichern (erst "<path>.part", fertig umbenannt), dazu "<path>.sha256" */
static int cmd_backup(const char *disk, uint64_t size, const char *path) {
    char err[512] = "", msg[768], json[1024], tmp[1100], side[1100], t1[32], t2[32];
    session_t s;
    size_t pl = strlen(path);
    /* nur Namen auf .img.zst: der Helfer schreibt mit Administratorrechten - nie eine Datei anderer Art */
    if (pl + 8 > sizeof(tmp) || pl < 9 || strcmp(path + pl - 8, ".img.zst")) { finish(0, "Ungültiger Dateiname.", NULL); return 2; }
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    snprintf(side, sizeof(side), "%s.sha256", path);
    on_progress("Belegte Bereiche ermitteln", 0);
    /* gesperrt wie beim Formatieren: Was das System gerade schreibt, käme sonst halb ins Image */
    if (plat_session_open(&s, disk, size, 1, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    ib_info_t info;
    memset(&info, 0, sizeof(info));
    snprintf(info.model, sizeof(info.model), "%s", s.info.model);
    snprintf(info.ident, sizeof(info.ident), "%s", s.info.serial);
    fsinfo_t fi;
    if (!fs_probe(s.fd, s.info.ss, size, &fi)) {
        snprintf(info.fs, sizeof(info.fs), "%s", fi.fs);
        snprintf(info.label, sizeof(info.label), "%s", fi.label);
    }
    FILE *f = plat_fopen(tmp, 1, err, sizeof(err));
    if (!f) { plat_session_close(&s, 0); finish(0, err, NULL); return 1; }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    ib_result_t r;
    out_errno = 0;
    int rc = ib_backup(s.fd, s.info.ss, size, &info, out_file, f, on_progress, &cancel_requested, &r, err, sizeof(err));
    plat_session_close(&s, 1);   /* Partitionen neu melden: Linux-Desktops hängen sie dann wieder ein (wie nach Umbenennen) */
    if (fflush(f) && !out_errno) out_errno = errno ? errno : EIO;
    if (fclose(f) && !out_errno) out_errno = errno ? errno : EIO;
    if (out_errno && !r.cancelled) {   /* auch nach einem Fehler von ib_backup: Ursache war das Schreiben */
        rc = -1;
        write_error(out_errno, err, sizeof(err));
    }
    if (rc || r.cancelled) {
        plat_remove(tmp);
        if (r.cancelled && !rc) { finish(0, "Abgebrochen.", NULL); return 1; }
        snprintf(msg, sizeof(msg), T("Sichern fehlgeschlagen: %s"), err);
        finish(0, msg, NULL);
        return 1;
    }
    FILE *h = plat_fopen(side, 1, err, sizeof(err));
    int hok = h && fprintf(h, "%s  %s\n", r.sha_file, base_name(path)) > 0;
    if (h && fclose(h)) hok = 0;
    if (!hok || plat_rename(tmp, path)) {
        plat_remove(tmp);
        plat_remove(side);
        snprintf(msg, sizeof(msg), T("Sichern fehlgeschlagen: %s"), err[0] ? err : T("Datei nicht anlegbar"));
        finish(0, msg, NULL);
        return 1;
    }
    snprintf(msg, sizeof(msg), T("%s gesichert: %s belegt, Datei %s."), title(&s), size_text(r.used, t1, sizeof(t1)),
             size_text(r.file_bytes, t2, sizeof(t2)));
    snprintf(json, sizeof(json), "{\"size\":%llu,\"used\":%llu,\"file_bytes\":%llu,\"sha256\":\"%s\",\"sha256_data\":\"%s\"}",
             (unsigned long long)size, (unsigned long long)r.used, (unsigned long long)r.file_bytes, r.sha_file, r.sha_img);
    finish(1, msg, json);
    return 0;
}

/* Image aus path auf den Datenträger schreiben; vorher passt es? danach Nachlesen */
static int cmd_restore(const char *disk, uint64_t size, const char *path) {
    char err[512] = "", msg[768], json[256], t1[32];
    on_progress("Image prüfen", 0);
    FILE *f = plat_fopen(path, 0, err, sizeof(err));
    if (!f) { finish(0, err, NULL); return 1; }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    ib_info_t info;
    ib_reader_t *rd = ib_open(in_file, f, &info, err, sizeof(err));
    if (!rd) { fclose(f); finish(0, err, NULL); return 1; }
    if (info.size > size) {
        ib_close(rd);
        fclose(f);
        char a[32], b[32];
        snprintf(msg, sizeof(msg), T("Das Image (%s) ist größer als der Datenträger (%s)."), size_text(info.size, a, sizeof(a)),
                 size_text(size, b, sizeof(b)));
        finish(0, msg, NULL);
        return 1;
    }
    session_t s;
    if (plat_session_open(&s, disk, size, 1, err, sizeof(err))) { ib_close(rd); fclose(f); finish(0, err, NULL); return 1; }
    ib_result_t r;
    int rc = ib_restore(rd, s.fd, s.tfd, s.info.ss, size, on_progress, &cancel_requested, &r, err, sizeof(err));
    ib_close(rd);
    fclose(f);
    plat_session_close(&s, 1);   /* neue Partitionstabelle einlesen lassen */
    if (!rc && r.cancelled) { finish(0, "Abgebrochen – der Datenträger ist nur teilweise beschrieben.", NULL); return 1; }
    if (rc) { snprintf(msg, sizeof(msg), T("Wiederherstellen fehlgeschlagen: %s"), err); finish(0, msg, NULL); return 1; }
    snprintf(msg, sizeof(msg), T("Image auf %s wiederhergestellt und geprüft (%s geschrieben)."), title(&s),
             size_text(r.used, t1, sizeof(t1)));
    snprintf(json, sizeof(json), "{\"size\":%llu,\"written\":%llu,\"sha256_data\":\"%s\"}", (unsigned long long)r.size,
             (unsigned long long)r.used, r.sha_img);
    finish(1, msg, json);
    return 0;
}

/* ------------------------------------------------------------------ Befehlszeile */

static int parse_fs(const char *s) {
    static const struct { const char *name; int fs; } map[] = {
        {"exfat", FS_EXFAT}, {"fat32", FS_FAT32}, {"ntfs", FS_NTFS}, {"fat", WFS_FAT16}, {"hfsplus", FS_HFSPLUS},
        {"ext2", WFS_EXT2}, {"ext3", WFS_EXT3}, {"ext4", FS_EXT4}, {"none", WFS_NONE},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(s, map[i].name)) return map[i].fs;
    return 0;
}

static int parse_type(const char *s) {
    static const char *const names[] = {"auto", "windows", "linux", "swap", "efi", "apple"};   /* = PT_* */
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

/* "part=" <fs>,<bytes|rest>,<cluster>,<typ>,<aktiv 0|1>,<bezeichnung> - die Bezeichnung zuletzt (darf Leerzeichen
 * enthalten, nie ein Komma: nur A-Z, 0-9, Leerzeichen, - und _) */
static int parse_part(const char *spec, wpart_t *p) {
    char buf[128], *f[6] = {0};
    if (strlen(spec) >= sizeof(buf)) return -1;
    strcpy(buf, spec);
    char *s = buf;
    for (int i = 0; i < 6; i++) {
        f[i] = s;
        char *c = i < 5 ? strchr(s, ',') : NULL;
        if (i < 5) { if (!c) return -1; *c = 0; s = c + 1; }
    }
    char *end;
    p->fs = parse_fs(f[0]);
    if (!p->fs) return -1;
    if (!strcmp(f[1], "rest")) p->bytes = 0;
    else {
        p->bytes = strtoull(f[1], &end, 10);
        if (*end || !p->bytes) return -1;
    }
    unsigned long long c = strtoull(f[2], &end, 10);
    if (*end || c > 32 * MIB) return -1;
    p->cluster = (uint32_t)c;
    if ((p->ptype = parse_type(f[3])) < 0) return -1;
    if (strcmp(f[4], "0") && strcmp(f[4], "1")) return -1;
    p->active = f[4][0] == '1';
    if (strlen(f[5]) >= sizeof(p->label)) return -1;
    strcpy(p->label, f[5]);
    return 0;
}

void serial_clean(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) *s = '_';
}

int helper_command(int argc, char **argv) {
    const char *cmd = argv[0];
    int rest = argc - 1;
    char **a = argv + 1;
    if (!strcmp(cmd, "list")) return plat_list();
    /* serial=...: Seriennummer laut Liste - unter den Optionen erlaubt, hier herausgenommen. Erst hinter den festen
       Angaben suchen: eine Bezeichnung "serial=..." bliebe sonst nicht die Bezeichnung */
    static char want_serial[64];
    want_serial[0] = 0;
    int first_opt = !strcmp(cmd, "format") ? 4 : !strcmp(cmd, "label") || !strcmp(cmd, "backup") || !strcmp(cmd, "restore") ? 3 : 2;
    for (int k = first_opt; k < rest; k++)
        if (!strncmp(a[k], "serial=", 7)) {
            snprintf(want_serial, sizeof(want_serial), "%s", a[k] + 7);
            memmove(&a[k], &a[k + 1], (size_t)(rest - k - 1) * sizeof(*a));
            rest--;
            break;
        }
    if (rest < 2) { finish(0, "Aufruf unvollständig.", NULL); return 2; }
    const char *disk = a[0];
    uint64_t size = strtoull(a[1], NULL, 10);
    /* gleiche Nummer und Größe, aber andere (oder jetzt gar keine) Seriennummer: ein baugleicher Datenträger wurde
       getauscht - nichts tun. Nur wenn sich das Gerät nicht befragen lässt, entscheidet allein die Größe. */
    char now_serial[64];
    if (want_serial[0] && plat_disk_serial(disk, now_serial, sizeof(now_serial)) == 0 &&
        strcmp(want_serial, now_serial) != 0) {
        finish(0, "Unter dieser Nummer steckt jetzt ein anderer Datenträger – bitte die Liste aktualisieren.", NULL);
        return 1;
    }
    if (!strcmp(cmd, "speed")) return cmd_speed(disk, size, rest >= 3 && !strcmp(a[2], "plain"));
    if (!strcmp(cmd, "label") && rest >= 3) return cmd_label(disk, size, a[2]);
    if (!strcmp(cmd, "backup") && rest == 3) return cmd_backup(disk, size, a[2]);
    if (!strcmp(cmd, "restore") && rest == 3) return cmd_restore(disk, size, a[2]);
    if ((!strcmp(cmd, "format") && rest >= 4) || !strcmp(cmd, "layout")) {
        static fmtreq_t q;
        memset(&q, 0, sizeof(q));
        int k = 2;
        if (!strcmp(cmd, "format")) {   /* eine Partition: format <disk> <size> <fs> <label> [...] */
            q.n = 1;
            q.p[0].fs = parse_fs(a[2]);
            if (!q.p[0].fs || q.p[0].fs == WFS_NONE) { finish(0, "Unbekanntes Dateisystem.", NULL); return 2; }
            if (strlen(a[3]) >= sizeof(q.p[0].label)) { finish(0, "Ungültige Bezeichnung.", NULL); return 2; }
            strcpy(q.p[0].label, a[3]);
            k = 4;
        }
        for (; k < rest; k++) {
            if (!strcmp(a[k], "verify")) q.verify = 1;
            else if (!strcmp(a[k], "full")) q.full = 1;
            else if (!strcmp(a[k], "folders")) q.folders = 1;
            else if (!strcmp(a[k], "plain")) q.plain = 1;
            else if (!strncmp(a[k], "limit=", 6)) q.limit = strtoull(a[k] + 6, NULL, 10);
            else if (!strncmp(a[k], "cluster=", 8) && !strcmp(cmd, "format")) {
                unsigned long long c = strtoull(a[k] + 8, NULL, 10);
                if (c > 32 * MIB) { finish(0, "Ungültige Zuordnungseinheit.", NULL); return 2; }
                q.p[0].cluster = (uint32_t)c;
            } else if (!strcmp(a[k], "scheme=auto")) q.scheme = LAYOUT_AUTO;
            else if (!strcmp(a[k], "scheme=mbr")) q.scheme = LAYOUT_MBR;
            else if (!strcmp(a[k], "scheme=gpt")) q.scheme = LAYOUT_GPT;
            else if (!strncmp(a[k], "part=", 5) && !strcmp(cmd, "layout")) {
                if (q.n >= MAX_PARTS) { finish(0, "Zu viele Partitionen.", NULL); return 2; }
                if (parse_part(a[k] + 5, &q.p[q.n])) { finish(0, "Ungültige Partitionsangabe.", NULL); return 2; }
                q.n++;
            } else { finish(0, "Unbekannte Angabe beim Formatieren.", NULL); return 2; }   /* nichts still übergehen */
        }
        if (!q.n) { finish(0, "Keine Partition angegeben.", NULL); return 2; }
        return cmd_format(disk, size, &q);
    }
    finish(0, "Unbekannter Auftrag.", NULL);
    return 2;
}
