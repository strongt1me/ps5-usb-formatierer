#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "extfs.h"
#include "mkfs.h"

const char *extfs_tool(int fs) { return fs == FS_NTFS ? "mkntfs" : "mke2fs"; }

static void add(extfs_cmd_t *c, const char *fmt, ...) {
    size_t used = 0;
    for (int i = 0; i < c->argc; i++) used += strlen(c->argv[i]) + 1;
    if (c->argc >= (int)(sizeof(c->argv) / sizeof(c->argv[0])) - 1 || used >= sizeof(c->buf)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->buf + used, sizeof(c->buf) - used, fmt, ap);
    va_end(ap);
    c->argv[c->argc++] = c->buf + used;
    c->argv[c->argc] = NULL;
}

int extfs_build(extfs_cmd_t *c, int fs, const char *dev, const char *label, uint32_t cluster, uint32_t ss,
                uint64_t start) {
    memset(c, 0, sizeof(*c));
    add(c, "%s", extfs_tool(fs));
    if (fs == FS_NTFS) {
        /* -Q: Schnellformat (nicht nullen), -F: auch auf Zeichengeräten/Dateien, -p/-H/-S: Partitionslage für
         * den Bootsektor wie bei Windows, -s: Sektorgröße */
        add(c, "-Q");
        add(c, "-F");
        add(c, "-s");
        add(c, "%u", ss);
        add(c, "-p");
        add(c, "%llu", (unsigned long long)start);
        add(c, "-H");
        add(c, "255");
        add(c, "-S");
        add(c, "63");
        if (cluster) { add(c, "-c"); add(c, "%u", cluster); }
        if (label && *label) { add(c, "-L"); add(c, "%s", label); }
    } else if (fs == FS_EXT4) {
        /* -F -F: ohne Rückfragen; Inode-Tabellen und Journal initialisiert Linux beim ersten Einhängen im
         * Hintergrund – sonst schriebe mke2fs auf großen Laufwerken viele Gigabyte Nullen */
        add(c, "-F");
        add(c, "-F");
        add(c, "-t");
        add(c, "ext4");
        add(c, "-E");
        add(c, "lazy_itable_init=1,lazy_journal_init=1");
        if (cluster) { add(c, "-b"); add(c, "%u", cluster); }
        if (label && *label) { add(c, "-L"); add(c, "%s", label); }
    } else {
        return -1;
    }
    add(c, "%s", dev);
    return 0;
}

int extfs_succeeded(int fs, const char *out) {
    if (fs == FS_NTFS) return strstr(out, "mkntfs completed successfully") != NULL;
    const char *p = strstr(out, "filesystem accounting information");
    return p && strstr(p, "done") != NULL;
}

int extfs_signature_ok(int fs, const uint8_t *head) {
    if (fs == FS_NTFS) return memcmp(head + 3, "NTFS    ", 8) == 0 && head[510] == 0x55 && head[511] == 0xAA;
    return head[1024 + 56] == 0x53 && head[1024 + 57] == 0xEF;   /* ext2/3/4-Superblock-Magic 0xEF53 */
}

const char *extfs_step(int fs, const char *line, int *percent) {
    static const struct { int fs; const char *key, *step; int pct; } map[] = {
        {FS_NTFS, "Cluster size has been automatically", "NTFS vorbereiten", 10},
        {FS_NTFS, "Creating NTFS volume structures", "NTFS-Strukturen schreiben", 40},
        {FS_NTFS, "creating root directory", "Wurzelverzeichnis anlegen", 80},
        {FS_NTFS, "mkntfs completed successfully", "NTFS fertig", 100},
        {FS_EXT4, "Creating filesystem with", "ext4 vorbereiten", 10},
        {FS_EXT4, "Allocating group tables", "Gruppentabellen anlegen", 25},
        {FS_EXT4, "Writing inode tables", "Inode-Tabellen schreiben", 45},
        {FS_EXT4, "Creating journal", "Journal anlegen", 65},
        {FS_EXT4, "Writing superblocks", "Superblöcke schreiben", 85},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].fs == fs && strstr(line, map[i].key)) {
            *percent = map[i].pct;
            return map[i].step;
        }
    return NULL;
}

void extfs_summary(int fs, uint32_t cluster, const char *output, char *out, size_t n) {
    unsigned size = cluster;
    const char *p;
    /* Bei "Standard" steht die gewählte Größe in der Ausgabe des Werkzeugs */
    if (!size && fs == FS_NTFS && (p = strstr(output, "automatically set to ")))
        sscanf(p + 21, "%u", &size);
    if (!size && fs == FS_EXT4 && (p = strstr(output, "Creating filesystem with "))) {
        unsigned long long blocks;
        unsigned k;
        if (sscanf(p + 25, "%llu %uk", &blocks, &k) == 2) size = k * 1024;
    }
    if (!size) size = 4096;
    if (size >= 1024) snprintf(out, n, "%s, %u KB %s", fs == FS_NTFS ? "NTFS" : "ext4", size / 1024,
                               fs == FS_NTFS ? "Cluster" : "Blöcke");
    else snprintf(out, n, "NTFS, %u Byte Cluster", size);
}
