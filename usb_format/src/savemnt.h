/* Einen Spielstand der Konsole öffnen (übernommen aus dem PS5 Cooling Center, savemnt.h).
 *
 * Ein Spielstand ist ein verschlüsseltes Dateisystem-Abbild (sdimg_<Name>). Um ihn auf eine andere Konsole zu bringen, müssen
 * seine Dateien im Klartext gelesen werden, und zum Einspielen muss ein neues Abbild angelegt und gefüllt werden. Beides geht
 * über die Funktionen der Konsole selbst - die App entschlüsselt nichts selbst:
 *
 *   Schlüssel  das Abbild trägt seinen Schlüssel an DIESE Konsole gebunden (96 Byte ab 0x800; PS4: in <Name>.bin daneben). Der
 *              pfsmgr des Kernels öffnet ihn (ioctl 0xc0845302 auf /dev/pfsmgr: 0x100-Byte-Puffer auf dem Heap, die gebundenen
 *              Bytes zuerst, der 32-Byte-Schlüssel kommt ab 0x60 zurück). Deshalb taugt eine rohe Kopie auf einer anderen
 *              Konsole nicht: Ihren Schlüssel kann nur die Konsole öffnen, die ihn angelegt hat.
 *   Einhängen  libSceFsInternalForVsh: sceFsInitMountSaveDataOpt / sceFsMountSaveData(opt, Abbild, Ordner, Schlüssel) mit
 *              Budget "system"; sceFsUmountSaveData. Eingehängt wird nie das Abbild, wo die Konsole es hält (endet in EPIPE),
 *              sondern eine Kopie unter /data.
 *   param      ein PS5-Spielstand hält sein echtes param.sfo im Kopf des Abbilds (Versatz 0xC00), im Dateisystem steht dort oft
 *              nur ein Block Nullen; der Kernel gibt es über /dev/pfsctldev heraus: 'PS5PARAM' u64, Version u32 (0),
 *              SFO-Größe u32, SFO-Versatz u32 (20), dann das SFO.
 *
 * Einhängen, Aushängen und Öffnen des Schlüssels brauchen die Kennung des Spielstand-Dienstes (authid 0x4800000000000010). Die gilt für
 * den ganzen Prozess, deshalb hält plat_save_service_enter/leave dazu eine Sperre - aber nur für diese Aufrufe: savemnt_open gibt beides
 * zurück, sobald das Einhängen steht, savemnt_close nimmt es fürs Aushängen wieder (wie das Cooling Center seit 1.88.0).
 *
 * Auf dem Host (Tests) berührt nichts davon einen Kernel: ein "Einhängen" kopiert den Inhalt, den der Test unter
 * USBFMT_SAVEMNT_STORE/<Kennung des Abbilds> abgelegt hat (siehe savemnt.c). */
#ifndef SAVEMNT_H
#define SAVEMNT_H

#include <stddef.h>
#include <stdint.h>

#define SAVEMNT_SEALED_OFFSET 0x800u
#define SAVEMNT_SEALED_BYTES  96u
#define SAVEMNT_PARAM_OFFSET  0xC00u
#define SAVEMNT_PARAM_BYTES   0x8000u
#define SAVEMNT_PARAM_MAGIC   UINT64_C(0x4D41524150355350)   /* "PS5PARAM" als Little-Endian-Zahl */

typedef struct {
    int  open;
    int  handle;
    int  released;   /* 1: Kennung und Sperre wurden nach dem Einhängen zurückgegeben (savemnt_close nimmt sie wieder) */
    char image[320];
    char mount[256];
} savemnt_t;

/* PS5-Abbild (Kopie unter /data) auf mount_dir (leerer Ordner) einhängen. Kennung und Sperre bleiben bis savemnt_close.
 * 0 oder -1 (err; dann ist nichts gehalten) */
int savemnt_open(const char *image, const char *mount_dir, savemnt_t *m, char *err, size_t err_len);
/* dasselbe für einen PS4-Spielstand: der gebundene Schlüssel kommt aus <Name>.bin (der Aufrufer liest ihn) */
int savemnt_open_sealed(const char *image, const uint8_t sealed[96], const char *mount_dir, savemnt_t *m, char *err, size_t err_len);
/* aushängen, Kennung und Sperre zurückgeben (immer). 0 = sauber ausgehängt, sonst -1 */
int savemnt_close(savemnt_t *m, char *err, size_t err_len);
/* neues, leeres PS5-Abbild (Datei darf nicht da sein), size Byte, Blöcke von block_size (32768/65536). 0 oder -1 */
int savemnt_create(const char *path, uint64_t size, uint32_t block_size, char *err, size_t err_len);
/* neues, leeres PS4-Abbild mit frischem, an diese Konsole gebundenem Schlüssel (sealed_out -> <Name>.bin). 0 oder -1 */
int savemnt_create_ps4(const char *path, uint64_t size, uint8_t sealed_out[96], char *err, size_t err_len);
/* aushängen, was auf mount_dir hängt (Rest eines abgebrochenen Laufs). 0 = losgelassen. Nie auf einen Ordner mit eigenen Dateien. */
int savemnt_umount_path(const char *mount_dir, char *err, size_t err_len);
/* das echte param.sfo aus dem Kopf des Abbilds; already_elevated = 1 zwischen open und close. 0 = gelesen, -1 = kein Blob,
 * -2 = Fehler (err) */
int savemnt_param_read(const char *image, int already_elevated, uint8_t *sfo, uint32_t sfo_cap, uint32_t *sfo_len, char *err, size_t err_len);
/* Art des Abbilds nach den ersten 0x40 Byte: 1 = PS4, 2 = PS5, 0 = keines */
int savemnt_header_kind(const uint8_t *header, size_t len);

#endif
