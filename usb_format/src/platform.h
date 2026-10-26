#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "library.h"
#include "mkfs.h"

#define MAX_DEVS   16
#define MAX_MOUNTS 4

typedef struct {
    char     path[96];
    char     fstype[16];
} mountinfo_t;

typedef struct {
    char        name[16];   /* z.B. "da0" */
    char        model[96];  /* Hersteller + Modell laut Laufwerk, z.B. "SanDisk Ultra" (kann leer sein) */
    char        path[256];  /* Geraete- bzw. Image-Pfad */
    uint64_t    size;       /* Bytes */
    uint32_t    ss;         /* Sektorgroesse */
    char        format[16]; /* aktuelles Dateisystem laut fs_probe, z.B. "exFAT", "leer", "unbekannt" */
    char        label[48];  /* Bezeichnung des Dateisystems (kann leer sein) */
    char        scheme[4];  /* Partitionstabelle: "MBR", "GPT" oder "" */
    char        ident[64];  /* Seriennummer laut GEOM <ident> (kann leer sein) - erkennt den Stick wieder */
    uint64_t    part_end;   /* Ende der ersten Partition in Bytes, 0 = keine */
    int         nmounts;
    mountinfo_t mounts[MAX_MOUNTS];
    int         locked;     /* ausserhalb von /mnt/usb* eingehaengt -> wird nicht angefasst */
} usbdev_t;

int  plat_scan(usbdev_t *out, int max);
int  plat_open_rw(const usbdev_t *d, char *err, size_t errlen);   /* fd oder -1 */
int  plat_open_ro(const usbdev_t *d, char *err, size_t errlen);   /* nur lesen, auch eingebunden; fd oder -1 */

/* Weitere Spiele-Orte für die Bibliothek (wie ShadowMountPlus), NULL am Ende; Paare "Pfad", "usb"|"dir" */
const char *const *plat_library_roots(void);

/* Titel-IDs zu Abbild-Pfaden von ShadowMountPlus (API und debug.log); *h mit free() freigeben */
void plat_library_hints(lib_hint_t **h, int *n);

/* POST an die API von ShadowMountPlus (PS5: 127.0.0.1:10101; Host-Tests: USBFMT_SMP_PORT). Antwortkörper (free())
 * und HTTP-Status, oder NULL, wenn ShadowMountPlus nicht erreichbar ist. */
char *plat_smp_post(const char *path, const char *body, int *http, size_t *len);

/* Für Sticks, die die PS5 eingebunden hat: meldet den Stick per Software neu an (USB_DEVICEENUMERATE) - geht das
 * nicht, wartet sie, bis der Benutzer ihn abzieht und wieder einsteckt. Die PS5 gibt ihn dabei selbst frei; beim
 * Wiederauftauchen wird er exklusiv geöffnet, bevor die PS5 ihn erneut einbinden kann.
 * Liefert den fd (Lesen/Schreiben, exklusiv) und in out die Daten nach dem Einstecken (der Name kann sich
 * ändern), sonst -1 und Fehlertext. *cancel wird regelmäßig geprüft. */
int  plat_wait_replug(const usbdev_t *d, usbdev_t *out, progress_fn progress, volatile int *cancel,
                      char *err, size_t errlen);
void plat_notify(const char *msg);
void plat_local_ip(char *buf, size_t len);
void plat_throttle(void);   /* nur fuer Tests auf dem Host: kuenstlich verlangsamen */

/* NTFS/ext4: schreibt das Dateisystem mit mkntfs bzw. mke2fs in die gerade angelegte erste Partition
 * (start/count in Sektoren; gpt: GPT statt MBR, die Partition heißt dann daNp1 statt daNs1).
 * 0 = ok und summary gefüllt, sonst Fehlertext in err. */
int  plat_external_mkfs(const usbdev_t *d, int fs, const char *label, uint32_t cluster, uint64_t start,
                        uint64_t count, int gpt, progress_fn progress, char *summary, size_t sumlen,
                        char *err, size_t errlen);

/* Ordner für Einstellungen und Merker (PS5: /data/usb_format) */
const char *plat_data_dir(void);

/* Ordner mit den Sandboxen laufender Titel (PS5: /mnt/sandbox, je Titel "PPSA12345_000"); NULL = keiner */
const char *plat_sandbox_dir(void);

/* Öffnet den PS5-Browser mit url. 0 = ok, sonst Fehlertext in err. */
int  plat_open_browser(const char *url, char *err, size_t errlen);

/* Ordner mit den Benutzern und ihren Spielständen (PS5: /user/home); NULL = keiner (Host ohne USBFMT_SAVES) */
const char *plat_saves_dir(void);

/* Firmware der PS5 als "9.00"; leer, wenn unbekannt (Host: USBFMT_FW) */
void plat_fw_version(char *out, size_t n);

/* Liegt path auf einem FAT32-Datenträger (einzelne Dateien höchstens 4 GB)? (Host: USBFMT_TARGET_FAT32 gesetzt) */
int  plat_path_fat32(const char *path);

#endif
