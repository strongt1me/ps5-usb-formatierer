/* Test-Plattform für Linux: Image-Dateien aus USBFMT_IMAGES (durch ':' getrennt) gelten als USB-Sticks,
 * USBFMT_MODELS (durch '|' getrennt) liefert die Gerätenamen dazu.
 * USBFMT_MOUNTED=1,0 markiert sie als eingebunden; USBFMT_MOUNTROOT=<ordner> lässt die Einbindung
 * "/mnt/usbN" auf <ordner>/usbN zeigen (Belegung, Spielebibliothek). USBFMT_INTERNAL=<a>:<b> sind weitere
 * Spiele-Ordner (wie /data/homebrew auf der PS5).
 * USBFMT_SLOW=1 verlangsamt das Formatieren, damit der Fortschritt sichtbar wird. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "extfs.h"
#include "fsdetect.h"
#include "platform.h"
#include "smpapi.h"

void plat_notify(const char *msg) { printf("[Benachrichtigung] %s\n", msg); fflush(stdout); }

void plat_throttle(void) {
    if (getenv("USBFMT_SLOW")) {
        struct timespec ts = {0, 40 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

int plat_scan(usbdev_t *out, int max) {
    const char *env = getenv("USBFMT_IMAGES");
    char list[2048];
    int n = 0;

    if (!env) return 0;
    snprintf(list, sizeof(list), "%s", env);
    for (char *save = NULL, *p = strtok_r(list, ":", &save); p && n < max; p = strtok_r(NULL, ":", &save)) {
        struct stat st;
        if (stat(p, &st) != 0) continue;
        usbdev_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "da%d", n);
        snprintf(d->path, sizeof(d->path), "%s", p);
        d->size = (uint64_t)st.st_size;
        d->ss = 512;
        int fd = open(p, O_RDONLY);
        fsinfo_t fi = { .fs = "unbekannt", .scheme = "" };
        if (fd >= 0) fs_probe(fd, d->ss, d->size, &fi);
        snprintf(d->format, sizeof(d->format), "%s", fi.fs);
        snprintf(d->label, sizeof(d->label), "%s", fi.label);
        snprintf(d->scheme, sizeof(d->scheme), "%s", fi.scheme);
        if (fd >= 0) close(fd);
        const char *models = getenv("USBFMT_MODELS");
        for (int k = 0; models && k < n; k++) {
            models = strchr(models, '|');
            if (models) models++;
        }
        if (models) snprintf(d->model, sizeof(d->model), "%.*s", (int)strcspn(models, "|"), models);
        /* USBFMT_MOUNTED=1,0: pro Image, ob es als eingehängt gilt (Standard: nicht eingehängt) */
        const char *mounted = getenv("USBFMT_MOUNTED");
        for (int k = 0; mounted && k < n; k++) {
            mounted = strchr(mounted, ',');
            if (mounted) mounted++;
        }
        if (mounted && *mounted == '1') {
            const char *root = getenv("USBFMT_MOUNTROOT");
            d->nmounts = 1;
            if (root) snprintf(d->mounts[0].path, sizeof(d->mounts[0].path), "%.80s/usb%d", root, n);
            else snprintf(d->mounts[0].path, sizeof(d->mounts[0].path), "/mnt/usb%d", n);
            snprintf(d->mounts[0].fstype, sizeof(d->mounts[0].fstype), "exfatfs");
        }
        n++;
    }
    return n;
}

int plat_open_rw(const usbdev_t *d, char *err, size_t errlen) {
    int fd = open(d->path, O_RDWR);
    if (fd < 0) snprintf(err, errlen, "%s kann nicht geöffnet werden", d->path);
    return fd;
}

int plat_open_ro(const usbdev_t *d, char *err, size_t errlen) {
    int fd = open(d->path, O_RDONLY);
    if (fd < 0) snprintf(err, errlen, "%s kann nicht gelesen werden", d->path);
    return fd;
}

const char *const *plat_library_roots(void) {
    static char list[1024];
    static const char *roots[17];
    const char *env = getenv("USBFMT_INTERNAL");
    int n = 0;
    if (env) {
        snprintf(list, sizeof(list), "%s", env);
        for (char *save = NULL, *p = strtok_r(list, ":", &save); p && n < 15; p = strtok_r(NULL, ":", &save)) {
            roots[n++] = p;
            roots[n++] = "dir";
        }
    }
    roots[n] = NULL;
    return roots;
}

/* Test: Abziehen/Einstecken wird nur angezeigt. USBFMT_REPLUG=wait wartet bis zum Abbrechen (POST /api/cancel). */
int plat_wait_replug(const usbdev_t *d, usbdev_t *out, progress_fn progress, volatile int *cancel,
                     char *err, size_t errlen) {
    const char *mode = getenv("USBFMT_REPLUG");
    progress("Stick jetzt abziehen", 0);
    if (mode && strcmp(mode, "wait") == 0) {
        for (int i = 0; i < 600 && !*cancel; i++) {
            struct timespec ts = {0, 50 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        snprintf(err, errlen, "%s", *cancel ? "Abgebrochen." : "Der Stick wurde nicht abgezogen.");
        return -1;
    }
    progress("Stick jetzt wieder einstecken", 0);
    *out = *d;
    out->nmounts = 0;
    return plat_open_rw(out, err, errlen);
}

void plat_local_ip(char *buf, size_t len) {
    const char *ip = getenv("USBFMT_IP");
    snprintf(buf, len, "%s", ip ? ip : "127.0.0.1");
}

const char *plat_data_dir(void) {
    const char *d = getenv("USBFMT_DATA");
    return d ? d : "/tmp/usbfmt";
}

/* Linux-Test: die echten Linux-Werkzeuge (PATH, z. B. ~/fstools/root/usr/sbin) auf eine Partitions-Datei
 * anwenden und diese dann an die Partitionsstelle des Images kopieren */
int plat_external_mkfs(const usbdev_t *d, int fs, const char *label, uint32_t cluster, uint64_t start,
                       uint64_t count, int gpt, progress_fn progress, char *summary, size_t sumlen,
                       char *err, size_t errlen) {
    (void)gpt;   /* Image: die Partition liegt bei start, gleich ob MBR oder GPT */
    char part[300], line[512];
    static char out[65536];
    size_t olen = 0, ll = 0;
    extfs_cmd_t cmd;
    int pfd[2];

    snprintf(part, sizeof(part), "%s.part", d->path);
    int tfd = open(part, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0 || ftruncate(tfd, (off_t)(count * d->ss)) != 0) {
        snprintf(err, errlen, "%s: %s", part, strerror(errno));
        if (tfd >= 0) close(tfd);
        return -1;
    }
    close(tfd);
    extfs_build(&cmd, fs, part, label, cluster, d->ss, start);

    progress(fs == FS_NTFS ? "NTFS schreiben (mkntfs)" : "ext4 schreiben (mke2fs)", 5);
    if (pipe(pfd) != 0) { snprintf(err, errlen, "pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        int null = open("/dev/null", O_RDONLY);
        dup2(null, 0);
        execvp(cmd.argv[0], cmd.argv);
        fprintf(stderr, "%s nicht gefunden\n", cmd.argv[0]);
        _exit(127);
    }
    close(pfd[1]);
    char buf[1024];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (olen + 1 < sizeof(out)) out[olen++] = buf[i];
            if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\b') {
                line[ll] = 0;
                int pct;
                const char *step = ll ? extfs_step(fs, line, &pct) : NULL;
                if (step) progress(step, pct);
                ll = 0;
            } else if (ll + 1 < sizeof(line)) {
                line[ll++] = buf[i];
            }
        }
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    out[olen] = 0;
    if (!extfs_succeeded(fs, out)) {
        snprintf(err, errlen, "%s ist fehlgeschlagen: %.200s", cmd.argv[0], out);
        unlink(part);
        return -1;
    }

    /* In das Image kopieren; Nullblöcke nur schreiben, wo das Ziel nicht schon null ist (Image bleibt dünn) */
    int src = open(part, O_RDONLY), dst = open(d->path, O_RDWR);
    static unsigned char a[1 << 20], b[1 << 20];
    static const unsigned char zero[1 << 20];
    uint64_t total = count * d->ss, off = 0;
    int ok = src >= 0 && dst >= 0;
    while (ok && off < total) {
        size_t want = total - off < sizeof(a) ? (size_t)(total - off) : sizeof(a);
        ok = pread(src, a, want, (off_t)off) == (ssize_t)want;
        if (!ok) break;
        if (memcmp(a, zero, want) != 0 ||
            (pread(dst, b, want, (off_t)(start * d->ss + off)) == (ssize_t)want && memcmp(b, zero, want) != 0))
            ok = pwrite(dst, a, want, (off_t)(start * d->ss + off)) == (ssize_t)want;
        off += want;
    }
    unsigned char head[4096];
    ok = ok && pread(dst, head, sizeof(head), (off_t)(start * d->ss)) == (ssize_t)sizeof(head) &&
         extfs_signature_ok(fs, head);
    if (src >= 0) close(src);
    if (dst >= 0) close(dst);
    unlink(part);
    if (!ok) {
        snprintf(err, errlen, "Partition ließ sich nicht ins Image übernehmen");
        return -1;
    }
    extfs_summary(fs, cluster, out, summary, sumlen);
    return 0;
}

static char *slurp_file(const char *path, size_t *len) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    char *buf = NULL;
    *len = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        rewind(f);
        if (n > 0 && (buf = malloc((size_t)n + 1))) { *len = fread(buf, 1, (size_t)n, f); buf[*len] = 0; }
    }
    fclose(f);
    return buf;
}

/* Test: ShadowMountPlus-Attrappe auf 127.0.0.1:USBFMT_SMP_PORT (test/smp_mock.py), sonst nicht erreichbar */
char *plat_smp_post(const char *path, const char *body, int *http, size_t *len) {
    const char *port = getenv("USBFMT_SMP_PORT");
    *http = 0;
    *len = 0;
    return port ? smp_http_post(atoi(port), path, body, http, len) : NULL;
}

/* Test: USBFMT_SMP_GAMES = Antwort von /api/v1/games, USBFMT_SMP_LOG = debug.log von ShadowMountPlus */
void plat_library_hints(lib_hint_t **h, int *n) {
    size_t len;
    char *buf;
    *h = NULL;
    *n = 0;
    if ((buf = slurp_file(getenv("USBFMT_SMP_LOG"), &len))) { lib_parse_smp_log(buf, len, h, n); free(buf); }
    if ((buf = slurp_file(getenv("USBFMT_SMP_GAMES"), &len))) { lib_parse_smp_games(buf, len, h, n); free(buf); }
}

int plat_open_browser(const char *url, char *err, size_t errlen) {
    (void)err; (void)errlen;
    printf("[Browser] %s\n", url);
    fflush(stdout);
    return 0;
}
