/* Einen Spielstand der Konsole öffnen (savemnt.h) - übernommen aus dem PS5 Cooling Center (savemnt.c) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"
#include "platform.h"
#include "savemnt.h"

int savemnt_header_kind(const uint8_t *header, size_t len) {
    if (!header || len < 0x40) return 0;
    uint64_t v = 0, magic = 0;
    for (int i = 7; i >= 0; i--) { v = (v << 8) | header[i]; magic = (magic << 8) | header[8 + i]; }
    if (magic != 20130315u) return 0;
    return v == 1 ? 1 : v == 2 ? 2 : 0;
}

#ifdef USBFMT_HOST

/* ── Host (Tests): kein Kernel ──
 * Jedes Abbild trägt ab Versatz 0x100 eine Kennung (16 Zeichen, "usbfmt-host-id:" davor). Einhängen kopiert den Ordner
 * <USBFMT_SAVEMNT_STORE>/<Kennung> in den Einhängeordner, Aushängen schreibt ihn zurück und leert den Einhängeordner.
 * Neue Abbilder bekommen eine neue Kennung. <Kennung>.param steht für den Blob im Kopf (fehlt er: keiner).
 * USBFMT_SAVEMNT_FAIL: "open", "close", "create" lässt das jeweils nächste scheitern (einmal je Datei-Marker
 * <Store>/.fail_<Name>). USBFMT_SAVEMNT_PARAM_ERR: die Frage nach dem Blob beantwortet die "Konsole" nicht. */

#define HOST_ID_OFF 0x100
#define HOST_ID_TAG "usbfmt-host-id:"

static int host_fail(const char *what) {
    const char *store = getenv("USBFMT_SAVEMNT_STORE");
    char p[1100];
    if (!store || snprintf(p, sizeof(p), "%s/.fail_%s", store, what) >= (int)sizeof(p)) return 0;
    if (unlink(p) == 0) return 1;
    return 0;
}

static int host_id(const char *image, char id[17]) {
    char buf[64];
    int fd = open(image, O_RDONLY);
    ssize_t n = fd >= 0 ? pread(fd, buf, sizeof(buf), HOST_ID_OFF) : -1;
    if (fd >= 0) close(fd);
    const size_t tl = strlen(HOST_ID_TAG);
    if (n < (ssize_t)(tl + 16) || memcmp(buf, HOST_ID_TAG, tl)) return -1;
    memcpy(id, buf + tl, 16);
    id[16] = 0;
    for (int i = 0; i < 16; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'z'))) return -1;
    return 0;
}

static int host_copy_tree(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s2[1100], d2[1100];
        snprintf(s2, sizeof(s2), "%s/%s", src, e->d_name);
        snprintf(d2, sizeof(d2), "%s/%s", dst, e->d_name);
        struct stat st;
        if (lstat(s2, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(d2, 0755) != 0 && errno != EEXIST) { rc = -1; break; }
            rc = host_copy_tree(s2, d2);
            chmod(d2, st.st_mode & 07777);
        } else if (S_ISREG(st.st_mode)) {
            FILE *in = fopen(s2, "rb"), *out = fopen(d2, "wb");
            if (!in || !out) rc = -1;
            else {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), in)) > 0) if (fwrite(buf, 1, n, out) != n) rc = -1;
            }
            if (in) fclose(in);
            if (out) fclose(out);
            chmod(d2, st.st_mode & 07777);
        }
    }
    closedir(d);
    return rc;
}

static void host_clear_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1100];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { host_clear_tree(p); rmdir(p); }
        else unlink(p);
    }
    closedir(d);
}

static int host_kept(const char *image, char *out, size_t n) {
    const char *store = getenv("USBFMT_SAVEMNT_STORE");
    char id[17];
    if (!store || host_id(image, id) != 0) return -1;
    return snprintf(out, n, "%s/%s", store, id) < (int)n ? 0 : -1;
}

static int host_open(const char *image, int want_kind, const char *mount_dir, savemnt_t *m, char *err, size_t err_len) {
    memset(m, 0, sizeof(*m));
    if (err_len) err[0] = 0;
    uint8_t head[0x40];
    int hfd = open(image, O_RDONLY);
    const ssize_t hn = hfd >= 0 ? pread(hfd, head, sizeof(head), 0) : -1;
    if (hfd >= 0) close(hfd);
    if (hn != (ssize_t)sizeof(head) || savemnt_header_kind(head, sizeof(head)) != want_kind) {
        snprintf(err, err_len, "%s", want_kind == 1 ? T("Das ist kein PS4-Spielstand.") : T("Das ist kein PS5-Spielstand."));
        return -1;
    }
    if (host_fail("open")) { snprintf(err, err_len, T("Die Konsole hat den Spielstand nicht eingehängt (Antwort 0x%x)."), 0x80020016u); return -1; }
    char kept[1100];
    struct stat ks;
    mkdir(mount_dir, 0755);
    if (host_kept(image, kept, sizeof(kept)) == 0 && stat(kept, &ks) == 0 && host_copy_tree(kept, mount_dir) != 0) {
        snprintf(err, err_len, "host: copy %s", kept);   /* nicht übersetzt */
        return -1;
    }
    snprintf(m->image, sizeof(m->image), "%s", image);
    snprintf(m->mount, sizeof(m->mount), "%s", mount_dir);
    m->open = 1;
    return 0;
}

int savemnt_open(const char *image, const char *mount_dir, savemnt_t *m, char *err, size_t err_len) {
    return host_open(image, 2, mount_dir, m, err, err_len);
}

int savemnt_open_sealed(const char *image, const uint8_t sealed[96], const char *mount_dir, savemnt_t *m, char *err, size_t err_len) {
    (void)sealed;
    return host_open(image, 1, mount_dir, m, err, err_len);
}

int savemnt_close(savemnt_t *m, char *err, size_t err_len) {
    if (err_len) err[0] = 0;
    if (!m || !m->open) return 0;
    m->open = 0;
    if (host_fail("close")) {
        snprintf(err, err_len, T("Die Konsole hat den Spielstand nicht sauber ausgehängt (Antwort 0x%x)."), 0x80020016u);
        return -1;
    }
    char kept[1100];
    if (host_kept(m->image, kept, sizeof(kept)) == 0) {
        mkdir(getenv("USBFMT_SAVEMNT_STORE"), 0755);
        mkdir(kept, 0755);
        host_clear_tree(kept);
        host_copy_tree(m->mount, kept);
    }
    host_clear_tree(m->mount);
    return 0;
}

static int host_create(const char *path, uint64_t size, int ver, uint32_t block_size, char *err, size_t err_len) {
    if (host_fail("create")) {
        snprintf(err, err_len, T("Die Konsole konnte das neue Abbild nicht formatieren (Antwort 0x%x)."), 0x80020016u);
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (fd < 0) { snprintf(err, err_len, T("Das neue Abbild ließ sich nicht anlegen (Fehler %d)."), errno); return -1; }
    uint8_t head[0x400];
    memset(head, 0, sizeof(head));
    const uint64_t version = (uint64_t)ver, magic = 20130315, blocks = size / block_size;
    memcpy(head, &version, 8);
    memcpy(head + 8, &magic, 8);
    if (ver == 2) {
        head[0x1c] = 0x0d;
        head[0x24] = 1;
        memcpy(head + 0x38, &blocks, 8);
    }
    memcpy(head + 0x20, &block_size, 4);
    static unsigned seq;
    char id[40];
    snprintf(id, sizeof(id), "%s%08x%08x", HOST_ID_TAG, (unsigned)getpid(), ++seq);
    memcpy(head + HOST_ID_OFF, id, strlen(id));
    if (write(fd, head, sizeof(head)) != (ssize_t)sizeof(head) || ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        unlink(path);
        snprintf(err, err_len, T("Das neue Abbild ließ sich nicht anlegen (Fehler %d)."), errno);
        return -1;
    }
    close(fd);
    char kept[1100];
    if (host_kept(path, kept, sizeof(kept)) == 0) { host_clear_tree(kept); rmdir(kept); }
    return 0;
}

int savemnt_create(const char *path, uint64_t size, uint32_t block_size, char *err, size_t err_len) {
    if (err_len) err[0] = 0;
    if ((block_size != 32768 && block_size != 65536) || size < 2 * (uint64_t)block_size || size % block_size) {
        snprintf(err, err_len, "%s", T("Die Größe des neuen Abbilds ist ungültig."));
        return -1;
    }
    return host_create(path, size, 2, block_size, err, err_len);
}

int savemnt_create_ps4(const char *path, uint64_t size, uint8_t sealed_out[96], char *err, size_t err_len) {
    if (err_len) err[0] = 0;
    if (size < 65536 || size % 32768) { snprintf(err, err_len, "%s", T("Die Größe des neuen Abbilds ist ungültig.")); return -1; }
    if (host_create(path, size, 1, 32768, err, err_len) != 0) return -1;
    for (int i = 0; i < 96; i++) sealed_out[i] = (uint8_t)(0xA0 + i);
    return 0;
}

int savemnt_umount_path(const char *mount_dir, char *err, size_t err_len) {
    if (err_len) err[0] = 0;
    host_clear_tree(mount_dir);
    return 0;
}

int savemnt_param_read(const char *image, int already_elevated, uint8_t *sfo, uint32_t sfo_cap, uint32_t *sfo_len, char *err, size_t err_len) {
    (void)already_elevated;
    if (err_len) err[0] = 0;
    *sfo_len = 0;
    if (getenv("USBFMT_SAVEMNT_PARAM_ERR")) {
        snprintf(err, err_len, T("Die Konsole beantwortet die Frage nach den Metadaten des Spielstands nicht (Fehler %d;%s)"), 25, " host");
        return -2;
    }
    char kept[1100], p[1200];
    if (host_kept(image, kept, sizeof(kept)) != 0 || snprintf(p, sizeof(p), "%s.param", kept) >= (int)sizeof(p)) return -1;
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(sfo, 1, sfo_cap, f);
    fclose(f);
    *sfo_len = (uint32_t)n;
    return n ? 0 : -1;
}

#else

/* ── Konsole ── */

#define AUTHID_SAVEDATA 0x4800000000000010ULL
#define PFSMGR_UNSEAL   0xc0845302u
#define VSH "libSceFsInternalForVsh.sprx"

typedef struct { uint8_t reserved; char *budgetid; } mount_opt_t;   /* Aufbau der Konsole: ein Flag-Byte, dann ein Zeiger */
typedef struct { uint8_t dummy; } umount_opt_t;

typedef int (*fn_init_mount_t)(mount_opt_t *);
typedef int (*fn_mount_t)(mount_opt_t *, const char *, const char *, uint8_t *);
typedef int (*fn_init_umount_t)(umount_opt_t *);
typedef int (*fn_umount_t)(umount_opt_t *, const char *, int, int);

static fn_init_mount_t  fn_init_mount;
static fn_mount_t       fn_mount;
static fn_init_umount_t fn_init_umount;
static fn_umount_t      fn_umount;

static int vsh_ready(char *err, size_t err_len) {
    if (fn_mount && fn_umount && fn_init_mount && fn_init_umount) return 0;
    fn_init_mount = (fn_init_mount_t)plat_dynsym(VSH, "sceFsInitMountSaveDataOpt");
    fn_mount = (fn_mount_t)plat_dynsym(VSH, "sceFsMountSaveData");
    fn_init_umount = (fn_init_umount_t)plat_dynsym(VSH, "sceFsInitUmountSaveDataOpt");
    fn_umount = (fn_umount_t)plat_dynsym(VSH, "sceFsUmountSaveData");
    if (fn_mount && fn_umount && fn_init_mount && fn_init_umount) return 0;
    fn_mount = NULL;
    snprintf(err, err_len, T("Die Konsole stellt das Einhängen von Spielständen nicht bereit (Funktionen in %s nicht gefunden)."), VSH);
    return -1;
}

static int enter(char *err, size_t err_len) {
    if (plat_save_service_enter(AUTHID_SAVEDATA) != 0) {
        snprintf(err, err_len, "%s", T("Die Rechte für den Spielstand-Dienst ließen sich nicht setzen (läuft der Formatierer über den Jailbreak?)."));
        return -1;
    }
    return 0;
}

/* gebundenen Schlüssel vom Kernel öffnen lassen; key32 = die 32 Byte. Die Kennung muss gesetzt sein. */
static int unseal(const char *image, const uint8_t *sealed, uint8_t key32[32], char *err, size_t err_len) {
    uint8_t *buf = calloc(1, 0x100);   /* auf dem Heap: der Treiber schreibt über das hinaus, was 0x84 auf dem Stack fassten */
    if (!buf) { snprintf(err, err_len, "%s", T("Kein Speicher")); return -1; }
    if (sealed) {
        memcpy(buf, sealed, SAVEMNT_SEALED_BYTES);   /* PS4: der Schlüssel aus der Begleitdatei */
    } else {
        int fd = open(image, O_RDONLY);
        if (fd < 0 || pread(fd, buf, SAVEMNT_SEALED_BYTES, SAVEMNT_SEALED_OFFSET) != (ssize_t)SAVEMNT_SEALED_BYTES) {
            snprintf(err, err_len, T("Der versiegelte Schlüssel des Spielstands ließ sich nicht lesen (Fehler %d)."), errno);
            if (fd >= 0) close(fd);
            free(buf);
            return -1;
        }
        close(fd);
    }
    int pfsmgr = open("/dev/pfsmgr", O_RDWR);
    if (pfsmgr < 0) {
        snprintf(err, err_len, T("Die Konsole lässt /dev/pfsmgr nicht öffnen (Fehler %d)."), errno);
        free(buf);
        return -1;
    }
    errno = 0;
    const int rc = ioctl(pfsmgr, PFSMGR_UNSEAL, buf);
    const int e = errno;
    close(pfsmgr);
    if (rc < 0) {
        snprintf(err, err_len, T("Die Konsole konnte den Schlüssel des Spielstands nicht öffnen (Antwort %d, Fehler %d)."), rc, e);
        free(buf);
        return -1;
    }
    memcpy(key32, buf + 0x60, 32);
    memset(buf, 0, 0x100);
    free(buf);
    return 0;
}

static int open_impl(const char *image, const uint8_t *sealed, const char *mount_dir, savemnt_t *m, char *err, size_t err_len) {
    memset(m, 0, sizeof(*m));
    if (err_len) err[0] = '\0';
    if (vsh_ready(err, err_len) != 0) return -1;
    struct stat st;
    if (stat(image, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, err_len, "%s", T("Die Abbild-Datei des Spielstands fehlt."));
        return -1;
    }
    uint8_t head[0x40];
    int hfd = open(image, O_RDONLY);
    const ssize_t hn = hfd >= 0 ? pread(hfd, head, sizeof(head), 0) : -1;
    if (hfd >= 0) close(hfd);
    if (hn != (ssize_t)sizeof(head) || savemnt_header_kind(head, sizeof(head)) != (sealed ? 1 : 2)) {
        snprintf(err, err_len, "%s", sealed ? T("Das ist kein PS4-Spielstand.") : T("Das ist kein PS5-Spielstand."));
        return -1;
    }
    mkdir(mount_dir, 0755);
    if (enter(err, err_len) != 0) return -1;
    uint8_t key[32];
    if (unseal(image, sealed, key, err, err_len) != 0) {
        plat_save_service_leave();
        return -1;
    }
    static char budget[] = "system";   /* die Option hält einen Zeiger, kein Feld */
    mount_opt_t opt;
    memset(&opt, 0, sizeof(opt));
    fn_init_mount(&opt);
    opt.budgetid = budget;
    struct sigaction ign, old;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_DFL;   /* wie die Werkzeuge der Konsole beim Einhängen */
    sigaction(SIGPIPE, &ign, &old);
    const int h = fn_mount(&opt, image, mount_dir, key);
    sigaction(SIGPIPE, &old, NULL);
    memset(key, 0, sizeof(key));
    if (h < 0) {
        snprintf(err, err_len, T("Die Konsole hat den Spielstand nicht eingehängt (Antwort 0x%x)."), (unsigned)h);
        plat_save_service_leave();
        return -1;
    }
    m->open = 1;
    m->handle = h;
    snprintf(m->image, sizeof(m->image), "%s", image);
    snprintf(m->mount, sizeof(m->mount), "%s", mount_dir);
    /* das Einhängen steht: Kennung und Sperre gleich zurück (wie das Cooling Center seit 1.88.0) - das Lesen und Schreiben der Dateien
     * darin sind gewöhnliche Dateiaufrufe und dauern bei großen Spielständen Minuten; savemnt_close nimmt beides fürs Aushängen wieder */
    plat_save_service_leave();
    m->released = 1;
    return 0;
}

int savemnt_open(const char *image, const char *mount_dir, savemnt_t *m, char *err, size_t err_len) {
    return open_impl(image, NULL, mount_dir, m, err, err_len);
}

int savemnt_open_sealed(const char *image, const uint8_t sealed[96], const char *mount_dir, savemnt_t *m, char *err, size_t err_len) {
    return open_impl(image, sealed, mount_dir, m, err, err_len);
}

int savemnt_close(savemnt_t *m, char *err, size_t err_len) {
    if (err_len) err[0] = '\0';
    if (!m || !m->open) return 0;
    if (m->released) {   /* Kennung und Sperre wieder, fürs Aushängen */
        if (plat_save_service_enter(AUTHID_SAVEDATA) != 0) {
            snprintf(err, err_len, "%s", T("Die Rechte für den Spielstand-Dienst ließen sich zum Aushängen nicht setzen."));
            return -1;   /* noch eingehängt (m->open bleibt 1): der Aufrufer sagt "PS5 neu starten", wie bei jedem gescheiterten Aushängen */
        }
        m->released = 0;
    }
    int rc = 0;
    umount_opt_t uopt;
    memset(&uopt, 0, sizeof(uopt));
    fn_init_umount(&uopt);
    struct sigaction ign, old;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &ign, &old);
    const int r = fn_umount(&uopt, m->mount, 0, 0);
    sigaction(SIGPIPE, &old, NULL);
    if (r < 0) {
        snprintf(err, err_len, T("Die Konsole hat den Spielstand nicht sauber ausgehängt (Antwort 0x%x)."), (unsigned)r);
        rc = -1;
    }
    sync();
    m->open = 0;
    plat_save_service_leave();
    return rc;
}

int savemnt_umount_path(const char *mount_dir, char *err, size_t err_len) {
    if (err_len) err[0] = '\0';
    if (vsh_ready(err, err_len) != 0) return -1;
    if (enter(err, err_len) != 0) return -1;
    umount_opt_t uopt;
    memset(&uopt, 0, sizeof(uopt));
    fn_init_umount(&uopt);
    struct sigaction ign, old;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &ign, &old);
    const int r = fn_umount(&uopt, mount_dir, 0, 0);
    sigaction(SIGPIPE, &old, NULL);
    plat_save_service_leave();
    if (r < 0) {
        snprintf(err, err_len, T("Die Konsole hat einen Rest eines früheren Einhängens nicht gelöst (Antwort 0x%x)."), (unsigned)r);
        return -1;
    }
    return 0;
}

typedef struct { int blockSize; uint8_t flags[2]; } create_opt_t;   /* Aufbau der Konsole */
typedef int (*fn_init_create_t)(create_opt_t *);
typedef int (*fn_ppr_create_t)(create_opt_t *, const char *, int, uint64_t);
typedef int (*fn_create_old_t)(create_opt_t *, const char *, int, uint64_t, uint8_t *);
typedef int (*fn_alloc_t)(int, uint64_t, int, void *);

int savemnt_create(const char *path, uint64_t size, uint32_t block_size, char *err, size_t err_len) {
    if (err_len) err[0] = '\0';
    if ((block_size != 32768 && block_size != 65536) || size < 2 * (uint64_t)block_size || size % block_size) {
        snprintf(err, err_len, "%s", T("Die Größe des neuen Abbilds ist ungültig."));
        return -1;
    }
    fn_init_create_t init = (fn_init_create_t)plat_dynsym(VSH, "sceFsInitCreatePfsSaveDataOpt");
    fn_ppr_create_t create = (fn_ppr_create_t)plat_dynsym(VSH, "sceFsCreatePprPfsSaveDataImage");
    fn_alloc_t alloc = (fn_alloc_t)plat_dynsym(VSH, "sceFsUfsAllocateSaveData");
    if (!init || !create || !alloc) {
        snprintf(err, err_len, T("Die Konsole stellt das Anlegen von Spielstand-Abbildern nicht bereit (Funktionen in %s nicht gefunden)."), VSH);
        return -1;
    }
    if (enter(err, err_len) != 0) return -1;
    int rc = -1;
    const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0755);
    if (fd < 0) {
        snprintf(err, err_len, T("Das neue Abbild ließ sich nicht anlegen (Fehler %d)."), errno);
    } else {
        errno = 0;
        const int ar = alloc(fd, size, 0x80, NULL);   /* der Platz wie bei den eigenen Spielstand-Abbildern der Konsole */
        const int ae = errno;
        close(fd);
        if (ar < 0) {
            snprintf(err, err_len, T("Die Konsole konnte den Platz für das neue Abbild nicht anlegen (Antwort 0x%x, Fehler %d)."), (unsigned)ar, ae);
            unlink(path);
        } else {
            create_opt_t opt;
            memset(&opt, 0, sizeof(opt));
            init(&opt);
            opt.blockSize = (int)block_size;
            opt.flags[1] = 0;
            struct sigaction ign, old;
            memset(&ign, 0, sizeof(ign));
            ign.sa_handler = SIG_DFL;
            sigaction(SIGPIPE, &ign, &old);
            const int r = create(&opt, path, 0, size);
            sigaction(SIGPIPE, &old, NULL);
            if (r != 0) {
                snprintf(err, err_len, T("Die Konsole konnte das neue Abbild nicht formatieren (Antwort 0x%x)."), (unsigned)r);
                unlink(path);
            } else {
                sync();
                rc = 0;
            }
        }
    }
    plat_save_service_leave();
    return rc;
}

int savemnt_create_ps4(const char *path, uint64_t size, uint8_t sealed_out[96], char *err, size_t err_len) {
    if (err_len) err[0] = '\0';
    if (size < 65536 || size % 32768) {
        snprintf(err, err_len, "%s", T("Die Größe des neuen Abbilds ist ungültig."));
        return -1;
    }
    fn_init_create_t init = (fn_init_create_t)plat_dynsym(VSH, "sceFsInitCreatePfsSaveDataOpt");
    fn_create_old_t create = (fn_create_old_t)plat_dynsym(VSH, "sceFsCreatePfsSaveDataImage");
    fn_alloc_t alloc = (fn_alloc_t)plat_dynsym(VSH, "sceFsUfsAllocateSaveData");
    if (!init || !create || !alloc) {
        snprintf(err, err_len, T("Die Konsole stellt das Anlegen von Spielstand-Abbildern nicht bereit (Funktionen in %s nicht gefunden)."), VSH);
        return -1;
    }
    if (enter(err, err_len) != 0) return -1;
    int rc = -1;
    uint8_t *buf = calloc(1, 0x100);
    if (!buf) {
        snprintf(err, err_len, "%s", T("Kein Speicher"));
    } else {
        /* frischer, an diese Konsole gebundener Schlüssel: dieselben zwei Knoten wie die Werkzeuge der Konsole, der erste, der antwortet */
        int got = 0;
        int kfd = open("/dev/sbl_srv", O_RDWR);
        if (kfd >= 0) { if (ioctl(kfd, 0x40845303, buf) >= 0) got = 1; close(kfd); }
        if (!got) {
            memset(buf, 0, 0x100);
            kfd = open("/dev/pfsmgr", O_RDWR);
            if (kfd >= 0) { if (ioctl(kfd, 0x40845303, buf) >= 0) got = 1; close(kfd); }
        }
        if (!got) {
            snprintf(err, err_len, "%s", T("Die Konsole konnte keinen Schlüssel für das neue Abbild erzeugen."));
        } else {
            memcpy(sealed_out, buf, SAVEMNT_SEALED_BYTES);
            uint8_t key[32];
            int ok = 0;
            memset(buf, 0, 0x100);
            memcpy(buf, sealed_out, SAVEMNT_SEALED_BYTES);
            int pf = open("/dev/pfsmgr", O_RDWR);
            if (pf >= 0) { if (ioctl(pf, PFSMGR_UNSEAL, buf) >= 0) { memcpy(key, buf + 0x60, 32); ok = 1; } close(pf); }
            if (!ok) {
                snprintf(err, err_len, "%s", T("Die Konsole konnte den neuen Schlüssel nicht öffnen."));
            } else {
                const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0755);
                if (fd < 0) {
                    snprintf(err, err_len, T("Das neue Abbild ließ sich nicht anlegen (Fehler %d)."), errno);
                } else {
                    const int ar = alloc(fd, size, 0, NULL);
                    if (ar < 0 && ftruncate(fd, (off_t)size) != 0) {   /* Platz von der Konsole, sonst als gewöhnliche Datei */
                        const int ae = errno;
                        close(fd);
                        unlink(path);
                        snprintf(err, err_len, T("Die Konsole konnte den Platz für das neue Abbild nicht anlegen (Antwort 0x%x, Fehler %d)."), (unsigned)ar, ae);
                    } else {
                        close(fd);
                        create_opt_t opt;
                        memset(&opt, 0, sizeof(opt));
                        init(&opt);
                        struct sigaction ign, old;
                        memset(&ign, 0, sizeof(ign));
                        ign.sa_handler = SIG_DFL;
                        sigaction(SIGPIPE, &ign, &old);
                        const int r = create(&opt, path, 0, size, key);
                        sigaction(SIGPIPE, &old, NULL);
                        if (r != 0) {
                            snprintf(err, err_len, T("Die Konsole konnte das neue Abbild nicht formatieren (Antwort 0x%x)."), (unsigned)r);
                            unlink(path);
                        } else {
                            sync();
                            rc = 0;
                        }
                    }
                }
                memset(key, 0, sizeof(key));
            }
        }
        memset(buf, 0, 0x100);
        free(buf);
    }
    plat_save_service_leave();
    return rc;
}

/* ── der PS5PARAM-Blob ── */

#define PFSCTL_PARAM_READ  0xC0189111u
#define PFSCTL_PARAM_READ2 0xC0209116u

typedef struct {
    const char *path;
    void       *buf;
    uint32_t    size;
    uint32_t    pad;
    uint64_t   *out;
} pfs_param_arg_t;

/* in der Reihenfolge der Werkzeuge der Konsole; /dev/pfsctldev hat Modus 0400 und wird erst nur lesend geöffnet */
static const char *const k_param_devs[] = { "/dev/pfsctl", "/dev/pfsmgr", "/dev/devpfs.ctl", "/dev/pfs", "/dev/pfsctldev", NULL };

static int blob_ok(const uint8_t *b) {
    uint64_t magic = 0;
    for (int i = 7; i >= 0; i--) magic = (magic << 8) | b[i];
    return magic == SAVEMNT_PARAM_MAGIC;
}

static void diag_add(char *d, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void diag_add(char *d, size_t cap, const char *fmt, ...) {
    const size_t n = strlen(d);
    if (n + 2 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d + n, cap - n, fmt, ap);
    va_end(ap);
}

int savemnt_param_read(const char *image, int already_elevated, uint8_t *sfo, uint32_t sfo_cap, uint32_t *sfo_len, char *err, size_t err_len) {
    if (err_len) err[0] = '\0';
    *sfo_len = 0;
    uint8_t *blob = calloc(1, SAVEMNT_PARAM_BYTES);
    if (!blob) { snprintf(err, err_len, "%s", T("Kein Speicher")); return -2; }
    if (!already_elevated && enter(err, err_len) != 0) { free(blob); return -2; }
    static const unsigned cmds[2] = { PFSCTL_PARAM_READ, PFSCTL_PARAM_READ2 };
    char diag[420];
    diag[0] = '\0';
    int found = 0, opened = 0, soft = 0, last_errno = 0;
    for (int d = 0; k_param_devs[d] && !found; d++) {
        int fd = open(k_param_devs[d], O_RDONLY);   /* ioctl geht bei jedem Öffnungsmodus; manche Knoten lehnen O_RDWR ab */
        if (fd < 0 && errno == EACCES) fd = open(k_param_devs[d], O_RDWR);
        if (fd < 0) {
            const int oe = errno;
            diag_add(diag, sizeof(diag), " %s:open=%d", k_param_devs[d] + 5, oe);
            if (!last_errno) last_errno = oe;
            continue;
        }
        opened = 1;
        for (int c = 0; c < 2 && !found; c++) {
            uint64_t outv = 0;
            pfs_param_arg_t a;
            memset(&a, 0, sizeof(a));
            memset(blob, 0, SAVEMNT_PARAM_BYTES);
            a.path = image;
            a.buf = blob;
            a.size = SAVEMNT_PARAM_BYTES;
            a.out = &outv;
            errno = 0;
            const int r = ioctl(fd, cmds[c], &a);
            const int ie = errno;
            if (r < 0) {
                diag_add(diag, sizeof(diag), " %s:%x=%d", k_param_devs[d] + 5, cmds[c] & 0xffff, ie);
                last_errno = ie ? ie : EIO;
                continue;
            }
            if (blob_ok(blob)) found = 1;
            else { soft = 1; diag_add(diag, sizeof(diag), " %s:%x=nomagic", k_param_devs[d] + 5, cmds[c] & 0xffff); }
        }
        close(fd);
    }
    if (!already_elevated) plat_save_service_leave();
    if (!found) {
        free(blob);
        if (soft) return -1;
        snprintf(err, err_len, opened ? T("Die Konsole beantwortet die Frage nach den Metadaten des Spielstands nicht (Fehler %d;%s)") :
                                        T("Kein Gerät der Konsole für die Metadaten des Spielstands ließ sich öffnen (Fehler %d;%s)"), last_errno, diag);
        return -2;
    }
    uint32_t ver = 0, size = 0, off = 0;
    memcpy(&ver, blob + 8, 4);
    memcpy(&size, blob + 12, 4);
    memcpy(&off, blob + 16, 4);
    if (ver != 0 || size == 0 || size > sfo_cap || off < 20 || (uint64_t)off + size > SAVEMNT_PARAM_BYTES) {
        free(blob);
        snprintf(err, err_len, T("Die Metadaten des Spielstands haben ein unbekanntes Format (Version %u, Größe %u)."), (unsigned)ver, (unsigned)size);
        return -2;
    }
    memcpy(sfo, blob + off, size);
    *sfo_len = size;
    free(blob);
    return 0;
}

#endif
