/*
 * Laufwerksprüfungen: Lesetempo, Schreib-/Lesetempo mit Echtheitsprüfung, vollständiges Überschreiben.
 *
 * Echtheit (gefälschte Größe): Billige Sticks melden z. B. 64 GB, haben aber nur 8 GB Flash. Was hinter der echten
 * Grenze geschrieben wird, landet entweder wieder vorne (die Adresse "läuft über") oder verschwindet. Geprüft wird mit
 * 4-KiB-Stichproben im festen Raster c + i*G (G eine Zweierpotenz): Läuft die Adresse an einer Grenze über, die ein
 * Vielfaches von G ist, trifft jede Stichprobe dahinter genau eine frühere. Jede Stichprobe trägt ihre Position; findet
 * sich an Position p die Kennung von q > p, ist die echte Größe q - p. Fehlen die Daten hinter einer Grenze, zählt die
 * erste fehlerhafte Position, sofern ab dort alles fehlt (sonst sind es einzelne defekte Bereiche).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "devtest.h"
#include "i18n.h"

#define MIB         (1024u * 1024u)
#define CHUNK       (4u * MIB)
#define SAMPLE      4096u
/* Billige Sticks brauchen für jede verstreute 4-KiB-Schreibstelle Sekunden (ganzer Löschblock wird neu geschrieben,
 * gemessen am VendorCo-Stick: rund 3 s). Gefälschte Größen sind Zweierpotenzen im GB-Bereich - dafür reicht ein grobes
 * Raster; 128 Stichproben halten den Schnelltest bei wenigen Minuten. */
#define MAX_SAMPLES 128u
#define EXTRA_SAMPLES 40u   /* Zweierpotenzen ab 64 MiB (reicht bis weit über 2^64 hinaus) */
#define SPEED_BYTES (256ull * MIB)

static const char MAGIC[16] = "USBFMT-PROBE-v1";

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

#ifdef USBFMT_HOST
/* Nur Host-Tests: gefälschten Stick nachbilden. USBFMT_FAKE_REAL=<Bytes>, USBFMT_FAKE_MODE=wrap (Standard) | drop |
 * eio (hinter der Grenze Ein-/Ausgabefehler) | dead (nimmt nichts an, liest nur Nullen; FAKE_REAL beliebig) */
static uint64_t sim_real;
static int sim_drop, sim_eio, sim_dead, sim_ready;

static int sim_slow;   /* USBFMT_SLOW=1: je Zugriff 20 ms warten (Abbrechen testen) */
static uint64_t sim_gone_at;   /* USBFMT_GONE_AT=<Byte>: ab dem ersten Zugriff dort ist der Stick "abgezogen" */
static int sim_gone;

static void sim_load(void) {
    if (sim_ready) return;
    const char *r = getenv("USBFMT_FAKE_REAL"), *m = getenv("USBFMT_FAKE_MODE"), *g = getenv("USBFMT_GONE_AT");
    sim_real = r ? strtoull(r, NULL, 10) : 0;
    sim_drop = m && strcmp(m, "drop") == 0;
    sim_eio = m && strcmp(m, "eio") == 0;
    sim_dead = m && strcmp(m, "dead") == 0;
    sim_slow = getenv("USBFMT_SLOW") != NULL;
    sim_gone_at = g ? strtoull(g, NULL, 10) : 0;
    sim_ready = 1;
}
#endif

static int io_wrote;   /* seit Prüfbeginn etwas geschrieben (Abbrechen davor: Daten noch unverändert) */

int dt_wrote(void) { return io_wrote; }

static ssize_t io_rw(int fd, int wr, void *buf, size_t len, uint64_t off) {
    if (wr) io_wrote = 1;   /* schon der Versuch zählt: ein halber Schreibvorgang verändert den Stick auch */
#ifdef USBFMT_HOST
    sim_load();
    if (sim_slow) usleep(20000);
    if (sim_gone || (sim_gone_at && off + len > sim_gone_at)) { sim_gone = 1; errno = ENXIO; return -1; }
    if (sim_dead) {   /* Schreiben verpufft, Lesen liefert Nullen (er antwortet also noch - nicht "abgezogen") */
        if (!wr) memset(buf, 0, len);
        return (ssize_t)len;
    }
    if (sim_eio && sim_real && off + len > sim_real) { errno = EIO; return -1; }
    if (sim_real) {
        for (size_t done = 0; done < len; ) {
            uint64_t o = off + done, phys = o % sim_real;
            size_t take = len - done;
            if (take > sim_real - phys) take = (size_t)(sim_real - phys);
            if (sim_drop && o >= sim_real) {
                if (!wr) memset((uint8_t *)buf + done, 0, take);
            } else if ((wr ? pwrite(fd, (uint8_t *)buf + done, take, (off_t)phys)
                           : pread(fd, (uint8_t *)buf + done, take, (off_t)phys)) != (ssize_t)take) {
                return -1;
            }
            done += take;
        }
        return (ssize_t)len;
    }
#endif
    return wr ? pwrite(fd, buf, len, (off_t)off) : pread(fd, buf, len, (off_t)off);
}

static int io_failed;   /* seit Prüfbeginn ein Lese-/Schreibfehler (nicht: falsche Daten) - nur ein Auftrag zugleich */

static int io_all(int fd, int wr, void *buf, size_t len, uint64_t off) {
    for (size_t done = 0; done < len; ) {
        ssize_t n = io_rw(fd, wr, (uint8_t *)buf + done, len - done, off + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { io_failed = 1; return -1; }
        done += (size_t)n;
    }
    return 0;
}

/* Nach Lese-/Schreibfehlern: Ist der Stick noch da? Gelesen wird am Anfang (16 MiB), der bei jedem Stick echt ist.
 * Antwortet er dort nicht mehr, wurde er abgezogen oder die USB-Verbindung ist weg - dann ist die Prüfung ungültig
 * (sonst würde ein echter Stick als "gefälscht ab hier" gespeichert). */
static int dev_gone(int fd, uint32_t ss) {
    void *p = NULL;
    size_t len = ss > SAMPLE ? ss : SAMPLE;
    if (posix_memalign(&p, 4096, len)) return 0;
    int gone = io_rw(fd, 0, p, len, 16ull * MIB) != (ssize_t)len;
    free(p);
    return gone;
}

static int gone_error(int fd, uint32_t ss, uint64_t at, char *err, size_t errlen) {
    if (!io_failed || !dev_gone(fd, ss)) return 0;
    snprintf(err, errlen, T("Laufwerk nicht mehr erreichbar (bei %llu MB) – abgezogen oder USB-Verbindung "
                            "unterbrochen? Die Prüfung ist ungültig."), (unsigned long long)(at / MIB));
    return 1;
}

static void dev_sync(int fd) { (void)fsync(fd); }   /* Rohgeräte melden ggf. EINVAL - dann ist nichts gepuffert */

/* xorshift64*: schnell, reproduzierbar aus dem Startwert */
static void fill(uint8_t *b, size_t len, uint64_t seed) {
    uint64_t s = seed | 1;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        uint64_t v = s * 2685821657736338717ull;
        memcpy(b + i, &v, 8);
    }
}

/* Block mit Kennung: MAGIC, nonce, eigene Position, Rest Zufallsdaten aus nonce ^ Position */
static void tagged(uint8_t *b, size_t len, uint64_t nonce, uint64_t pos) {
    for (size_t o = 0; o < len; o += SAMPLE) {
        uint64_t p = pos + o;
        memcpy(b + o, MAGIC, 16);
        memcpy(b + o + 16, &nonce, 8);
        memcpy(b + o + 24, &p, 8);
        fill(b + o + 32, SAMPLE - 32, nonce ^ p);
    }
}

/* Überlauf-Abstände sind Vielfache der echten Größe (k-mal übergelaufen); ihr ggT ist die echte Größe */
static uint64_t gcd64(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

/* Kennung eines gelesenen Blocks: Position, die darin steht, oder 0, wenn es kein Block dieses Laufs ist */
static uint64_t tag_pos(const uint8_t *b, uint64_t nonce) {
    uint64_t p;
    if (memcmp(b, MAGIC, 16) != 0 || memcmp(b + 16, &nonce, 8) != 0) return 0;
    memcpy(&p, b + 24, 8);
    return p;
}

static uint64_t nonce64(void) { return (uint64_t)arc4random() << 32 | arc4random(); }

static uint8_t *alloc_buf(size_t len, char *err, size_t errlen) {
    void *p = NULL;
    if (posix_memalign(&p, 4096, len)) { snprintf(err, errlen, "%s", T("Kein Speicher")); return NULL; }
    return p;
}

const char *dt_rating(double mbps) {
    return mbps >= 100 ? "schnell" : mbps >= 40 ? "ausreichend" : "langsam";
}

/* Ein durchgehender Balken über alle Schritte eines Auftrags: jeder Schritt bekommt einen festen Abschnitt (lo..hi) und meldet
 * darin 0-100 %. Der Balken läuft nie rückwärts und steht nach dem Start nie auf 0 (sonst zeigen die Oberflächen einen
 * Lauf-Balken ohne Wert). */
static int ph_lo, ph_hi = 100, ph_last;

static void phase(int lo, int hi) { ph_lo = lo; ph_hi = hi; }

static void phase_reset(void) { ph_lo = 0; ph_hi = 100; ph_last = 0; }

static void rep(progress_fn progress, const char *step, int pct) {
    int all = ph_lo + pct * (ph_hi - ph_lo) / 100;
    if (all < 1) all = 1;
    if (all > 99) all = 99;
    if (all < ph_last) all = ph_last;
    ph_last = all;
    progress(step, all);
}

/* Fortschritt einer Messung mit Mengen- und Zeitgrenze: der größere Anteil, höchstens 99 bis zum Ende */
static int time_pct(uint64_t done, uint64_t total, double elapsed, double limit) {
    double a = total ? (double)done / (double)total : 1, b = elapsed / limit, p = a > b ? a : b;
    return p >= 1 ? 99 : (int)(p * 100);
}

int dt_read_speed(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
                  char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    phase_reset();
    uint8_t *buf = alloc_buf(CHUNK, err, errlen);
    if (!buf) return -1;
    uint64_t start = size / 4 / MIB * MIB, total = SPEED_BYTES;
    if (start + total > size) total = (size - start) / ss * ss;
    uint64_t done = 0;
    double t0 = now_s();
    while (done < total) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; break; }
        size_t take = total - done < CHUNK ? (size_t)(total - done) : CHUNK;
        if (io_all(fd, 0, buf, take, start + done)) {
            snprintf(err, errlen, T("Lesefehler bei %llu MB: %s"), (unsigned long long)((start + done) / MIB),
                     strerror(errno));
            r->bad = 1;
            r->bad_at = start + done;
            free(buf);
            return -1;
        }
        done += take;
        /* Ende nach Menge oder nach 8 s - was zuerst kommt; so läuft der Balken gleichmäßig bis zum Schluss */
        rep(progress, T("Lesetempo messen"), time_pct(done, total, now_s() - t0, 8));
        if (now_s() - t0 > 8 && done >= 16ull * MIB) break;
    }
    double dt = now_s() - t0;
    r->read_mbps = dt > 0 ? (double)done / dt / 1e6 : 0;
    free(buf);
    return 0;
}

/* Schreib-/Lesetempo: sequentiell ab 16 MiB, bis 256 MiB bzw. 15 Sekunden; das Gelesene wird verglichen */
static int speed_rw(int fd, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r, uint8_t *a,
                    uint8_t *b, char *err, size_t errlen) {
    const uint64_t start = 16ull * MIB, nonce = nonce64();
    uint64_t total = size / 4 / MIB * MIB, done = 0;
    if (total > SPEED_BYTES) total = SPEED_BYTES;
    if (total < CHUNK || start + total > size) return 0;   /* zu klein zum Messen */
    double t0 = now_s();
    phase(0, 12);
    while (done < total) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; return 0; }
        fill(a, CHUNK, nonce ^ (start + done));
        if (io_all(fd, 1, a, CHUNK, start + done)) {
            snprintf(err, errlen, T("Schreibfehler bei %llu MB: %s"), (unsigned long long)((start + done) / MIB),
                     strerror(errno));
            return -1;
        }
        done += CHUNK;
        rep(progress, T("Schreibtempo messen"), time_pct(done, total, now_s() - t0, 15));
        if (now_s() - t0 > 15 && done >= 16ull * MIB) break;
    }
    dev_sync(fd);
    double dt = now_s() - t0;
    r->write_mbps = dt > 0 ? (double)done / dt / 1e6 : 0;

    uint64_t written = done;
    done = 0;
    t0 = now_s();
    phase(12, 20);
    while (done < written) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; return 0; }
        if (io_all(fd, 0, b, CHUNK, start + done)) {
            snprintf(err, errlen, T("Lesefehler bei %llu MB: %s"), (unsigned long long)((start + done) / MIB),
                     strerror(errno));
            return -1;
        }
        fill(a, CHUNK, nonce ^ (start + done));
        if (memcmp(a, b, CHUNK) != 0 && !r->bad) { r->bad = 1; r->bad_at = start + done; }
        done += CHUNK;
        rep(progress, T("Lesetempo messen"), (int)(done * 100 / written));
    }
    dt = now_s() - t0;
    r->read_mbps = dt > 0 ? (double)written / dt / 1e6 : 0;
    return 0;
}

int dt_verify(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
              char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    phase_reset();
    io_failed = 0;
    io_wrote = 0;
    uint8_t *a = alloc_buf(CHUNK, err, errlen), *b = a ? alloc_buf(CHUNK, err, errlen) : NULL;
    uint64_t *pos = NULL;
    uint8_t *state = NULL;   /* je Stichprobe: 0 offen, 1 gut, 2 fehlerhaft */
    int rc = -1;
    if (!a || !b) goto out;

    if (speed_rw(fd, size, progress, cancel, r, a, b, err, errlen)) goto out;
    if (r->cancelled) { rc = 0; goto out; }

    /* Raster: G = 16 MiB, verdoppelt bis höchstens MAX_SAMPLES Stichproben; dazu eine am Ende und je eine knapp
     * unter jeder Zweierpotenz ab 64 MiB. Ohne diese landen bei einem überlaufenden Stick, dessen echte Größe
     * kleiner als G ist, alle Rasterproben an derselben Stelle - der ggT wäre dann G statt der echten Größe. */
    uint64_t G = 16ull * MIB;
    while (size / G > MAX_SAMPLES) G *= 2;
    const uint64_t c = G / 2, nonce = nonce64();
    uint32_t n = 0;
    pos = malloc((MAX_SAMPLES + 2 + EXTRA_SAMPLES) * sizeof(*pos));
    state = calloc(MAX_SAMPLES + 2 + EXTRA_SAMPLES, 1);
    if (!pos || !state) { snprintf(err, errlen, "%s", T("Kein Speicher")); goto out; }
    for (uint64_t p = c; p + SAMPLE <= size && n < MAX_SAMPLES; p += G) pos[n++] = p;
    uint64_t end = (size - SAMPLE) / SAMPLE * SAMPLE;
    if (!n || end > pos[n - 1]) pos[n++] = end;
    for (uint64_t p2 = 64ull * MIB; p2 < size && n < MAX_SAMPLES + 2 + EXTRA_SAMPLES; p2 *= 2) pos[n++] = p2 - SAMPLE;
    /* aufsteigend schreiben: eine spätere Stichprobe darf nur eine niedrigere überschreiben (siehe Auswertung) */
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t k = i; k > 0 && pos[k - 1] > pos[k]; k--) { uint64_t t = pos[k]; pos[k] = pos[k - 1]; pos[k - 1] = t; }
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) if (!m || pos[i] != pos[m - 1]) pos[m++] = pos[i];
    n = m;

    double t0 = now_s();
    (void)t0;
    phase(20, 60);   /* Stichproben schreiben, danach lesen: ein durchgehender Balken */
    for (uint32_t i = 0; i < n; i++) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
        tagged(a, SAMPLE, nonce, pos[i]);
        if (io_all(fd, 1, a, SAMPLE, pos[i])) state[i] = 3;
        rep(progress, T("Echtheit prüfen"), (int)((uint64_t)(i + 1) * 100 / n));
    }
    dev_sync(fd);
    phase(60, 95);
    if (gone_error(fd, ss, pos[n - 1], err, errlen)) goto out;

    /* state: 1 gut, 2 überschrieben (eine spätere Stichprobe landete hier), 3 verloren (Fehler, Nullen, Müll) */
    uint64_t alias = 0, first_lost = 0, last_good = 0;
    uint32_t nlost = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
        if (state[i] != 3 && io_all(fd, 0, b, SAMPLE, pos[i]) == 0) {
            tagged(a, SAMPLE, nonce, pos[i]);
            uint64_t other = tag_pos(b, nonce);
            if (memcmp(a, b, SAMPLE) == 0) state[i] = 1;
            else if (other > pos[i]) {
                state[i] = 2;
                alias = gcd64(alias, other - pos[i]);
            } else state[i] = 3;
        } else {
            state[i] = 3;
        }
        if (state[i] == 3 && !nlost++) first_lost = pos[i];
        if (state[i] == 1 && !nlost) last_good = pos[i] + SAMPLE;
        if (i % 16 == 0) rep(progress, T("Echtheit prüfen: lesen"), (int)((uint64_t)i * 100 / n));
    }
    if (gone_error(fd, ss, first_lost, err, errlen)) goto out;

    /* Verloren ab first_lost (fast) alles: Grenze zwischen der letzten guten und der ersten verlorenen Stichprobe
     * durch Halbieren auf 1 MiB genau suchen (sicher nutzbar ist nur, was eine Probe bestätigt hat). Sonst sind es
     * einzelne defekte Bereiche. */
    uint64_t lost_limit = 0;
    if (nlost) {
        uint32_t after = 0, lost_after = 0;
        for (uint32_t i = 0; i < n; i++)
            if (pos[i] >= first_lost) { after++; lost_after += state[i] == 3; }
        if (lost_after * 10 >= after * 9 && first_lost > 0 && last_good > 0) {
            uint64_t lo = last_good / MIB * MIB, hi = first_lost;   /* lo: bis hier bestätigt, hi: verloren */
            for (uint64_t step = 1; hi - lo > MIB; step++) {
                if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
                uint64_t mid = (lo + (hi - lo) / 2) / MIB * MIB;
                if (mid <= lo) mid = lo + MIB;
                if (mid >= hi) break;
                phase(95, 99);
                rep(progress, T("Echtheit prüfen: Grenze suchen"), 50);
                tagged(a, SAMPLE, nonce ^ step, mid);
                int good = io_all(fd, 1, a, SAMPLE, mid) == 0;
                dev_sync(fd);
                good = good && io_all(fd, 0, b, SAMPLE, mid) == 0 && memcmp(a, b, SAMPLE) == 0;
                if (good) lo = mid; else hi = mid;
            }
            if (gone_error(fd, ss, hi, err, errlen)) goto out;   /* Grenze nur, wenn der Stick noch antwortet */
            lost_limit = lo;
        } else if (lost_after * 10 >= after * 9 && first_lost > 0) {
            r->bad = 1;   /* schon die erste Stichprobe verloren: kein nutzbarer Bereich erkennbar */
            r->bad_at = first_lost;
        } else {
            r->bad = 1;
            if (!r->bad_at || first_lost < r->bad_at) r->bad_at = first_lost;
        }
    }
    if (alias || lost_limit) {
        uint64_t real = alias && (!lost_limit || alias < lost_limit) ? alias : lost_limit;
        r->fake = 1;
        r->real_bytes = real / MIB * MIB;
        if (r->bad && r->bad_at >= r->real_bytes) r->bad = 0;   /* nur die Fälschung, keine defekten Bereiche */
    }
    rc = 0;
out:
    free(a); free(b); free(pos); free(state);
    return rc;
}

void dt_scrub(int fd, uint64_t size) {
    void *z = NULL;
    if (posix_memalign(&z, 4096, MIB)) return;
    memset(z, 0, MIB);
    (void)io_rw(fd, 1, z, MIB, 0);
    (void)io_rw(fd, 1, z, MIB, MIB);
    if (size >= 4ull * MIB) (void)io_rw(fd, 1, z, MIB, (size - MIB) / 512 * 512);   /* GPT-Sicherungskopie: das letzte MiB bis ans Ende */
    dev_sync(fd);
    free(z);
}

/* Schreibfehler ab Position from: Ist dahinter nichts mehr beschreib- und lesbar (Stichproben an Zweierpotenz-Abständen
 * und am Ende), ist das die Grenze eines gefälschten Sticks - einzelne defekte Bereiche ließen Stellen dahinter
 * heil. 1 = alles dahinter tot. */
static int tail_dead(int fd, uint64_t from, uint64_t size, uint64_t nonce, uint8_t *a, uint8_t *b) {
    int tried = 0;
    for (uint64_t d = MIB; tried < 12; d *= 2) {
        uint64_t p = from + d > size - SAMPLE || d == 0 ? (size - SAMPLE) / SAMPLE * SAMPLE : (from + d) / SAMPLE * SAMPLE;
        if (p <= from) break;
        tagged(a, SAMPLE, nonce ^ 0x7a11u, p);
        if (io_all(fd, 1, a, SAMPLE, p) == 0) {
            dev_sync(fd);
            if (io_all(fd, 0, b, SAMPLE, p) == 0 && memcmp(a, b, SAMPLE) == 0) return 0;
        }
        tried++;
        if (p == (size - SAMPLE) / SAMPLE * SAMPLE) break;   /* das Ende ist geprüft */
    }
    return tried > 0;
}

/* Jeder 4-KiB-Block bekommt seine Position (wie f3write/f3read): Läuft die Adresse eines gefälschten Sticks über,
 * steht beim Prüfen an niedriger Stelle die Kennung einer höheren - die Differenz ist die echte Größe. Fehlende
 * oder falsche Daten ohne fremde Kennung sind defekte Bereiche bzw. (ab einer Stelle durchgehend) das Ende. */
int dt_full(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
            char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    phase_reset();
    phase(0, 50);   /* Beschreiben: erste Hälfte, Prüfen: zweite Hälfte - ein durchgehender Balken */
    io_wrote = 0;
    uint8_t *a = alloc_buf(CHUNK, err, errlen), *b = a ? alloc_buf(CHUNK, err, errlen) : NULL;
    int rc = -1;
    if (!a || !b) goto out;
    uint64_t total = size / SAMPLE * SAMPLE;
    const uint64_t nonce = nonce64();
    io_failed = 0;

    double t0 = now_s(), last = 0;
    uint64_t done = 0, write_end = 0;   /* write_end: Schreibfehler hier, dahinter nichts mehr beschreibbar */
    while (done < total) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
        size_t take = total - done < CHUNK ? (size_t)(total - done) : CHUNK;
        tagged(a, take, nonce, done);
        if (io_all(fd, 1, a, take, done)) {
            if (gone_error(fd, ss, done, err, errlen)) goto out;   /* abgezogen, nicht defekt */
            /* gefälschte Sticks antworten hinter der echten Grenze oft mit Fehlern: ist dahinter alles tot, ist das
             * das Ende des echten Speichers - geprüft wird dann nur das Geschriebene davor */
            if (done >= MIB && tail_dead(fd, done, size, nonce, a, b)) {
                write_end = done;
                total = done;
                break;
            }
            snprintf(err, errlen, T("Schreibfehler bei %llu MB: %s – der Datenträger ist vermutlich defekt"),
                     (unsigned long long)(done / MIB), strerror(errno));
            r->bad = 1;
            r->bad_at = done;
            goto out;
        }
        done += take;
        double t = now_s();
        if (t - last >= 1 || done == total) {
            rep(progress, T("Beschreiben"), (int)(done * 100 / total));
            last = t;
        }
    }
    dev_sync(fd);
    r->write_mbps = (double)total / (now_s() - t0) / 1e6;

    uint64_t alias = 0, first_lost = 0, lost = 0, blocks_after_lost = 0;
    int have_lost = 0;   /* first_lost gültig (0 ist eine echte Position: schon der erste Block verloren) */
    t0 = now_s();
    last = 0;
    done = 0;
    phase(50, 99);
    while (done < total) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) { r->cancelled = 1; rc = 0; goto out; }
        size_t take = total - done < CHUNK ? (size_t)(total - done) : CHUNK;
        int rerr = io_all(fd, 0, b, take, done) != 0;
        if (rerr && gone_error(fd, ss, done, err, errlen)) goto out;   /* abgezogen: kein "gefälscht ab hier" */
        tagged(a, take, nonce, done);
        if (rerr || memcmp(a, b, take) != 0) {
            for (size_t o = 0; o < take; o += SAMPLE) {
                if (!rerr && memcmp(a + o, b + o, SAMPLE) == 0) { if (have_lost) blocks_after_lost++; continue; }
                uint64_t p = done + o, other = rerr ? 0 : tag_pos(b + o, nonce);
                if (other > p) { alias = gcd64(alias, other - p); continue; }
                if (!have_lost) { have_lost = 1; first_lost = p; }
                lost++;
                blocks_after_lost++;
            }
        } else if (have_lost) {
            blocks_after_lost += take / SAMPLE;
        }
        done += take;
        double t = now_s();
        if (t - last >= 1 || done == total) {
            rep(progress, T("Prüfen"), (int)(done * 100 / total));
            last = t;
        }
    }
    r->read_mbps = (double)total / (now_s() - t0) / 1e6;

    /* ab first_lost (fast) alles verloren = Ende des echten Speichers, sonst einzelne defekte Bereiche. Ein
     * Schreibfehler mit totem Rest (write_end) ist ebenfalls ein Ende - sofern davor alles stimmte. */
    const int tail_lost = lost && lost * 10 >= blocks_after_lost * 9;
    uint64_t lost_limit = tail_lost ? first_lost : 0;
    if (write_end && (!have_lost || first_lost >= write_end)) lost_limit = write_end;
    else if (write_end && !tail_lost) {   /* davor schon einzelne defekte Bereiche: defekt, nicht gefälscht */
        r->bad = 1;
        r->bad_at = first_lost;
    }
    if (lost_limit && lost_limit < MIB) lost_limit = 0;   /* schon der Anfang verloren: kein nutzbarer Bereich */
    if (alias || lost_limit) {
        uint64_t real = alias && (!lost_limit || alias < lost_limit) ? alias : lost_limit;
        r->fake = 1;
        r->real_bytes = real / MIB * MIB;
        if (!r->real_bytes) r->fake = 0;   /* weniger als 1 MiB echt: das ist ein defekter Stick */
    }
    if (have_lost && !r->fake && !r->bad) { r->bad = 1; r->bad_at = first_lost; }
    rc = 0;
out:
    free(a); free(b);
    return rc;
}
