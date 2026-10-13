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

/* Öffnet den PS5-Browser mit url. 0 = ok, sonst Fehlertext in err. */
int  plat_open_browser(const char *url, char *err, size_t errlen);

#endif
