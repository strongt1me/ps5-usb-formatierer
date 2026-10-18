/* usbfmt_linux - Linux-Teil des PC-Helfers (Ablauf und Befehle: ../pc/helper.c): findet USB-Datenträger (ohne
 * Root-Rechte: /sys, /proc und die udev-Datenbank), hängt ihre Laufwerke aus und öffnet sie exklusiv, schreibt NTFS,
 * FAT16 und ext2/3/4 mit den Werkzeugen des Systems (mkntfs, mkfs.fat, mke2fs) und benennt NTFS/FAT16 mit ntfslabel
 * bzw. fatlabel um. Das Programm startet ihn zum Formatieren über pkexec (siehe windows/pc_linux.py).
 *
 *   usbfmt_linux [--out F] [--cancel F] [--lang de|en|it|es|fr] list|format|layout|speed|label ...
 *
 * Datenträger heißen wie unter /dev ("sdb", "mmcblk0"). Nur USB- und SD-Datenträger, nie einer mit dem laufenden
 * System (/, /boot, /usr, /home, Auslagerung - auch über LVM/RAID). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* build.sh setzt es für alle Dateien (linuxcompat.h steht davor) */
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/ioctl.h>
#include <sys/mount.h>   /* auch BLKGETSIZE64, BLKSSZGET, BLKRRPART, BLKFLSBUF, BLKROGET (linux/fs.h verträgt sich mit
                          * sys/mount.h in älteren glibc nicht) */
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../extfs.h"
#include "../i18n.h"
#include "../mkfs.h"
#include "../vollabel.h"
#include "../pc/helper.h"

#define MAX_PARTS_SCAN 64

static const char *cancel_path;
static const char *image_path;   /* nur Test-Build: Abbild-Datei statt Datenträger */

/* ------------------------------------------------------------------ Hilfen */

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

/* eine Zeile aus einer (sys-)Datei, ohne Zeilenende und Leerzeichen am Rand */
static int read_line(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "re");
    if (!f) { buf[0] = 0; return -1; }
    if (!fgets(buf, (int)n, f)) buf[0] = 0;
    fclose(f);
    size_t l = strlen(buf);
    while (l && isspace((unsigned char)buf[l - 1])) buf[--l] = 0;
    size_t s = 0;
    while (buf[s] == ' ') s++;
    if (s) memmove(buf, buf + s, l - s + 1);
    return 0;
}

static unsigned long long read_num(const char *path) {
    char b[64];
    return read_line(path, b, sizeof(b)) ? 0 : strtoull(b, NULL, 10);
}

/* Gerätename nur aus a-z, 0-9 (nie "/" oder "..") */
static int valid_name(const char *s) {
    if (!*s || strlen(s) > 24) return 0;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* Name der Partition index (0-basiert): sdb -> sdb1, mmcblk0/nvme0n1/loop0 -> mmcblk0p1 */
static void part_name(const char *disk, int index, char *buf, size_t n) {
    size_t l = strlen(disk);
    snprintf(buf, n, "%s%s%d", disk, l && isdigit((unsigned char)disk[l - 1]) ? "p" : "", index + 1);
}

static void sys_error(char *buf, size_t n, const char *what, int e) {
    snprintf(buf, n, "%s: %s", T(what), strerror(e));
}

/* ------------------------------------------------------------------ Datenträger finden */

typedef struct {
    char     name[32];     /* sdb1 */
    unsigned maj, min;
    char     mount[256];   /* erster Einhängepunkt oder leer */
    char     fs[16], label[64];
    int      fat16;        /* vfat mit FAT12/FAT16 */
    int      holders;      /* von LVM/RAID/dm belegt */
    uint64_t total, free;
} partinfo_t;

/* Einhängepunkte: \040 usw. zurückwandeln */
static void unescape_mount(char *s) {
    char *w = s;
    for (char *r = s; *r; ) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 4;
        } else *w++ = *r++;
    }
    *w = 0;
}

/* erster Einhängepunkt des Geräts maj:min (aus /proc/self/mountinfo) */
static int mount_of(unsigned maj, unsigned min, char *mp, size_t n) {
    FILE *f = fopen("/proc/self/mountinfo", "re");
    if (!f) return 0;
    char line[4096];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        unsigned a, b;
        char point[2048];
        if (sscanf(line, "%*s %*s %u:%u %*s %2047s", &a, &b, point) != 3) continue;
        if (a == maj && b == min) {
            unescape_mount(point);
            snprintf(mp, n, "%s", point);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* Bezeichnung aus der udev-Datenbank: ID_FS_LABEL_ENC enthält \xNN-Folgen */
static void udev_unescape(const char *in, char *out_, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; ) {
        if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])) {
            char hex[3] = {p[2], p[3], 0};
            out_[o++] = (char)strtol(hex, NULL, 16);
            p += 4;
        } else out_[o++] = *p++;
    }
    out_[o] = 0;
}

/* Dateisystem und Bezeichnung, wie udev sie ermittelt hat (lesbar ohne Root) */
static void udev_fs(partinfo_t *p) {
    char path[64], line[512], type[32] = "", version[32] = "", label[192] = "";
    snprintf(path, sizeof(path), "/run/udev/data/b%u:%u", p->maj, p->min);
    FILE *f = fopen(path, "re");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "E:ID_FS_TYPE=", 13)) snprintf(type, sizeof(type), "%s", line + 13);
        else if (!strncmp(line, "E:ID_FS_VERSION=", 16)) snprintf(version, sizeof(version), "%s", line + 16);
        else if (!strncmp(line, "E:ID_FS_LABEL_ENC=", 18)) udev_unescape(line + 18, label, sizeof(label));
    }
    fclose(f);
    snprintf(p->label, sizeof(p->label), "%s", label);
    p->fat16 = !strcmp(type, "vfat") && (!strcmp(version, "FAT16") || !strcmp(version, "FAT12"));
    const char *shown = !strcmp(type, "vfat") ? (p->fat16 ? "FAT" : "FAT32") : !strcmp(type, "exfat") ? "exFAT"
                      : !strcmp(type, "ntfs") || !strcmp(type, "ntfs3") ? "NTFS" : !strcmp(type, "hfsplus") ? "HFS+"
                      : type;
    snprintf(p->fs, sizeof(p->fs), "%s", shown);
}

/* Partitionen des Datenträgers (aus /sys/block/<disk>/) */
static int disk_parts(const char *disk, partinfo_t *v, int max) {
    char dir[96];
    snprintf(dir, sizeof(dir), "/sys/block/%s", disk);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        char path[192], devno[32];
        if (strncmp(e->d_name, disk, strlen(disk)) || !valid_name(e->d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s/partition", dir, e->d_name);
        if (access(path, F_OK)) continue;
        partinfo_t *p = &v[n];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", e->d_name);
        snprintf(path, sizeof(path), "%s/%s/dev", dir, e->d_name);
        if (read_line(path, devno, sizeof(devno)) || sscanf(devno, "%u:%u", &p->maj, &p->min) != 2) continue;
        snprintf(path, sizeof(path), "%s/%s/holders", dir, e->d_name);
        DIR *h = opendir(path);
        if (h) {
            struct dirent *x;
            while ((x = readdir(h))) if (x->d_name[0] != '.') p->holders = 1;
            closedir(h);
        }
        udev_fs(p);
        if (mount_of(p->maj, p->min, p->mount, sizeof(p->mount))) {
            struct statvfs s;
            if (!statvfs(p->mount, &s)) {
                p->total = (uint64_t)s.f_blocks * s.f_frsize;
                p->free = (uint64_t)s.f_bavail * s.f_frsize;
            }
        }
        n++;
    }
    closedir(d);
    /* nach Partitionsnummer sortieren (readdir liefert beliebig) */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && (strlen(v[j].name) < strlen(v[j - 1].name) ||
                                  (strlen(v[j].name) == strlen(v[j - 1].name) && strcmp(v[j].name, v[j - 1].name) < 0)); j--) {
            partinfo_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
    return n;
}

/* Hängt das Blockgerät (sysfs-Pfad) an Datenträger disk - als dieser, als Partition davon oder über dm/LVM/RAID? */
static int sysdev_on_disk(const char *sys, const char *disk, int depth) {
    char real[PATH_MAX], path[PATH_MAX + 16];
    if (depth > 4 || !realpath(sys, real)) return 0;
    char *base = strrchr(real, '/');
    if (base && !strcmp(base + 1, disk)) return 1;
    if (base) {   /* Partition: .../block/<disk>/<part> */
        *base = 0;
        char *parent = strrchr(real, '/');
        int hit = parent && !strcmp(parent + 1, disk);
        *base = '/';
        if (hit) return 1;
    }
    snprintf(path, sizeof(path), "%s/slaves", real);
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    int hit = 0;
    while (!hit && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char s2[PATH_MAX + 300];
        snprintf(s2, sizeof(s2), "%s/%s", path, e->d_name);
        hit = sysdev_on_disk(s2, disk, depth + 1);
    }
    closedir(d);
    return hit;
}

/* Datenträger mit dem laufenden System: /, /boot, /usr, /var, /home oder Auslagerung - wird nie angeboten */
static int system_disk(const char *disk) {
    static const char *const sys_mounts[] = {"/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home", NULL};
    FILE *f = fopen("/proc/self/mountinfo", "re");
    char line[4096], path[64];
    int hit = 0;
    if (f) {
        while (!hit && fgets(line, sizeof(line), f)) {
            unsigned a, b;
            char point[2048];
            if (sscanf(line, "%*s %*s %u:%u %*s %2047s", &a, &b, point) != 3) continue;
            /* btrfs & Co. melden eine Pseudo-Gerätenummer (0:xx) - dann zählt das Quellgerät hinter " - " */
            const char *dash = strstr(line, " - ");
            char fstype[64], src[512];
            struct stat st;
            if (!a && dash && sscanf(dash + 3, "%63s %511s", fstype, src) == 2 && !strncmp(src, "/dev/", 5) &&
                !stat(src, &st) && S_ISBLK(st.st_mode)) {
                a = major(st.st_rdev);
                b = minor(st.st_rdev);
            }
            if (!a) continue;
            unescape_mount(point);
            for (int i = 0; sys_mounts[i] && !hit; i++)
                if (!strcmp(point, sys_mounts[i])) {
                    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", a, b);
                    hit = sysdev_on_disk(path, disk, 0);
                }
        }
        fclose(f);
    }
    f = fopen("/proc/swaps", "re");
    if (f) {
        while (!hit && fgets(line, sizeof(line), f)) {
            char dev[256];
            struct stat st;
            if (sscanf(line, "%255s", dev) != 1 || strncmp(dev, "/dev/", 5) || stat(dev, &st) || !S_ISBLK(st.st_mode)) continue;
            snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
            hit = sysdev_on_disk(path, disk, 0);
        }
        fclose(f);
    }
    return hit;
}

/* nur Test-Build: Loop-Geräte gelten als USB (USBFMT_TEST_LOOP=1) - so prüft die CI den echten Gerätezugriff */
static int test_loop(const char *disk) {
#ifdef USBFMT_TEST
    const char *e = getenv("USBFMT_TEST_LOOP");
    return e && !strcmp(e, "1") && !strncmp(disk, "loop", 4);
#else
    (void)disk;
    return 0;
#endif
}

/* USB (auch Kartenleser am USB) oder SD-Karte im Steckplatz? Sonst nie anbieten - auch nicht eingelötetes eMMC
 * (mmcblk mit Typ "MMC", oft mit Windows oder dem Bootloader darauf) und seine boot-/rpmb-Bereiche. */
static int usb_disk(const char *disk) {
    char sys[96], real[PATH_MAX], type[16];
    if (test_loop(disk)) return 1;
    if (!strncmp(disk, "mmcblk", 6)) {
        snprintf(sys, sizeof(sys), "/sys/block/%s/device/type", disk);
        return !strstr(disk, "boot") && !strstr(disk, "rpmb") && !read_line(sys, type, sizeof(type)) && !strcmp(type, "SD");
    }
    snprintf(sys, sizeof(sys), "/sys/block/%s", disk);
    if (!realpath(sys, real)) return 0;
    return strstr(real, "/usb") != NULL;
}

static void trim_spaces(char *s) {
    size_t l = strlen(s);
    while (l && s[l - 1] == ' ') s[--l] = 0;
}

/* Seriennummer des Datenträgers laut udev (ID_SERIAL_SHORT) - lesbar ohne Root. Rückgabe 1: udev kennt das Gerät
   (noch) nicht - dann ist ein leeres Ergebnis kein Beleg für "hat keine Seriennummer" */
static int udev_serial(const char *disk, char *out_, size_t n) {
    char path[96], devno[32], line[512];
    unsigned maj, min;
    out_[0] = 0;
    snprintf(path, sizeof(path), "/sys/block/%s/dev", disk);
    if (read_line(path, devno, sizeof(devno)) || sscanf(devno, "%u:%u", &maj, &min) != 2) return 1;
    snprintf(path, sizeof(path), "/run/udev/data/b%u:%u", maj, min);
    FILE *f = fopen(path, "re");
    if (!f) return 1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "E:ID_SERIAL_SHORT=", 18)) { snprintf(out_, n, "%s", line + 18); break; }
    }
    fclose(f);
    serial_clean(out_);
    return 0;
}

int plat_disk_serial(const char *disk, char *buf, size_t n) {
    buf[0] = 0;
    if (!valid_name(disk)) return -1;
    return udev_serial(disk, buf, n);
}

static int disk_info(const char *disk, diskinfo_t *di) {
    char path[128], vendor[64], model[96];
    memset(di, 0, sizeof(*di));
    if (!valid_name(disk)) return -1;
    snprintf(di->id, sizeof(di->id), "%s", disk);
    snprintf(path, sizeof(path), "/sys/block/%s/size", disk);
    di->size = read_num(path) * 512ull;
    snprintf(path, sizeof(path), "/sys/block/%s/queue/logical_block_size", disk);
    di->ss = (uint32_t)read_num(path);
    snprintf(path, sizeof(path), "/sys/block/%s/removable", disk);
    di->removable = read_num(path) != 0;
    snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", disk);
    read_line(path, vendor, sizeof(vendor));
    snprintf(path, sizeof(path), "/sys/block/%s/device/model", disk);
    read_line(path, model, sizeof(model));
    if (!model[0]) {   /* SD-Karten: device/name */
        snprintf(path, sizeof(path), "/sys/block/%s/device/name", disk);
        read_line(path, model, sizeof(model));
    }
    trim_spaces(vendor);
    trim_spaces(model);
    snprintf(di->model, sizeof(di->model), "%s%s%s", vendor, vendor[0] && model[0] ? " " : "", model);
    di->serial_known = !udev_serial(disk, di->serial, sizeof(di->serial));
    return di->size > 0 && di->ss >= 512 ? 0 : -1;
}

int plat_list(void) {
    DIR *d = opendir("/sys/block");
    if (!d) { fprintf(out, "[]\n"); return 0; }
    char names[64][32];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < 64)
        if (valid_name(e->d_name) && (!strncmp(e->d_name, "sd", 2) || !strncmp(e->d_name, "mmcblk", 6) ||
                                      test_loop(e->d_name)))
            snprintf(names[nn++], sizeof(names[0]), "%s", e->d_name);
    closedir(d);
    for (int i = 1; i < nn; i++)   /* sda, sdb, ... in fester Reihenfolge */
        for (int j = i; j > 0 && strcmp(names[j], names[j - 1]) < 0; j--) {
            char t[32];
            memcpy(t, names[j], 32); memcpy(names[j], names[j - 1], 32); memcpy(names[j - 1], t, 32);
        }
    /* erst die USB-Datenträger (nur /sys), dann ihre Partitionen - das Lesen der Dateisysteme weckt schlafende
       Festplatten; Fortschritt auf stderr (list_progress) */
    static diskinfo_t usb[64];
    int nu = 0;
    list_progress(0, NULL, 0);
    for (int k = 0; k < nn; k++)
        if (!disk_info(names[k], &usb[nu]) && usb_disk(names[k]) && !system_disk(names[k])) nu++;
    int first = 1;
    fprintf(out, "[");
    for (int k = 0; k < nu; k++) {
        const diskinfo_t *dp = &usb[k];
        const char *pending[64];
        for (int i = k; i < nu; i++) pending[i - k] = usb[i].model[0] ? usb[i].model : usb[i].id;
        list_progress(10 + 90 * k / nu, pending, nu - k);
        diskinfo_t di = *dp;
        partinfo_t v[MAX_PARTS_SCAN];
        int nv = disk_parts(di.id, v, MAX_PARTS_SCAN);
        fprintf(out, "%s{\"disk\":", first ? "" : ",");
        json_str(out, di.id);
        fprintf(out, ",\"size\":%llu,\"ss\":%u,\"removable\":%s,\"model\":", (unsigned long long)di.size, di.ss,
                di.removable ? "true" : "false");
        json_str(out, di.model);
        fprintf(out, ",\"serial\":");
        json_str(out, di.serial);
        fprintf(out, ",\"volumes\":[");
        int shown = 0;
        for (int i = 0; i < nv; i++) {
            if (!v[i].fs[0] && !v[i].mount[0]) continue;   /* leere Partition: wie unter Windows nicht aufführen */
            fprintf(out, "%s{\"letters\":", shown++ ? "," : "");
            json_str(out, v[i].mount);   /* Einhängepunkt statt Laufwerksbuchstabe */
            fprintf(out, ",\"device\":\"/dev/%s\",\"fs\":", v[i].name);
            json_str(out, v[i].fs);
            fprintf(out, ",\"label\":");
            json_str(out, v[i].label);
            fprintf(out, ",\"total\":%llu,\"free\":%llu}", (unsigned long long)v[i].total, (unsigned long long)v[i].free);
        }
        fprintf(out, "]}");
        first = 0;
    }
    list_progress(100, NULL, 0);
    fprintf(out, "]\n");
    return 0;
}

/* ------------------------------------------------------------------ Exklusiver Zugriff */

struct plat_session {
    char name[32];
};

int plat_image_mode(void) { return image_path != NULL; }

/* jeden Einhängepunkt des Geräts maj:min aushängen (nie erzwungen) */
static int unmount_dev(unsigned maj, unsigned min, char *err, size_t n) {
    char mp[256];
    for (int round = 0; round < 8 && mount_of(maj, min, mp, sizeof(mp)); round++) {
        int ok = 0;
        for (int t = 0; t < 10 && !(ok = umount2(mp, 0) == 0); t++) sleep_ms(300);
        if (!ok) {
            snprintf(err, n, T("Laufwerk %s wird gerade benutzt. Bitte Dateimanager-Fenster und Programme "
                               "schließen, die darauf zugreifen, und es noch einmal versuchen."), mp);
            return -1;
        }
        /* leere Einhängeordner der Desktop-Automatik wieder entfernen */
        if (!strncmp(mp, "/media/", 7) || !strncmp(mp, "/run/media/", 11)) rmdir(mp);
    }
    return 0;
}

/* alle Laufwerke des Datenträgers aushängen (nie erzwungen: hält ein Programm Dateien offen, lieber abbrechen) -
 * auch ein Dateisystem direkt auf dem ganzen Datenträger (ohne Partitionstabelle) */
static int unmount_all(const char *disk, char *err, size_t n) {
    partinfo_t v[MAX_PARTS_SCAN];
    int nv = disk_parts(disk, v, MAX_PARTS_SCAN);
    for (int i = 0; i < nv; i++) {
        if (v[i].holders) {
            snprintf(err, n, T("Laufwerk %s wird vom System verwendet (LVM, RAID oder Verschlüsselung) – abgelehnt."),
                     v[i].name);
            return -1;
        }
        if (unmount_dev(v[i].maj, v[i].min, err, n)) return -1;
    }
    char path[96], devno[32];
    unsigned maj, min;
    snprintf(path, sizeof(path), "/sys/block/%s/dev", disk);
    if (!read_line(path, devno, sizeof(devno)) && sscanf(devno, "%u:%u", &maj, &min) == 2 && unmount_dev(maj, min, err, n))
        return -1;
    return 0;
}

int plat_session_open(session_t *s, const char *disk, uint64_t size, int write, char *err, size_t n) {
    static plat_session_t ps;
    memset(s, 0, sizeof(*s));
    memset(&ps, 0, sizeof(ps));
    s->p = &ps;
    s->fd = s->tfd = -1;
    if (image_path) {   /* Test: Abbild-Datei */
        struct stat st;
        s->fd = open(image_path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
        if (s->fd < 0 || fstat(s->fd, &st) || (uint64_t)st.st_size != size) {
            snprintf(err, n, "%s", T("Abbild nicht passend"));
            plat_session_close(s, 0);
            return -1;
        }
        s->info.size = size;
        s->info.ss = 512;
        snprintf(s->info.id, sizeof(s->info.id), "image");
        snprintf(s->info.model, sizeof(s->info.model), "Testabbild");
        s->tfd = open(image_path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_DIRECT);
        if (s->tfd < 0) s->tfd = s->fd;   /* manche Dateisysteme (WSL /mnt/c) kennen kein O_DIRECT */
        return 0;
    }
    if (disk_info(disk, &s->info)) { snprintf(err, n, T("Datenträger %s nicht gefunden – bitte die Liste aktualisieren."), disk); return -1; }
    if (!usb_disk(disk)) { snprintf(err, n, T("Datenträger %s ist kein USB-Datenträger – abgelehnt."), disk); return -1; }
    if (system_disk(disk)) { snprintf(err, n, T("Datenträger %s enthält das laufende System – abgelehnt."), disk); return -1; }
    if (s->info.size != size) {
        snprintf(err, n, T("Datenträger %s hat sich geändert (andere Größe) – bitte die Liste aktualisieren."), disk);
        return -1;
    }
    snprintf(ps.name, sizeof(ps.name), "%s", disk);
    if (write && unmount_all(disk, err, n)) return -1;
    char dev[48];
    snprintf(dev, sizeof(dev), "/dev/%s", disk);
    /* exklusiv: solange er offen ist, kann niemand (auch nicht die Desktop-Automatik) etwas davon einhängen */
    s->fd = open(dev, (write ? O_RDWR | O_EXCL : O_RDONLY) | O_CLOEXEC);
    if (s->fd < 0) {
        int e = errno;
        if (e == EACCES || e == EPERM) snprintf(err, n, "%s", T("Zugriff verweigert – Administratorrechte nötig."));
        else if (e == EBUSY) snprintf(err, n, T("Laufwerk %s wird gerade benutzt. Bitte Dateimanager-Fenster und Programme "
                                                "schließen, die darauf zugreifen, und es noch einmal versuchen."), dev);
        else sys_error(err, n, "Datenträger öffnen", e);
        return -1;
    }
    uint64_t real = 0;
    int ro = 0;
    if (ioctl(s->fd, BLKGETSIZE64, &real) || real != size) {
        snprintf(err, n, T("Datenträger %s hat sich geändert (andere Größe) – bitte die Liste aktualisieren."), disk);
        plat_session_close(s, 0);
        return -1;
    }
    if (write && !ioctl(s->fd, BLKROGET, &ro) && ro) {
        snprintf(err, n, "%s", T("Der Datenträger ist schreibgeschützt (Schalter am Stick oder an der SD-Karte?)."));
        plat_session_close(s, 0);
        return -1;
    }
    /* Prüfung/Tempo am Zwischenspeicher vorbei: sonst käme das Zurücklesen aus dem Speicher statt vom Stick und ein
     * gefälschter Stick fiele nicht auf */
    s->tfd = open(dev, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_DIRECT);
    if (s->tfd < 0) { sys_error(err, n, "Datenträger öffnen", errno); plat_session_close(s, 0); return -1; }
    return 0;
}

/* das System die (neue) Partitionstabelle einlesen lassen und auf die Geräte warten */
/* udevadm (nur eine root-eigene Datei) mit bis zu drei Argumenten; ohne udevadm kurz warten */
static void udevadm(const char *a1, const char *a2, const char *a3) {
    static const char *const paths[] = {"/usr/bin/udevadm", "/bin/udevadm", "/sbin/udevadm", NULL};
    for (int i = 0; paths[i]; i++) {
        struct stat st;
        if (stat(paths[i], &st) || st.st_uid != 0 || (st.st_mode & 022)) continue;
        pid_t pid = fork();
        if (pid == 0) {
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
            static char *const env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL};
            char *const argv[] = {"udevadm", (char *)a1, (char *)a2, (char *)a3, NULL};
            execve(paths[i], argv, env);   /* feste Umgebung wie bei den anderen Werkzeugen */
            _exit(127);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
        return;
    }
    sleep_ms(1500);
}

static void settle(void) { udevadm("settle", "--timeout=10", NULL); }

/* udev das neue Dateisystem einer Partition erkennen lassen - sonst zeigt die Liste noch eine Weile den alten Stand */
static void reprobe(const char *dev) {
    udevadm("trigger", "--action=change", dev);
    settle();
}

void plat_session_close(session_t *s, int changed) {
    if (s->tfd >= 0 && s->tfd != s->fd) close(s->tfd);
    s->tfd = -1;
    if (s->fd >= 0) {
        fsync(s->fd);
        if (!image_path) {
            ioctl(s->fd, BLKFLSBUF, 0);
            if (changed)
                for (int t = 0; t < 5 && ioctl(s->fd, BLKRRPART, 0); t++) sleep_ms(400);
        }
        close(s->fd);
        s->fd = -1;
        if (changed && !image_path) settle();
    }
}

/* ------------------------------------------------------------------ Werkzeuge des Systems */

/* Werkzeug nur aus den Systemordnern und nur, wenn es root gehört und sonst niemand es ändern kann - der Helfer läuft
 * als root */
static const char *find_tool(const char *const names[]) {
    static const char *const dirs[] = {"/usr/sbin", "/sbin", "/usr/bin", "/bin", NULL};
    static char path[128];
    for (int i = 0; names[i]; i++)
        for (int d = 0; dirs[d]; d++) {
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", dirs[d], names[i]);
            if (!stat(path, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022) && (st.st_mode & 0111))
                return path;
        }
    return NULL;
}

/* Programm starten, Ausgabe sammeln (out_ mit n Bytes) und - falls fs gesetzt - Fortschritt melden (lo..hi).
 * Rückgabe: Exit-Code, -1 = nicht startbar */
static int run_tool(const char *path, char *const argv[], int fs, int lo, int hi, char *out_, size_t n) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);   /* stirbt der Helfer, stirbt das Werkzeug mit */
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        static char *const env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "LANG=C", NULL};
        execve(path, argv, env);   /* englische Ausgabe: der Fortschritt wird daraus gelesen */
        _exit(127);
    }
    close(pfd[1]);
    size_t olen = 0;
    char seg[256], buf[4096];
    size_t sl = 0;
    const char *step = NULL;
    int step_pct = 0;
    ssize_t got;
    while ((got = read(pfd[0], buf, sizeof(buf))) > 0 || (got < 0 && errno == EINTR)) {
        for (ssize_t i = 0; i < got; i++) {
            char c = buf[i];
            if (olen + 1 < n) out_[olen++] = c;
            if (c != '\n' && c != '\r' && c != '\b') { if (sl < sizeof(seg) - 1) seg[sl++] = c; continue; }
            seg[sl] = 0;
            if (sl && fs) {
                int pct;
                unsigned a, b;
                const char *st = extfs_step(fs, seg, &pct);
                if (st) { step = st; step_pct = pct; on_progress(st, lo + pct * (hi - lo) / 100); }
                else if (step && sscanf(seg, " %u/%u", &a, &b) == 2 && b && a <= b) {
                    int pc = step_pct + (step_pct < 80 ? 20 : 100 - step_pct) * (int)a / (int)b;
                    on_progress(step, lo + pc * (hi - lo) / 100);
                }
            }
            sl = 0;
        }
    }
    out_[olen] = 0;
    close(pfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* letzte Textzeile der Ausgabe als Grund */
static const char *last_line(char *text) {
    char *e = text + strlen(text);
    while (e > text && strchr("\r\n\b ", e[-1])) *--e = 0;
    char *ln = e;
    while (ln > text && ln[-1] != '\n' && ln[-1] != '\r' && ln[-1] != '\b') ln--;
    return ln;
}

static void tool_failed(char *err, size_t n, const char *tool, int code, char *output) {
    const char *why = last_line(output);
    if (code < 0) snprintf(err, n, T("%s ließ sich nicht starten"), tool);
    else snprintf(err, n, T("%s ist fehlgeschlagen%s%s"), tool, *why ? ": " : "", why);
}

/* Gerät der Partition index; prüft, dass sie wirklich dort liegt (start/count in Sektoren) */
static int part_device(const diskinfo_t *di, int index, uint64_t start, uint64_t count, char *dev, size_t dn,
                       char *err, size_t n) {
    char name[40], path[128];
    part_name(di->id, index, name, sizeof(name));
    const unsigned long long want_start = start * di->ss / 512, want_size = count * di->ss / 512;
    for (int t = 0; t < 50; t++) {   /* bis zu 10 s auf die Geräte warten */
        snprintf(path, sizeof(path), "/sys/block/%s/%s/start", di->id, name);
        unsigned long long s0 = read_num(path);
        snprintf(path, sizeof(path), "/sys/block/%s/%s/size", di->id, name);
        unsigned long long sz = read_num(path);
        snprintf(dev, dn, "/dev/%s", name);
        struct stat st;
        if (s0 == want_start && sz == want_size && !stat(dev, &st) && S_ISBLK(st.st_mode)) return 0;
        sleep_ms(200);
    }
    snprintf(err, n, T("Die neue Partition %s ist nicht erschienen. Stick abziehen, neu einstecken und erneut versuchen."),
             name);
    return -1;
}

/* Zuordnungseinheit aus dem Bootsektor (FAT/NTFS) - für die Ergebnismeldung */
static uint32_t boot_cluster(const char *dev, int ntfs) {
    uint8_t b[512];
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t r = pread(fd, b, sizeof(b), 0);
    close(fd);
    if (r != (ssize_t)sizeof(b)) return 0;
    uint32_t bps = b[11] | b[12] << 8, spc = b[13];
    if (ntfs && spc > 0x80) return bps << (256 - spc);   /* NTFS: große Einheiten als negativer Exponent */
    return bps * spc;
}

int plat_os_format(const diskinfo_t *di, int fs, int index, uint64_t start, uint64_t count, const char *label,
                   uint32_t cluster, uint32_t serial, int lo, int hi, char *summary, size_t sn, char *err, size_t n) {
    (void)serial;
    static char output[16384];
    char dev[48], cs[24], copt[24], spc[16];
    if (part_device(di, index, start, count, dev, sizeof(dev), err, n)) return -1;
    const int ntfs = fs == FS_NTFS;
    static const char *const ntfs_names[] = {"mkntfs", "mkfs.ntfs", NULL};
    static const char *const fat_names[] = {"mkfs.fat", "mkfs.vfat", "mkdosfs", NULL};
    const char *tool = find_tool(ntfs ? ntfs_names : fat_names);
    if (!tool) {
        snprintf(err, n, T("%s fehlt – bitte das Paket %s installieren."), ntfs ? "mkntfs" : "mkfs.fat",
                 ntfs ? "ntfs-3g" : "dosfstools");
        return -1;
    }
    char *argv[16];
    int a = 0;
    argv[a++] = (char *)tool;
    on_progress(ntfs ? N_("NTFS schreiben") : N_("FAT schreiben"), lo + (hi - lo) / 10);
    if (ntfs) {
        argv[a++] = "--fast";
        argv[a++] = "--force";
        if (cluster) { snprintf(copt, sizeof(copt), "%u", cluster); argv[a++] = "-c"; argv[a++] = copt; }
        if (label[0]) { argv[a++] = "-L"; argv[a++] = (char *)label; }
    } else {
        /* wie Windows: kleine Partitionen FAT12, sonst FAT16 */
        argv[a++] = "-F";
        argv[a++] = count * di->ss < 16 * MIB ? "12" : "16";
        if (cluster) { snprintf(spc, sizeof(spc), "%u", cluster / di->ss); argv[a++] = "-s"; argv[a++] = spc; }
        if (label[0]) { argv[a++] = "-n"; argv[a++] = (char *)label; }
    }
    argv[a++] = dev;
    argv[a] = NULL;
    int rc = run_tool(tool, argv, ntfs ? FS_NTFS : 0, lo, hi, output, sizeof(output));
    if (rc != 0) { tool_failed(err, n, strrchr(tool, '/') + 1, rc, output); return -1; }
    sync();
    reprobe(dev);
    uint32_t c = boot_cluster(dev, ntfs);
    const char *kind = ntfs ? "NTFS" : count * di->ss < 16 * MIB ? "FAT12" : "FAT16";
    if (c) snprintf(summary, sn, T("%s, %s Cluster"), kind, size_text(c, cs, sizeof(cs)));
    else snprintf(summary, sn, "%s", kind);
    on_progress(ntfs ? N_("NTFS schreiben") : N_("FAT schreiben"), hi);
    return 0;
}

int plat_mke2fs(const diskinfo_t *di, int index, uint64_t start, uint64_t count, const char *type, uint32_t block,
                const char *label, int lo, int hi, char *summary, size_t sn, char *err, size_t n) {
    static char output[32768];
    static const char *const names[] = {"mke2fs", NULL};
    char dev[PATH_MAX], bopt[16], eopt[96], kib[32];
    const char *tool = find_tool(names);
    if (!tool) { snprintf(err, n, T("%s fehlt – bitte das Paket %s installieren."), "mke2fs", "e2fsprogs"); return -1; }
    char *argv[20];
    int a = 0;
    argv[a++] = (char *)tool;
    argv[a++] = "-F";
    argv[a++] = "-F";
    argv[a++] = "-t";
    argv[a++] = (char *)type;
    if (image_path) {   /* Test: in den Bereich der Abbild-Datei */
        snprintf(dev, sizeof(dev), "%s", image_path);
        snprintf(eopt, sizeof(eopt), "offset=%llu,lazy_itable_init=1,lazy_journal_init=1,nodiscard",
                 (unsigned long long)(start * di->ss));
    } else {
        if (part_device(di, index, start, count, dev, sizeof(dev), err, n)) return -1;
        snprintf(eopt, sizeof(eopt), "lazy_itable_init=1,lazy_journal_init=1,nodiscard");
    }
    argv[a++] = "-E";
    argv[a++] = eopt;
    if (block) { snprintf(bopt, sizeof(bopt), "%u", block); argv[a++] = "-b"; argv[a++] = bopt; }
    if (label && label[0]) { argv[a++] = "-L"; argv[a++] = (char *)label; }
    argv[a++] = dev;
    snprintf(kib, sizeof(kib), "%lluk", (unsigned long long)(count * di->ss / 1024));
    argv[a++] = kib;
    argv[a] = NULL;
    int rc = run_tool(tool, argv, FS_EXT4, lo, hi, output, sizeof(output));
    if (rc != 0 || !extfs_succeeded(FS_EXT4, output)) { tool_failed(err, n, "mke2fs", rc, output); return -1; }
    if (!image_path) reprobe(dev);
    unsigned long long blocks = 0;
    unsigned kb = block ? block / 1024 : 0;
    const char *p = strstr(output, "Creating filesystem with ");
    if (!kb && p) sscanf(p + 25, "%llu %uk", &blocks, &kb);
    snprintf(summary, sn, T("%s, %u KB Blöcke"), type, kb ? kb : 4);
    return 0;
}

/* ------------------------------------------------------------------ Umbenennen (NTFS/FAT16) */

/* Dateisystem direkt aus dem Bootsektor (1 = NTFS, 2 = FAT12/16, 0 = anderes). Die udev-Datenbank kann kurz nach
 * dem Formatieren noch den alten Stand haben. */
static int boot_kind(const char *dev) {
    uint8_t b[512];
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t r = pread(fd, b, sizeof(b), 0);
    close(fd);
    if (r != (ssize_t)sizeof(b) || b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (!memcmp(b + 3, "NTFS    ", 8)) return 1;
    const unsigned bps = b[11] | b[12] << 8, fat16_size = b[22] | b[23] << 8;   /* FAT32 und exFAT: 0 */
    if (memcmp(b + 3, "EXFAT   ", 8) && bps >= 512 && bps <= 4096 && !(bps & (bps - 1)) && b[13] && b[16] && fat16_size)
        return 2;
    return 0;
}

int plat_os_label(const char *disk, uint64_t size, const char *label, diskinfo_t *di, int *handled, char *err,
                  size_t n) {
    *handled = 0;
    if (!valid_name(disk)) return 0;   /* das Öffnen meldet den Fehler */
    partinfo_t v[MAX_PARTS_SCAN];
    char dev[48];
    int nv = disk_parts(disk, v, MAX_PARTS_SCAN), hit = -1, kind = 0;
    for (int i = 0; i < nv; i++) {
        snprintf(dev, sizeof(dev), "/dev/%s", v[i].name);
        const int k = boot_kind(dev);
        if (!k) continue;
        if (hit != -1) { *handled = 1; snprintf(err, n, "%s", T("Mehrere NTFS-/FAT-Laufwerke auf dem Datenträger.")); return -1; }
        hit = i;
        kind = k;
    }
    if (hit < 0) return 0;
    *handled = 1;
    if (disk_info(disk, di) || !usb_disk(disk) || system_disk(disk) || di->size != size) {
        snprintf(err, n, T("Datenträger %s hat sich geändert (andere Größe) – bitte die Liste aktualisieren."), disk);
        return -1;
    }
    const int ntfs = kind == 1;
    /* ntfslabel und fatlabel hielten eine Bezeichnung mit "-" am Anfang für eine Option ("--" verstehen nicht alle
     * Fassungen - fatlabel aus dosfstools 4.1 zählt nur die Argumente) */
    if (!label_valid(label, ntfs ? FS_NTFS : FS_FAT32) || label[0] == '-') {
        snprintf(err, n, T("Ungültige Bezeichnung: max. %d Zeichen, nur A-Z, 0-9, Leerzeichen, - und _"), ntfs ? 32 : 11);
        return -1;
    }
    if (unmount_all(disk, err, n)) return -1;
    static const char *const ntfs_names[] = {"ntfslabel", NULL};
    static const char *const fat_names[] = {"fatlabel", "dosfslabel", NULL};
    const char *tool = find_tool(ntfs ? ntfs_names : fat_names);
    if (!tool) {
        snprintf(err, n, T("%s fehlt – bitte das Paket %s installieren."), ntfs ? "ntfslabel" : "fatlabel",
                 ntfs ? "ntfs-3g" : "dosfstools");
        return -1;
    }
    static char output[4096];
    snprintf(dev, sizeof(dev), "/dev/%s", v[hit].name);
    char *argv[6];
    int a = 0;
    argv[a++] = (char *)tool;
    if (!ntfs && !label[0]) argv[a++] = "--reset";   /* FAT: Bezeichnung entfernen */
    argv[a++] = dev;
    if (ntfs || label[0]) argv[a++] = (char *)label;
    argv[a] = NULL;
    int rc = run_tool(tool, argv, 0, 0, 0, output, sizeof(output));
    if (rc != 0) { tool_failed(err, n, strrchr(tool, '/') + 1, rc, output); return -1; }
    /* Partitionen neu einlesen lassen: die Desktop-Automatik bindet das Laufwerk dann wieder ein */
    char whole[48];
    snprintf(whole, sizeof(whole), "/dev/%s", disk);
    int fd = open(whole, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ioctl(fd, BLKRRPART, 0);
        close(fd);
        settle();
    }
    return 0;
}

int plat_read_mbr_type(const diskinfo_t *di) {
    if (image_path) return -1;
    char dev[48];
    uint8_t b[512];
    snprintf(dev, sizeof(dev), "/dev/%s", di->id);
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = pread(fd, b, sizeof(b), 0);
    close(fd);
    return r == (ssize_t)sizeof(b) && b[510] == 0x55 && b[511] == 0xAA && b[450] != 0xEE ? b[450] : -1;
}

/* ------------------------------------------------------------------ Start */

/* wer den Helfer über pkexec/sudo gestartet hat (dem gehört die Ausgabedatei) */
static uid_t invoker(gid_t *gid) {
    const char *u = getenv("PKEXEC_UID"), *s = getenv("SUDO_UID"), *g = getenv("SUDO_GID");
    *gid = g ? (gid_t)strtoul(g, NULL, 10) : getgid();
    if (u) return (uid_t)strtoul(u, NULL, 10);
    if (s) return (uid_t)strtoul(s, NULL, 10);
    return getuid();
}

/* Ausgabedatei als root anlegen - nur NEU, nur in einem echten Ordner (keine Verknüpfung auf dem Weg), der dem
 * Aufrufer gehört und den sonst niemand beschreiben kann: Ein Schadprogramm könnte uns sonst eine Datei an fremder
 * Stelle anlegen lassen (etwa /etc/ld.so.preload, die danach dem Aufrufer gehörte). Ein Ordner von root zählt nur,
 * wenn ausschließlich root hineinsieht (mktemp -d des per sudo gestarteten Programms). */
static FILE *open_out(const char *path) {
    char dir[PATH_MAX], real[PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path || (size_t)(slash - path) >= sizeof(dir) || !slash[1]) return NULL;
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = 0;
    if (!realpath(dir, real) || strcmp(real, dir)) return NULL;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return NULL;
    struct stat st;
    gid_t gid;
    uid_t uid = invoker(&gid);
    if (fstat(dfd, &st) || !S_ISDIR(st.st_mode) ||
        !((st.st_uid == uid && !(st.st_mode & (S_IWGRP | S_IWOTH))) || (st.st_uid == 0 && !(st.st_mode & 077)))) {
        close(dfd);
        return NULL;
    }
    int fd = openat(dfd, slash + 1, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    close(dfd);
    if (fd < 0) return NULL;
    if (fchown(fd, uid, (gid_t)-1)) {}   /* gehört dem Aufrufer (darf er löschen/lesen ohnehin) */
    FILE *f = fdopen(fd, "w");
    if (!f) close(fd);
    return f;
}

/* primäre Gruppe des Aufrufers aus /etc/passwd (pkexec setzt keine SUDO_GID) */
static gid_t passwd_gid(uid_t uid, gid_t fallback) {
    FILE *f = fopen("/etc/passwd", "re");
    char line[1024];
    gid_t g = fallback;
    while (f && fgets(line, sizeof(line), f)) {
        char *fld[4] = {line, NULL, NULL, NULL};
        for (int k = 1; k < 4 && fld[k - 1]; k++) {
            char *c = strchr(fld[k - 1], ':');
            if (c) { *c = 0; fld[k] = c + 1; }
        }
        if (fld[3] && (uid_t)strtoul(fld[2], NULL, 10) == uid) { g = (gid_t)strtoul(fld[3], NULL, 10); break; }
    }
    if (f) fclose(f);
    return g;
}

/* Für einen Dateizugriff vorübergehend die Rechte des Aufrufers annehmen (fsuid/fsgid und nur dessen Gruppe).
 * 0 = umgeschaltet bzw. ohnehin nicht root. */
static int saved_ng = -1;
static gid_t saved_groups[256];
static int become_invoker(void) {
    if (geteuid() != 0) return 0;
    gid_t gid;
    uid_t uid = invoker(&gid);
    if (!getenv("SUDO_GID")) gid = passwd_gid(uid, gid);
    if (!uid) return 0;   /* root selbst gestartet */
    saved_ng = getgroups(256, saved_groups);
    if (saved_ng < 0 || setgroups(1, &gid)) { saved_ng = -1; return -1; }
    setfsgid(gid);
    setfsuid(uid);
    if ((uid_t)setfsuid((uid_t)-1) != uid || (gid_t)setfsgid((gid_t)-1) != gid) return -1;
    return 0;
}

static void become_root(void) {
    if (saved_ng < 0) return;
    setfsuid(0);
    setfsgid(0);
    if (setgroups((size_t)saved_ng, saved_groups)) {}
    saved_ng = -1;
}

FILE *plat_fopen(const char *path, int write, char *err, size_t n) {
    int fd = -1, e = EPERM;
    if (!become_invoker()) {
        fd = open(path, write ? O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0644);
        e = errno;
    }
    become_root();
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) { close(fd); fd = -1; e = EINVAL; }
    FILE *f = fd >= 0 ? fdopen(fd, write ? "w" : "r") : NULL;
    if (!f) {
        if (fd >= 0) close(fd);
        sys_error(err, n, write ? N_("Datei anlegen") : N_("Datei öffnen"), e);
    }
    return f;
}

int plat_rename(const char *from, const char *to) {
    int rc = become_invoker() ? -1 : rename(from, to);
    become_root();
    return rc;
}

void plat_remove(const char *path) {
    if (!become_invoker() && unlink(path)) {}
    become_root();
}

static void *cancel_watch(void *arg) {
    (void)arg;
    for (;;) {
        struct stat st;
        if (cancel_path && !lstat(cancel_path, &st)) __atomic_store_n(&cancel_requested, 1, __ATOMIC_RELAXED);
        sleep_ms(300);
    }
    return NULL;
}

int main(int argc, char **argv) {
    out = stdout;
    signal(SIGPIPE, SIG_IGN);
    umask(022);
    int i = 1;
    for (; i + 1 < argc && !strncmp(argv[i], "--", 2); i += 2) {
        if (!strcmp(argv[i], "--out")) {
            FILE *f = open_out(argv[i + 1]);
            if (!f) return 10;   /* Ausgabedatei nicht anlegbar - das Programm meldet den Code */
            out = f;
        } else if (!strcmp(argv[i], "--cancel")) {
            cancel_path = argv[i + 1];
        } else if (!strcmp(argv[i], "--lang")) {
            i18n_set(i18n_parse(argv[i + 1]) < 0 ? LANG_DE : i18n_parse(argv[i + 1]));
#ifdef USBFMT_TEST
        } else if (!strcmp(argv[i], "--image")) {
            image_path = argv[i + 1];
#endif
        } else {
            fprintf(stderr, "Unbekannte Option %s\n", argv[i]);   /* nichts still überspringen */
            return 2;
        }
    }
    if (i >= argc) { fprintf(stderr, "Aufruf: usbfmt_linux list | format | layout | speed | label ...\n"); return 2; }
    if (cancel_path) {
        struct stat st;   /* schon vor dem Start abgebrochen: sofort wissen, nicht erst nach dem ersten Schreiben */
        if (!lstat(cancel_path, &st)) __atomic_store_n(&cancel_requested, 1, __ATOMIC_RELAXED);
        pthread_t t;
        pthread_create(&t, NULL, cancel_watch, NULL);
    }
    return helper_command(argc - i, argv + i);
}
