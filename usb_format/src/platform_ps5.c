/* PS5: USB-Massenspeicher sind /dev/daN (umass/CAM), Partitionen /dev/daNsM bzw. /dev/daNpM. */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "extfs.h"
#include "fsdetect.h"
#include "geom_xml.h"
#include "platform.h"
#include "smpapi.h"

#define DATA_DIR "/data/usb_format"
#define TOOL_DIR DATA_DIR "/bin"

/* mkntfs (ntfs-3g) und mke2fs (e2fsprogs), für die PS5 übersetzt (tools/build_fstools.sh), werden eingebettet
 * und bei Bedarf nach TOOL_DIR geschrieben. Sie laufen über den ELF-Loader als eigener Prozess: Bei Fehlern
 * beenden sie sich mit exit(), was den Formatierer sonst mitreißen würde. */
__asm__(".section .rodata\n"
        ".global tool_mkntfs\n"
        "tool_mkntfs:\n"
        ".incbin \"tools/bin/mkntfs.elf\"\n"
        ".global tool_mkntfs_end\n"
        "tool_mkntfs_end:\n"
        ".global tool_mke2fs\n"
        "tool_mke2fs:\n"
        ".incbin \"tools/bin/mke2fs.elf\"\n"
        ".global tool_mke2fs_end\n"
        "tool_mke2fs_end:\n"
        ".previous\n");
extern const unsigned char tool_mkntfs[], tool_mkntfs_end[], tool_mke2fs[], tool_mke2fs_end[];

#define LOADER_PORT 9021

typedef struct {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

void plat_notify(const char *msg) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void plat_throttle(void) {}

const char *plat_data_dir(void) { return DATA_DIR; }

/* Symbol aus einer Sony-Bibliothek suchen statt fest binden: Fehlt eines unter dem erwarteten Namen, bricht der
 * Lader sonst schon vor main() ab und der Payload stirbt ohne Meldung. Die Bibliothek bleibt gelinkt (geladen),
 * gesucht wird zur Laufzeit auf vier Wegen (wie im PS5 Temperatur Studio, dynsym.c). */
static void *dynsym(const char *module, const char *symbol) {
    void *mod = dlopen(module, RTLD_LAZY);  /* bereits geladen -> liefert nur das Handle */
    void *addr = mod ? dlsym(mod, symbol) : NULL;
    if (addr) return addr;

    pid_t pid = getpid();
    uint32_t handle = 0;
    kernel_dynlib_handle(pid, module, &handle);
    if (!handle) return NULL;
    intptr_t found = kernel_dynlib_dlsym(pid, handle, symbol);
    if (found > 0) return (void *)found;
    char nid[12];
    nid_encode(symbol, nid);
    found = kernel_dynlib_resolve(pid, handle, nid);
    return found > 0 ? (void *)found : NULL;
}

int plat_open_browser(const char *url, char *err, size_t errlen) {
    typedef int (*fn_usinit_t)(void *);
    typedef int (*fn_browser_t)(const char *, void *);
    static int user_ready;

    fn_browser_t browser = (fn_browser_t)dynsym("libSceSystemService.sprx", "sceSystemServiceLaunchWebBrowser");
    if (!browser) {
        snprintf(err, errlen, "sceSystemServiceLaunchWebBrowser nicht gefunden");
        return -1;
    }
    if (!user_ready) {
        fn_usinit_t init = (fn_usinit_t)dynsym("libSceUserService.sprx", "sceUserServiceInitialize");
        if (init) init(NULL);
        user_ready = 1;
    }
    int rc = browser(url, NULL);
    if (rc != 0) {
        snprintf(err, errlen, "Browser ließ sich nicht öffnen (0x%08x)", (unsigned)rc);
        return -1;
    }
    return 0;
}

static int is_whole_disk(const char *n) {
    if (strncmp(n, "da", 2) != 0 || !n[2]) return 0;
    for (n += 2; *n; n++)
        if (*n < '0' || *n > '9') return 0;
    return 1;
}

/* "/dev/da0s1" gehört zu "da0", "/dev/da10" nicht */
static int belongs_to(const char *from, const char *dev) {
    size_t n = strlen(dev);
    if (strncmp(from, "/dev/", 5) != 0) return 0;
    from += 5;
    if (strncmp(from, dev, n) != 0) return 0;
    return from[n] == '\0' || from[n] == 's' || from[n] == 'p';
}

/* Hersteller/Modell aus GEOM (kern.geom.confxml, Klasse DISK, Feld <descr>).
 * Der da-Treiber füllt es aus der SCSI-Inquiry; das Laufwerk selbst wird dafür nicht angesprochen. */
static void fill_models(usbdev_t *devs, int n) {
    size_t len = 0;
    if (n <= 0 || sysctlbyname("kern.geom.confxml", NULL, &len, NULL, 0) != 0 || !len) return;
    len += 8192;  /* kann zwischen den beiden Aufrufen wachsen */
    char *xml = malloc(len + 1);
    if (!xml) return;
    if (sysctlbyname("kern.geom.confxml", xml, &len, NULL, 0) == 0) {
        xml[len] = 0;
        for (int i = 0; i < n; i++)
            geom_disk_descr(xml, devs[i].name, devs[i].model, sizeof(devs[i].model));
    }
    free(xml);
}

/* Eingebundene Dateisysteme mit eigenem Puffer (getmntinfo() teilt einen statischen - nicht thread-sicher,
 * und Bibliothek-Suche, Aufträge und Anfragen fragen gleichzeitig). Aufrufer gibt *out frei. */
static int mounts_get(struct statfs **out) {
    *out = NULL;
    for (int tries = 0; tries < 3; tries++) {
        int n = getfsstat(NULL, 0, MNT_NOWAIT);
        if (n <= 0) return 0;
        n += 4;
        struct statfs *buf = calloc((size_t)n, sizeof(*buf));
        if (!buf) return 0;
        int got = getfsstat(buf, (long)((size_t)n * sizeof(*buf)), MNT_NOWAIT);
        if (got >= 0 && got < n) { *out = buf; return got; }
        free(buf);   /* inzwischen mehr geworden: neu zählen */
    }
    return 0;
}

int plat_scan(usbdev_t *out, int max) {
    struct statfs *mnt = NULL;
    int nmnt = mounts_get(&mnt);
    DIR *dir = opendir("/dev");
    struct dirent *e;
    int n = 0;

    if (!dir) { free(mnt); return -1; }
    while ((e = readdir(dir)) && n < max) {
        if (!is_whole_disk(e->d_name)) continue;

        usbdev_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%s", e->d_name);
        snprintf(d->path, sizeof(d->path), "/dev/%s", e->d_name);

        int fd = open(d->path, O_RDONLY);
        if (fd < 0) continue;
        off_t media = 0;
        u_int sector = 0;
        int ok = ioctl(fd, DIOCGMEDIASIZE, &media) == 0 && ioctl(fd, DIOCGSECTORSIZE, &sector) == 0;
        if (ok && media > 0 && sector >= 512 && sector <= 4096) {
            fsinfo_t fi;   /* nur lesen, auch eingebunden */
            fs_probe(fd, sector, (uint64_t)media, &fi);
            snprintf(d->format, sizeof(d->format), "%s", fi.fs);
            snprintf(d->label, sizeof(d->label), "%s", fi.label);
            snprintf(d->scheme, sizeof(d->scheme), "%s", fi.scheme);
        }
        close(fd);
        if (!ok || media <= 0 || sector < 512 || sector > 4096) continue;  /* kein Medium */
        d->size = (uint64_t)media;
        d->ss = sector;

        for (int i = 0; i < nmnt; i++) {
            if (!belongs_to(mnt[i].f_mntfromname, d->name)) continue;
            if (strncmp(mnt[i].f_mntonname, "/mnt/usb", 8) != 0) d->locked = 1;
            if (d->nmounts < MAX_MOUNTS) {
                mountinfo_t *m = &d->mounts[d->nmounts++];
                snprintf(m->path, sizeof(m->path), "%s", mnt[i].f_mntonname);
                snprintf(m->fstype, sizeof(m->fstype), "%s", mnt[i].f_fstypename);
            }
        }
        n++;
    }
    closedir(dir);
    free(mnt);
    fill_models(out, n);
    return n;
}

/* Bewusst kein eigenes Aushängen: Die PS5 spiegelt /mnt/usbN per nullfs in die Sandboxen von System-Apps.
 * Ein erzwungenes unmount() ließ diese Spiegel ins Leere zeigen, danach starteten Spiele/Apps nicht mehr
 * (CE-108255-1) bis zum Neustart. Formatiert wird nur, was die PS5 nicht eingebunden hat; eingebundene Sticks
 * gibt sie beim Abziehen selbst frei (plat_wait_replug). Ihre eigenen Freigabe-Funktionen
 * (sceUsbStorageRequestUnmap, sceShellCoreUtilRequestEjectDevice) lehnt der USB-Dienst für Payloads ab
 * (0x80f40002). */
int plat_open_rw(const usbdev_t *d, char *err, size_t errlen) {
    /* O_EXCL: GEOM verweigert das Öffnen, solange noch ein Dateisystem darauf hängt */
    int fd = open(d->path, O_RDWR | O_EXCL);
    if (fd < 0)
        snprintf(err, errlen, "%s kann nicht geöffnet werden (%s). Ist der Stick noch in Benutzung?",
                 d->path, strerror(errno));
    return fd;
}

/* Nur lesen geht auch, während die PS5 den Datenträger nutzt (Tempo messen) */
int plat_open_ro(const usbdev_t *d, char *err, size_t errlen) {
    int fd = open(d->path, O_RDONLY);
    if (fd < 0) snprintf(err, errlen, "%s kann nicht gelesen werden (%s)", d->path, strerror(errno));
    return fd;
}

/* Wie die Standard-Suchorte von ShadowMountPlus (USB-Laufwerke kommen aus plat_scan dazu) */
const char *const *plat_library_roots(void) {
    static const char *const roots[] = {"/data/homebrew", "dir", "/data/etaHEN/games", "dir",
                                        "/mnt/ext0", "usb", "/mnt/ext1", "usb", NULL};
    return roots;
}

/* ------------------------------------------------------------ Eingebundene Sticks: abziehen, einstecken */

#define REPLUG_WAIT_MS  (120 * 1000)
#define REPLUG_UNITS    32
#define REPLUG_ATTEMPTS 3

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int dev_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int unit_mounted(const char *name) {
    struct statfs *mnt = NULL;
    int n = mounts_get(&mnt), found = 0;
    for (int i = 0; i < n && !found; i++)
        found = belongs_to(mnt[i].f_mntfromname, name);
    free(mnt);
    return found;
}

/* Öffnet ein gerade erschienenes Gerät exklusiv. Die PS5 bindet die Partition danach nicht mehr ein: Das Einbinden
 * braucht selbst exklusiven Zugriff auf das Gerät darunter. -2 = die PS5 hat es schon eingebunden. */
static int grab(const char *path, const char *name) {
    for (int i = 0; i < 2000; i++) {  /* bis 2 s, falls der Knoten vor dem Gerät fertig ist */
        int fd = open(path, O_RDWR | O_EXCL);
        if (fd >= 0) return fd;
        if (unit_mounted(name)) return -2;
        if (!dev_exists(path)) return -1;
        usleep(1000);
    }
    return -1;
}

/* ---- Software-Abziehen: den Stick per USB_DEVICEENUMERATE neu anmelden lassen. Für die PS5 ist das wie Abziehen
 * und Einstecken (Kernel: "disconnected"/"detached", AutoMounter räumt Einbindung und Spiegel selbst ab). Angefasst
 * wird nur ein USB-Massenspeicher (Treiber umass), dessen Seriennummer eindeutig zu der des Laufwerks passt -
 * WLAN-Chip, Controller oder eine andere Platte nie. */

#define SOFT_BACK_MS 15000   /* so lange darf das Neu-Anmelden dauern, sonst von Hand abziehen */

static int disk_ident(const char *name, char *out, size_t n) {
    size_t len = 0;
    int ok = 0;
    out[0] = 0;
    if (sysctlbyname("kern.geom.confxml", NULL, &len, NULL, 0) != 0 || !len) return 0;
    len += 8192;
    char *xml = malloc(len + 1);
    if (!xml) return 0;
    if (sysctlbyname("kern.geom.confxml", xml, &len, NULL, 0) == 0) {
        xml[len] = 0;
        ok = geom_disk_ident(xml, name, out, n);
    }
    free(xml);
    return ok;
}

static int usb_mass_storage(int fd) {
    char drv[64] = "";
    struct usb_gen_descriptor gd;
    memset(&gd, 0, sizeof(gd));
    gd.ugd_data = drv;
    gd.ugd_maxlen = sizeof(drv) - 1;
    return ioctl(fd, USB_GET_IFACE_DRIVER, &gd) == 0 && strncmp(drv, "umass", 5) == 0;
}

static int usb_serial(int fd, char *out, size_t n) {
    struct usb_device_descriptor dd;
    static unsigned char buf[256];   /* nicht auf den Stapel (64-KiB-Threads) */
    struct usb_gen_descriptor gd;
    out[0] = 0;
    if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) != 0 || !dd.iSerialNumber) return -1;
    memset(buf, 0, sizeof(buf));
    memset(&gd, 0, sizeof(gd));
    gd.ugd_data = buf;
    gd.ugd_maxlen = sizeof(buf);
    gd.ugd_lang_id = 0x0409;
    gd.ugd_string_index = dd.iSerialNumber;
    if (ioctl(fd, USB_GET_STRING_DESC, &gd) != 0) return -1;
    size_t o = 0;
    for (int i = 2; i + 1 < buf[0] && o + 1 < n; i += 2) out[o++] = buf[i + 1] ? '?' : (char)buf[i];
    out[o] = 0;
    return 0;
}

/* 0 = Neu-Anmeldung ausgelöst; -1 = nicht möglich (dann von Hand abziehen) */
static int soft_unplug(const char *disk) {
    char ident[96], node[64] = "", ser[96], p[64];
    int matches = 0;
    if (!disk_ident(disk, ident, sizeof(ident)) || strlen(ident) < 4) return -1;
    DIR *dir = opendir("/dev");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        if (strncmp(e->d_name, "ugen", 4) != 0) continue;
        snprintf(p, sizeof(p), "/dev/%s", e->d_name);
        int fd = open(p, O_RDWR);
        if (fd < 0) continue;
        if (usb_mass_storage(fd) && usb_serial(fd, ser, sizeof(ser)) == 0 && strcmp(ser, ident) == 0) {
            snprintf(node, sizeof(node), "%s", p);
            matches++;
        }
        close(fd);
    }
    if (dir) closedir(dir);
    if (matches != 1) return -1;   /* keine oder doppelte Seriennummer: lieber von Hand */
    int fd = open(node, O_RDWR), arg = 0;
    if (fd < 0) return -1;
    int rc = ioctl(fd, USB_DEVICEENUMERATE, &arg);
    close(fd);
    return rc == 0 ? 0 : -1;
}

/* Gerätezustand je daN: Nach dem Neu-Anmelden kann der Stick binnen Millisekunden unter demselben Namen
 * wieder da sein - erkannt wird er dann an der neuen Identität des Geräteknotens */
typedef struct { int exists; ino_t ino; dev_t rdev; } node_t;

static void node_get(const char *path, node_t *n) {
    struct stat st;
    n->exists = stat(path, &st) == 0;
    n->ino = n->exists ? st.st_ino : 0;
    n->rdev = n->exists ? st.st_rdev : 0;
}

static void snapshot(node_t *snap) {
    char path[32];
    for (int k = 0; k < REPLUG_UNITS; k++) {
        snprintf(path, sizeof(path), "/dev/da%d", k);
        node_get(path, &snap[k]);
    }
}

int plat_wait_replug(const usbdev_t *d, usbdev_t *out, progress_fn progress, volatile int *cancel,
                     char *err, size_t errlen) {
    char cur[sizeof(d->path)], path[32], name[16];
    node_t snap[REPLUG_UNITS], now;
    int soft = 1;   /* zuerst per Software; geht das nicht, zieht der Benutzer ab */
    snprintf(cur, sizeof(cur), "%s", d->path);

    for (int attempt = 0; attempt < REPLUG_ATTEMPTS; attempt++) {
        /* 1. Freigeben: Die PS5 gibt den Stick beim (Software-)Abziehen selbst frei, samt ihrer Spiegel */
        if (soft) {
            progress("Stick wird per Software neu angemeldet", 0);
            snapshot(snap);
            if (strncmp(cur, "/dev/", 5) != 0 || soft_unplug(cur + 5) != 0) soft = 0;
        }
        if (!soft) {
            progress(attempt ? "Die PS5 war schneller - Stick bitte noch einmal abziehen" : "Stick jetzt abziehen", 0);
            long t0 = now_ms();
            while (dev_exists(cur)) {
                if (*cancel) { snprintf(err, errlen, "Abgebrochen."); return -1; }
                if (now_ms() - t0 > REPLUG_WAIT_MS) {
                    snprintf(err, errlen, "Der Stick wurde nicht abgezogen (2 Minuten gewartet).");
                    return -1;
                }
                usleep(50 * 1000);
            }
            snapshot(snap);
            progress("Stick jetzt wieder einstecken", 0);
        }

        /* 2. Wieder da: jedes neu erschienene daN sofort exklusiv öffnen, passende Größe = unser Stick */
        long t0 = now_ms();
        int lost = 0, fallback = 0;
        while (!lost && !fallback) {
            if (*cancel) { snprintf(err, errlen, "Abgebrochen."); return -1; }
            if (soft && now_ms() - t0 > SOFT_BACK_MS) { fallback = 1; break; }
            if (!soft && now_ms() - t0 > REPLUG_WAIT_MS) {
                snprintf(err, errlen, "Der Stick wurde nicht wieder eingesteckt (2 Minuten gewartet).");
                return -1;
            }
            for (int k = 0; k < REPLUG_UNITS && !lost; k++) {
                snprintf(path, sizeof(path), "/dev/da%d", k);
                node_get(path, &now);
                if (!now.exists) { snap[k].exists = 0; continue; }
                if (snap[k].exists && snap[k].ino == now.ino && snap[k].rdev == now.rdev) continue;
                snprintf(name, sizeof(name), "da%d", k);
                int fd = grab(path, name);
                if (fd == -2) {          /* die PS5 war schneller: noch einmal freigeben lassen - wenn es unser Stick ist */
                    off_t media = 0;
                    u_int sector = 0;
                    int ro = open(path, O_RDONLY);
                    int ours = ro >= 0 && ioctl(ro, DIOCGMEDIASIZE, &media) == 0 &&
                               ioctl(ro, DIOCGSECTORSIZE, &sector) == 0 && (uint64_t)media == d->size && sector == d->ss;
                    if (ro >= 0) close(ro);
                    snap[k] = now;
                    if (!ours) continue;   /* ein anderer Stick wurde eingebunden: in Ruhe lassen */
                    snprintf(cur, sizeof(cur), "%s", path);
                    lost = 1;
                    break;
                }
                snap[k] = now;           /* diese Instanz nicht noch einmal versuchen */
                if (fd < 0) continue;
                off_t media = 0;
                u_int sector = 0;
                if (ioctl(fd, DIOCGMEDIASIZE, &media) != 0 || ioctl(fd, DIOCGSECTORSIZE, &sector) != 0 ||
                    (uint64_t)media != d->size || sector != d->ss || unit_mounted(name)) {
                    close(fd);           /* anderes Gerät */
                    continue;
                }
                memset(out, 0, sizeof(*out));
                snprintf(out->name, sizeof(out->name), "%s", name);
                snprintf(out->path, sizeof(out->path), "%s", path);
                out->size = (uint64_t)media;
                out->ss = sector;
                fill_models(out, 1);
                return fd;
            }
            usleep(1000);
        }
        if (fallback) {   /* Software-Neuanmeldung hat nicht gegriffen: derselbe Versuch von Hand */
            soft = 0;
            attempt--;
        }
    }
    snprintf(err, errlen, "Die PS5 hat den Stick %d-mal schneller eingebunden. Bitte erneut versuchen.",
             REPLUG_ATTEMPTS);
    return -1;
}

/* ------------------------------------------------------------ NTFS/ext4 über mkntfs/mke2fs */

/* Werkzeug ablegen, falls es fehlt oder sich unterscheidet */
static int ensure_tool(const char *path, const unsigned char *data, size_t len, char *err, size_t errlen) {
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size == len) {
        FILE *f = fopen(path, "rb");
        static unsigned char buf[65536];   /* nicht auf den Stapel: Threads haben auf der PS5 nur 64 KiB */
        size_t off = 0, n;
        int same = f != NULL;
        while (same && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
            same = off + n <= len && memcmp(buf, data + off, n) == 0;
            off += n;
        }
        if (f) fclose(f);
        if (same && off == len) return 0;
    }
    mkdir(DATA_DIR, 0755);
    mkdir(TOOL_DIR, 0755);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(err, errlen, "%s ließ sich nicht ablegen: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* Argumente für den ELF-Loader: Leerzeichen/Backslash mit '\' schützen (wird dort so getrennt),
 * das Ganze dann URL-kodieren (die Zeile wird dort dekodiert) */
static void loader_args(const extfs_cmd_t *c, char *out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (int i = 1; i < c->argc && o + 4 < n; i++) {
        char arg[512];
        size_t a = 0;
        if (i > 1) arg[a++] = ' ';
        for (const char *s = c->argv[i]; *s && a + 3 < sizeof(arg); s++) {
            if (*s == ' ' || *s == '\\') arg[a++] = '\\';
            arg[a++] = *s;
        }
        for (size_t k = 0; k < a && o + 4 < n; k++) {
            unsigned char ch = (unsigned char)arg[k];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ',' || ch == '=') {
                out[o++] = (char)ch;
            } else {
                out[o++] = '%';
                out[o++] = hex[ch >> 4];
                out[o++] = hex[ch & 15];
            }
        }
    }
    out[o] = 0;
}

int plat_external_mkfs(const usbdev_t *d, int fs, const char *label, uint32_t cluster, uint64_t start,
                       uint64_t count, int gpt, progress_fn progress, char *summary, size_t sumlen,
                       char *err, size_t errlen) {
    const char *tool = extfs_tool(fs);
    const unsigned char *data = fs == FS_NTFS ? tool_mkntfs : tool_mke2fs;
    size_t len = (size_t)((fs == FS_NTFS ? tool_mkntfs_end : tool_mke2fs_end) - data);
    char part[64], path[128], args[2048], line[512];
    struct stat st;
    (void)count;

    /* Nach dem Schließen liest GEOM die neue Partitionstabelle ein und legt /dev/daNs1 (MBR) bzw. daNp1 (GPT) an */
    snprintf(part, sizeof(part), "%s%s", d->path, gpt ? "p1" : "s1");
    progress("Partition suchen", 0);
    for (int i = 0; i < 50 && stat(part, &st) != 0; i++) usleep(200 * 1000);
    if (stat(part, &st) != 0) {
        snprintf(err, errlen, "Die neue Partition %s ist nicht erschienen. Stick abziehen, neu einstecken und erneut "
                 "versuchen.", part);
        return -1;
    }

    snprintf(path, sizeof(path), TOOL_DIR "/%s.elf", tool);
    if (ensure_tool(path, data, len, err, errlen) != 0) return -1;

    extfs_cmd_t cmd;
    if (extfs_build(&cmd, fs, part, label, cluster, d->ss, start) != 0) {
        snprintf(err, errlen, "Unbekanntes Dateisystem");
        return -1;
    }
    loader_args(&cmd, args, sizeof(args));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(LOADER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        if (s >= 0) close(s);
        snprintf(err, errlen, "ELF-Loader (Port %d) nicht erreichbar – läuft der Jailbreak noch?", LOADER_PORT);
        return -1;
    }
    struct timeval tv = {300, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[2400];
    int rl = snprintf(req, sizeof(req), "file:%s?args=%s\n", path, args);
    if (send(s, req, (size_t)rl, 0) != rl) {
        close(s);
        snprintf(err, errlen, "%s ließ sich nicht starten", tool);
        return -1;
    }
    shutdown(s, SHUT_WR);   /* Rückfragen des Werkzeugs bekommen sofort "Ende der Eingabe" */

    /* Ausgabe sammeln (Ende = Werkzeug beendet) und für den Fortschritt zeilenweise auswerten */
    static char out[65536];
    size_t olen = 0, ll = 0;
    progress(fs == FS_NTFS ? "NTFS schreiben (mkntfs)" : "ext4 schreiben (mke2fs)", 5);
    for (;;) {
        char buf[1024];
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (olen + 1 < sizeof(out)) out[olen++] = buf[i];
            char ch = buf[i];
            if (ch == '\n' || ch == '\r' || ch == '\b') {
                line[ll] = 0;
                int pct;
                const char *step = ll ? extfs_step(fs, line, &pct) : NULL;
                if (step) progress(step, pct);
                ll = 0;
            } else if (ll + 1 < sizeof(line)) {
                line[ll++] = ch;
            }
        }
    }
    close(s);
    out[olen] = 0;

    if (!extfs_succeeded(fs, out)) {
        /* Letzte sinnvolle Zeile als Fehlermeldung */
        char last[200] = "";
        for (char *p = strtok(out, "\r\n\b"); p; p = strtok(NULL, "\r\n\b"))
            if (strlen(p) > 3) snprintf(last, sizeof(last), "%s", p);
        snprintf(err, errlen, "%s ist fehlgeschlagen%s%s", tool, last[0] ? ": " : "", last);
        return -1;
    }

    /* Gegenprobe: Kopf des neuen Dateisystems lesen */
    static unsigned char head[4096] __attribute__((aligned(4096)));
    int fd = open(part, O_RDONLY);
    int ok = fd >= 0 && pread(fd, head, sizeof(head), 0) == (ssize_t)sizeof(head) && extfs_signature_ok(fs, head);
    if (fd >= 0) close(fd);
    if (!ok) {
        snprintf(err, errlen, "%s meldete Erfolg, aber auf %s steht kein gültiges Dateisystem", tool, part);
        return -1;
    }
    extfs_summary(fs, cluster, out, summary, sumlen);
    return 0;
}

/* ------------------------------------------------------------ Titel-IDs von ShadowMountPlus */

#define SMP_API_PORT 10101
#define SMP_MAX      (8 * 1024 * 1024)

static char *slurp_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    *len = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        rewind(f);
        if (n > 0 && n < SMP_MAX && (buf = malloc((size_t)n + 1))) { *len = fread(buf, 1, (size_t)n, f); buf[*len] = 0; }
    }
    fclose(f);
    return buf;
}

char *plat_smp_post(const char *path, const char *body, int *http, size_t *len) {
    return smp_http_post(SMP_API_PORT, path, body, http, len);
}

void plat_library_hints(lib_hint_t **h, int *n) {
    static const char *const logs[] = {"/data/shadowmount/debug.log.1", "/data/shadowmount/debug.log"};
    size_t len;
    char *buf;
    *h = NULL;
    *n = 0;
    for (int i = 0; i < 2; i++)   /* älteres Protokoll zuerst: der jüngste Eintrag zu einem Pfad gewinnt */
        if ((buf = slurp_file(logs[i], &len))) { lib_parse_smp_log(buf, len, h, n); free(buf); }
    int http;
    if ((buf = plat_smp_post("/api/v1/games", "{}", &http, &len))) {
        if (http == 200) lib_parse_smp_games(buf, len, h, n);
        free(buf);
    }
}

void plat_local_ip(char *buf, size_t len) {
    struct ifaddrs *ifa = NULL;
    snprintf(buf, len, "PS5-IP");
    if (getifaddrs(&ifa) != 0) return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)i->ifa_addr;
        if (ntohl(sin->sin_addr.s_addr) >> 24 == 127) continue;
        inet_ntop(AF_INET, &sin->sin_addr, buf, (socklen_t)len);
        break;
    }
    freeifaddrs(ifa);
}
