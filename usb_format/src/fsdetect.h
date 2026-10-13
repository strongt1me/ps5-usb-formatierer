#ifndef FSDETECT_H
#define FSDETECT_H

#include <stddef.h>
#include <stdint.h>

/* Dateisystem des Datenträgers (nur lesen): "exFAT", "FAT32", "FAT16", "FAT12", "NTFS", "ext4", "ext3", "ext2",
 * "leer", "keine Partition" oder "unbekannt". fd: Gerät bzw. Image, ss: Sektorgröße. */
const char *fs_detect(int fd, uint32_t ss);

/* Wie fs_detect, dazu Lage der (ersten) Partition und die Bezeichnung */
typedef struct {
    const char *fs;         /* wie fs_detect */
    const char *scheme;     /* "MBR", "GPT" oder "" (Dateisystem ohne Partitionstabelle) */
    uint64_t    start;      /* Byte-Offset des Dateisystems */
    uint64_t    size;       /* Bytes der Partition; ohne Tabelle: Rest des Datenträgers */
    char        label[48];  /* Bezeichnung (exFAT, FAT, ext), leer wenn keine */
} fsinfo_t;

int fs_probe(int fd, uint32_t ss, uint64_t devsize, fsinfo_t *out);   /* 0 = gelesen */

/* Dateisystem aus dem Anfang einer Partition (Bootsektor, bei ext ab Byte 1024 der Superblock), sonst NULL */
const char *fs_detect_buf(const uint8_t *b, size_t len);

/* FAT32: Wurzelordner-Eintrag der Bezeichnung (Attribut 0x08) bzw. erster freier Platz im ersten Cluster des
 * Wurzelordners. fd/start wie bei fs_probe. 0 = ok, *found = 1 wenn eine Bezeichnung existiert. */
int fs_fat32_label_slot(int fd, uint64_t start, uint64_t *off, int *found);

#endif
