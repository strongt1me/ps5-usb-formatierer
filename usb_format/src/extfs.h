#ifndef EXTFS_H
#define EXTFS_H

/* NTFS und ext4 schreiben die bewährten Originalwerkzeuge mkntfs (ntfs-3g) und mke2fs (e2fsprogs).
 * Hier steht, was PS5- und Linux-Testversion gemeinsam brauchen. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  buf[1024];
    char *argv[24];      /* argv[0] = Werkzeugname, NULL-terminiert */
    int   argc;
} extfs_cmd_t;

const char *extfs_tool(int fs);   /* "mkntfs" bzw. "mke2fs" */

/* Kommandozeile für das Werkzeug: Schnellformat, ohne Rückfragen. cluster 0 = Standard. */
int extfs_build(extfs_cmd_t *c, int fs, const char *dev, const char *label, uint32_t cluster, uint32_t ss,
                uint64_t start);

/* Hat das Werkzeug laut Ausgabe erfolgreich abgeschlossen? */
int extfs_succeeded(int fs, const char *output);

/* Liegt am Partitionsanfang ein gültiger Kopf des Dateisystems? head = die ersten 4096 Bytes */
int extfs_signature_ok(int fs, const uint8_t *head);

/* Schritt-Text für eine Ausgabezeile, oder NULL, wenn sie keinen neuen Schritt anzeigt */
const char *extfs_step(int fs, const char *line, int *percent);

/* Zusammenfassung wie "NTFS, 4 KB Cluster" (bei Standard-Größe aus der Werkzeug-Ausgabe gelesen) */
void extfs_summary(int fs, uint32_t cluster, const char *output, char *out, size_t n);

#endif
