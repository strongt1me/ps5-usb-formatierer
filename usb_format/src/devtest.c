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

#define MIB         (1024u * 1024u)
#define CHUNK       (4u * MIB)
#define SAMPLE      4096u
#define MAX_SAMPLES 4096u
#define SPEED_BYTES (256ull * MIB)

static const char MAGIC[16] = "USBFMT-PROBE-v1";

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

#ifdef USBFMT_HOST
/* Nur Host-Tests: gefälschten Stick nachbilden. USBFMT_FAKE_REAL=<Bytes>, USBFMT_FAKE_MODE=wrap (Standard) | drop */
static uint64_t sim_real;
static int sim_drop, sim_ready;

static int sim_slow;   /* USBFMT_SLOW=1: je Zugriff 20 ms warten (Abbrechen testen) */

static void sim_load(void) {
    if (sim_ready) return;
    const char *r = getenv("USBFMT_FAKE_REAL"), *m = getenv("USBFMT_FAKE_MODE");
    sim_real = r ? strtoull(r, NULL, 10) : 0;
    sim_drop = m && strcmp(m, "drop") == 0;
    sim_slow = getenv("USBFMT_SLOW") != NULL;
    sim_ready = 1;
}
#endif

static ssize_t io_rw(int fd, int wr, void *buf, size_t len, uint64_t off) {
#ifdef USBFMT_HOST
    sim_load();
    if (sim_slow) usleep(20000);
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

static int io_all(int fd, int wr, void *buf, size_t len, uint64_t off) {
    for (size_t done = 0; done < len; ) {
        ssize_t n = io_rw(fd, wr, (uint8_t *)buf + done, len - done, off + done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
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
    if (posix_memalign(&p, 4096, len)) { snprintf(err, errlen, "Kein Speicher"); return NULL; }
    return p;
}

const char *dt_rating(double mbps) {
    return mbps >= 100 ? "schnell" : mbps >= 40 ? "ausreichend" : "langsam";
}

/* "noch ca. 12 Min." aus Restbytes und Tempo */
static void eta_text(char *out, size_t n, const char *what, uint64_t left, double bps) {
    if (bps <= 0) { snprintf(out, n, "%s", what); return; }
    double s = (double)left / bps;
    if (s < 60) snprintf(out, n, "%s – noch < 1 Min.", what);
    else if (s < 5400) snprintf(out, n, "%s – noch ca. %d Min.", what, (int)(s / 60 + 0.5));
    else snprintf(out, n, "%s – noch ca. %d Std. %d Min.", what, (int)(s / 3600), (int)(s / 60) % 60);
}

int dt_read_speed(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
                  char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    uint8_t *buf = alloc_buf(CHUNK, err, errlen);
    if (!buf) return -1;
    uint64_t start = size / 4 / MIB * MIB, total = SPEED_BYTES;
    if (start + total > size) total = (size - start) / ss * ss;
    uint64_t done = 0;
    double t0 = now_s();
    while (done < total) {
        if (*cancel) { r->cancelled = 1; break; }
        size_t take = total - done < CHUNK ? (size_t)(total - done) : CHUNK;
        if (io_all(fd, 0, buf, take, start + done)) {
            snprintf(err, errlen, "Lesefehler bei %llu MB: %s", (unsigned long long)((start + done) / MIB),
                     strerror(errno));
            r->bad = 1;
            r->bad_at = start + done;
            free(buf);
            return -1;
        }
        done += take;
        progress("Lesetempo messen", (int)(done * 100 / total));
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
    while (done < total) {
        if (*cancel) { r->cancelled = 1; return 0; }
        fill(a, CHUNK, nonce ^ (start + done));
        if (io_all(fd, 1, a, CHUNK, start + done)) {
            snprintf(err, errlen, "Schreibfehler bei %llu MB: %s", (unsigned long long)((start + done) / MIB),
                     strerror(errno));
            return -1;
        }
        done += CHUNK;
        progress("Schreibtempo messen", (int)(done * 100 / total));
        if (now_s() - t0 > 15 && done >= 16ull * MIB) break;
    }
    dev_sync(fd);
    double dt = now_s() - t0;
    r->write_mbps = dt > 0 ? (double)done / dt / 1e6 : 0;

    uint64_t written = done;
    done = 0;
    t0 = now_s();
    while (done < written) {
        if (*cancel) { r->cancelled = 1; return 0; }
        if (io_all(fd, 0, b, CHUNK, start + done)) {
            snprintf(err, errlen, "Lesefehler bei %llu MB: %s", (unsigned long long)((start + done) / MIB),
                     strerror(errno));
            return -1;
        }
        fill(a, CHUNK, nonce ^ (start + done));
        if (memcmp(a, b, CHUNK) != 0 && !r->bad) { r->bad = 1; r->bad_at = start + done; }
        done += CHUNK;
        progress("Lesetempo messen", (int)(done * 100 / written));
    }
    dt = now_s() - t0;
    r->read_mbps = dt > 0 ? (double)written / dt / 1e6 : 0;
    return 0;
}

int dt_verify(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
              char *err, size_t errlen) {
    (void)ss;
    memset(r, 0, sizeof(*r));
    uint8_t *a = alloc_buf(CHUNK, err, errlen), *b = a ? alloc_buf(CHUNK, err, errlen) : NULL;
    uint64_t *pos = NULL;
    uint8_t *state = NULL;   /* je Stichprobe: 0 offen, 1 gut, 2 fehlerhaft */
    int rc = -1;
    if (!a || !b) goto out;

    if (speed_rw(fd, size, progress, cancel, r, a, b, err, errlen)) goto out;
    if (r->cancelled) { rc = 0; goto out; }

    /* Raster: G = 16 MiB, verdoppelt bis höchstens 4096 Stichproben; dazu eine am Ende */
    uint64_t G = 16ull * MIB;
    while (size / G > MAX_SAMPLES) G *= 2;
    const uint64_t c = G / 2, nonce = nonce64();
    uint32_t n = 0;
    pos = malloc((MAX_SAMPLES + 2) * sizeof(*pos));
    state = calloc(MAX_SAMPLES + 2, 1);
    if (!pos || !state) { snprintf(err, errlen, "Kein Speicher"); goto out; }
    for (uint64_t p = c; p + SAMPLE <= size && n < MAX_SAMPLES; p += G) pos[n++] = p;
    uint64_t end = (size - SAMPLE) / SAMPLE * SAMPLE;
    if (!n || end > pos[n - 1]) pos[n++] = end;

    for (uint32_t i = 0; i < n; i++) {
        if (*cancel) { r->cancelled = 1; rc = 0; goto out; }
        tagged(a, SAMPLE, nonce, pos[i]);
        if (io_all(fd, 1, a, SAMPLE, pos[i])) state[i] = 3;
        if (i % 16 == 0) progress("Echtheit prüfen: schreiben", (int)((uint64_t)i * 100 / n));
    }
    dev_sync(fd);

    /* state: 1 gut, 2 überschrieben (eine spätere Stichprobe landete hier), 3 verloren (Fehler, Nullen, Müll) */
    uint64_t alias = 0, first_lost = 0, last_good = 0;
    uint32_t nlost = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (*cancel) { r->cancelled = 1; rc = 0; goto out; }
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
        if (i % 16 == 0) progress("Echtheit prüfen: lesen", (int)((uint64_t)i * 100 / n));
    }

    /* Verloren ab first_lost (fast) alles: Grenze in 1-MiB-Schritten zwischen der letzten guten und der ersten
     * verlorenen Stichprobe nachschärfen. Sonst sind es einzelne defekte Bereiche. */
    uint64_t lost_limit = 0;
    if (nlost) {
        uint32_t after = 0, lost_after = 0;
        for (uint32_t i = 0; i < n; i++)
            if (pos[i] >= first_lost) { after++; lost_after += state[i] == 3; }
        if (lost_after * 10 >= after * 9 && first_lost > 0) {
            uint64_t lo = (last_good + MIB - 1) / MIB * MIB, k = 0;
            uint64_t fine[64];
            for (uint64_t p = lo; p < first_lost && k < 64; p += MIB) fine[k++] = p;
            for (uint64_t i = 0; i < k; i++) {
                tagged(a, SAMPLE, nonce ^ 1, fine[i]);
                (void)io_all(fd, 1, a, SAMPLE, fine[i]);
            }
            dev_sync(fd);
            lost_limit = first_lost;
            for (uint64_t i = 0; i < k; i++) {
                tagged(a, SAMPLE, nonce ^ 1, fine[i]);
                if (io_all(fd, 0, b, SAMPLE, fine[i]) || memcmp(a, b, SAMPLE) != 0) { lost_limit = fine[i]; break; }
            }
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
    progress("Echtheit prüfen: lesen", 100);
    rc = 0;
out:
    free(a); free(b); free(pos); free(state);
    return rc;
}

/* Jeder 4-KiB-Block bekommt seine Position (wie f3write/f3read): Läuft die Adresse eines gefälschten Sticks über,
 * steht beim Prüfen an niedriger Stelle die Kennung einer höheren - die Differenz ist die echte Größe. Fehlende
 * oder falsche Daten ohne fremde Kennung sind defekte Bereiche bzw. (ab einer Stelle durchgehend) das Ende. */
int dt_full(int fd, uint32_t ss, uint64_t size, progress_fn progress, volatile int *cancel, devtest_t *r,
            char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    uint8_t *a = alloc_buf(CHUNK, err, errlen), *b = a ? alloc_buf(CHUNK, err, errlen) : NULL;
    char step[64];
    int rc = -1;
    if (!a || !b) goto out;
    const uint64_t total = size / SAMPLE * SAMPLE, nonce = nonce64();
    (void)ss;

    double t0 = now_s(), last = 0;
    uint64_t done = 0;
    while (done < total) {
        if (*cancel) { r->cancelled = 1; rc = 0; goto out; }
        size_t take = total - done < CHUNK ? (size_t)(total - done) : CHUNK;
        tagged(a, take, nonce, done);
        if (io_all(fd, 1, a, take, done)) {
            snprintf(err, errlen, "Schreibfehler bei %llu MB: %s – der Datenträger ist vermutlich defekt",
                     (unsigned long long)(done / MIB), strerror(errno));
            r->bad = 1;
            r->bad_at = done;
            goto out;
        }
        done += take;
        double t = now_s();
        if (t - last >= 1 || done == total) {
            eta_text(step, sizeof(step), "Beschreiben", total - done, (double)done / (t - t0));
            progress(step, (int)(done * 100 / total));
            last = t;
        }
    }
    dev_sync(fd);
    r->write_mbps = (double)total / (now_s() - t0) / 1e6;

    uint64_t alias = 0, first_lost = 0, lost = 0, blocks_after_lost = 0;
    t0 = now_s();
    last = 0;
    done = 0;
    while (done < total) {
        if (*cancel) { r->cancelled = 1; rc = 0; goto out; }
        size_t take = total - done < CHUNK ? (size_t)(total - done) : CHUNK;
        int rerr = io_all(fd, 0, b, take, done) != 0;
        tagged(a, take, nonce, done);
        if (rerr || memcmp(a, b, take) != 0) {
            for (size_t o = 0; o < take; o += SAMPLE) {
                if (!rerr && memcmp(a + o, b + o, SAMPLE) == 0) { if (first_lost) blocks_after_lost++; continue; }
                uint64_t p = done + o, other = rerr ? 0 : tag_pos(b + o, nonce);
                if (other > p) { alias = gcd64(alias, other - p); continue; }
                if (!first_lost) first_lost = p;
                lost++;
                blocks_after_lost++;
            }
        } else if (first_lost) {
            blocks_after_lost += take / SAMPLE;
        }
        done += take;
        double t = now_s();
        if (t - last >= 1 || done == total) {
            eta_text(step, sizeof(step), "Prüfen", total - done, (double)done / (t - t0));
            progress(step, (int)(done * 100 / total));
            last = t;
        }
    }
    r->read_mbps = (double)total / (now_s() - t0) / 1e6;

    /* ab first_lost (fast) alles verloren = Ende des echten Speichers, sonst einzelne defekte Bereiche */
    uint64_t lost_limit = lost && lost * 10 >= blocks_after_lost * 9 ? first_lost : 0;
    if (alias || lost_limit) {
        uint64_t real = alias && (!lost_limit || alias < lost_limit) ? alias : lost_limit;
        r->fake = 1;
        r->real_bytes = real / MIB * MIB;
    }
    if (lost && !lost_limit) { r->bad = 1; r->bad_at = first_lost; }
    rc = 0;
out:
    free(a); free(b);
    return rc;
}
