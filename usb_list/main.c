/*
 * usb_list - listet USB-Massenspeicher (/dev/da*) auf der PS5
 * Nur lesend: es wird nichts geschrieben oder ausgehaengt.
 *
 * Ausgabe: stdout (bei elfldr ueber socat sichtbar) + Systembenachrichtigung.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <sys/param.h>
#include <sys/mount.h>

typedef struct {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *msg) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void fmt_size(uint64_t bytes, char *out, size_t n) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(out, n, "%.1f %s", v, u[i]);
}

/* Mountpoint + FS-Typ zu einem Device suchen */
static int find_mount(const char *devpath, char *mnt, size_t mn, char *fs, size_t fn) {
    int cnt = getfsstat(NULL, 0, MNT_NOWAIT);
    if (cnt <= 0) return 0;
    struct statfs *buf = calloc(cnt, sizeof(struct statfs));
    if (!buf) return 0;
    cnt = getfsstat(buf, cnt * sizeof(struct statfs), MNT_NOWAIT);
    int found = 0;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(buf[i].f_mntfromname, devpath) == 0) {
            snprintf(mnt, mn, "%s", buf[i].f_mntonname);
            snprintf(fs, fn, "%s", buf[i].f_fstypename);
            found = 1;
            break;
        }
    }
    free(buf);
    return found;
}

int main(void) {
    DIR *d = opendir("/dev");
    if (!d) {
        perror("opendir /dev");
        notify("usb_list: /dev nicht lesbar");
        return 1;
    }

    char summary[2048] = "USB-Geraete:\n";
    int total = 0;
    struct dirent *e;

    printf("%-12s %-10s %-8s %-8s %s\n", "Device", "Groesse", "Sektor", "FS", "Mountpoint");
    printf("----------------------------------------------------------\n");

    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "da", 2) != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("%-12s (open fehlgeschlagen)\n", e->d_name);
            continue;
        }

        off_t media = 0;
        u_int sector = 0;
        ioctl(fd, DIOCGMEDIASIZE, &media);
        ioctl(fd, DIOCGSECTORSIZE, &sector);
        close(fd);

        char sz[32], mnt[MNAMELEN] = "-", fs[MFSNAMELEN] = "-";
        fmt_size((uint64_t)media, sz, sizeof(sz));
        find_mount(path, mnt, sizeof(mnt), fs, sizeof(fs));

        printf("%-12s %-10s %-8u %-8s %s\n", e->d_name, sz, sector, fs, mnt);

        char line[160];
        snprintf(line, sizeof(line), "%s %s %s\n", e->d_name, sz, mnt);
        strncat(summary, line, sizeof(summary) - strlen(summary) - 1);
        total++;
    }
    closedir(d);

    if (total == 0) {
        printf("Keine /dev/da* Geraete gefunden.\n");
        notify("usb_list: keine USB-Geraete gefunden");
    } else {
        notify(summary);
    }
    return 0;
}
