/*
 * Bezeichnung ändern ohne zu formatieren.
 * exFAT: Eintrag "Volume Label" (0x83) im Wurzelordner; gibt es keinen, kommt er auf den ersten freien Platz.
 * FAT32: Feld BS_VolLab im Bootsektor und in seiner Sicherungskopie sowie der Eintrag mit Attribut 0x08 im
 * Wurzelordner (Windows und die PS5 lesen den Eintrag, ältere Systeme den Bootsektor).
 * Geschrieben wird sektorweise (lesen, ändern, schreiben), wie es Rohgeräte unter FreeBSD verlangen.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsdetect.h"
#include "fsread.h"
#include "i18n.h"
#include "mkfs.h"
#include "vollabel.h"

static int patch(int fd, uint32_t ss, uint64_t off, const void *data, size_t len, char *err, size_t errlen) {
    uint64_t sec = off - off % ss;
    uint8_t *buf;
    if (off % ss + len > ss) { snprintf(err, errlen, "%s", T("Interner Fehler: Eintrag über Sektorgrenze")); return -1; }
    if (posix_memalign((void **)&buf, 4096, ss)) { snprintf(err, errlen, "%s", T("Kein Speicher")); return -1; }
    int ok = pread(fd, buf, ss, (off_t)sec) == (ssize_t)ss;
    if (ok) {
        memcpy(buf + (off - sec), data, len);
        ok = pwrite(fd, buf, ss, (off_t)sec) == (ssize_t)ss;
    }
    free(buf);
    if (!ok) snprintf(err, errlen, T("Schreibfehler bei Byte %llu: %s"), (unsigned long long)off, strerror(errno));
    return ok ? 0 : -1;
}

static int set_exfat(int fd, uint32_t ss, uint64_t start, const char *label, char *err, size_t errlen) {
    fsr_t *f = fsr_open(fd, start);
    uint64_t off;
    int found;
    if (!f) { snprintf(err, errlen, "%s", T("exFAT-Dateisystem nicht lesbar")); return -1; }
    int rc = fsr_exfat_label_slot(f, &off, &found);
    fsr_close(f);
    if (rc) { snprintf(err, errlen, "%s", T("Im Wurzelordner ist kein Platz für die Bezeichnung")); return -1; }
    uint8_t e[32] = {0};
    size_t n = strlen(label);
    e[0] = 0x83;
    e[1] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) { e[2 + 2 * i] = (uint8_t)label[i]; e[3 + 2 * i] = 0; }
    if (!n && !found) return 0;   /* keine Bezeichnung gewünscht und keine vorhanden */
    return patch(fd, ss, off, e, sizeof(e), err, errlen);
}

static int set_fat32(int fd, uint32_t ss, uint64_t start, const char *label, char *err, size_t errlen) {
    uint8_t lab[11], *bs;
    memset(lab, ' ', sizeof(lab));
    for (size_t i = 0; i < 11 && label[i]; i++)
        lab[i] = (uint8_t)(label[i] >= 'a' && label[i] <= 'z' ? label[i] - 32 : label[i]);

    /* Bootsektor und Sicherungskopie (Sektor laut BPB_BkBootSec) */
    if (posix_memalign((void **)&bs, 4096, ss)) { snprintf(err, errlen, "%s", T("Kein Speicher")); return -1; }
    int ok = pread(fd, bs, ss, (off_t)start) == (ssize_t)ss && memcmp(bs + 0x52, "FAT32   ", 8) == 0;
    unsigned backup = ok ? (unsigned)(bs[50] | bs[51] << 8) : 0;
    free(bs);
    if (!ok) { snprintf(err, errlen, "%s", T("FAT32-Bootsektor nicht lesbar")); return -1; }
    const uint8_t *boot_lab = label[0] ? lab : (const uint8_t *)"NO NAME    ";
    if (patch(fd, ss, start + 0x47, boot_lab, 11, err, errlen)) return -1;
    if (backup && backup < 32 && patch(fd, ss, start + (uint64_t)backup * ss + 0x47, boot_lab, 11, err, errlen))
        return -1;

    /* Eintrag im Wurzelordner: ändern, anlegen oder (leere Bezeichnung) als gelöscht markieren */
    uint64_t off;
    int found = 0;
    if (fs_fat32_label_slot(fd, start, &off, &found) != 0) return 0;   /* Wurzelordner voll: Bootsektor reicht */
    if (!label[0]) {
        static const uint8_t del = 0xE5;
        return found ? patch(fd, ss, off, &del, 1, err, errlen) : 0;
    }
    uint8_t e[32] = {0};
    memcpy(e, lab, 11);
    e[11] = 0x08;
    return patch(fd, ss, off, e, found ? 12 : 32, err, errlen);   /* vorhandenen Eintrag: nur Name + Attribut */
}

int vol_set_label(int fd, uint32_t ss, uint64_t devsize, const char *label, char *err, size_t errlen) {
    fsinfo_t fi;
    fs_probe(fd, ss, devsize, &fi);
    int fs = !strcmp(fi.fs, "exFAT") ? FS_EXFAT : !strcmp(fi.fs, "FAT32") ? FS_FAT32 : 0;
    if (!fs) {
        snprintf(err, errlen, T("Umbenennen geht nur bei exFAT und FAT32 (dieser Datenträger: %s)"), T(fi.fs));
        return -1;
    }
    if (!label_valid(label, fs)) {
        snprintf(err, errlen, "%s", T("Ungültige Bezeichnung: max. 11 Zeichen, nur A-Z, 0-9, Leerzeichen, - und _"));
        return -1;
    }
    int rc = fs == FS_EXFAT ? set_exfat(fd, ss, fi.start, label, err, errlen)
                            : set_fat32(fd, ss, fi.start, label, err, errlen);
    if (rc == 0 && fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
        snprintf(err, errlen, "fsync: %s", strerror(errno));
        return -1;
    }
    return rc;
}
