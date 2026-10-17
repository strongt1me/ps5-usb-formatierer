/* usbfmt_win.exe - Windows-Teil des PC-Helfers (Ablauf und Befehle: ../pc/helper.c): findet USB-Datenträger,
 * sperrt und öffnet sie, schreibt NTFS und FAT (FAT16) mit der Formatierfunktion von Windows (FormatEx) und ext2/3/4
 * mit der mitgelieferten mke2fs.exe.
 *
 *   usbfmt_win [--out F] [--cancel F] [--lang de|en|it|es|fr] list|format|layout|speed|label ...
 *
 * Existiert die Datei --cancel, werden Prüfung und Tempomessung abgebrochen. Nur USB- und SD-Datenträger, nie die
 * Systemplatte. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../devtest.h"
#include "../extfs.h"
#include "../i18n.h"
#include "../mkfs.h"
#include "../mkfs_int.h"
#include "../vollabel.h"
#include "../pc/helper.h"

#undef strerror

#define MAX_DISKS   64
#define MAX_VOLS    16

static const WCHAR *cancel_path;   /* Unicode: Temp-Pfade mit Umlauten (C:\Users\Jürgen\...) */

/* ------------------------------------------------------------------ Hilfen */

static void utf8(const WCHAR *w, char *buf, size_t n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL)) buf[0] = 0;
}

static void win_error(char *buf, size_t n, const char *what) {
    DWORD code = GetLastError();
    WCHAR w[200];
    char m[256] = "";
    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, w, 200, NULL))
        utf8(w, m, sizeof(m));
    size_t l = strlen(m);
    while (l && strchr("\r\n. ", m[l - 1])) m[--l] = 0;
    snprintf(buf, n, T("%s: %s (Windows-Fehler %lu)"), T(what), m, (unsigned long)code);
}

static DWORD WINAPI cancel_watch(LPVOID arg) {
    (void)arg;
    for (;;) {
        if (cancel_path && GetFileAttributesW(cancel_path) != INVALID_FILE_ATTRIBUTES) __atomic_store_n(&cancel_requested, 1, __ATOMIC_RELAXED);
        Sleep(300);
    }
    return 0;
}

/* ------------------------------------------------------------------ Datenträger */

typedef struct {
    WCHAR    guid[64];     /* \\?\Volume{...}\ */
    char     letters[32];  /* "E:" bzw. "E: F:" */
    char     fs[16], label[128];   /* bis 32 UTF-16-Zeichen als UTF-8 */
    DWORD    serial;       /* Seriennummer des Dateisystems */
    uint64_t total, free;
    int      spans;        /* liegt auch auf einem anderen Datenträger (übergreifend/Stripeset, dynamisch) */
} volinfo_t;

/* USB oder SD-Karte - nicht BusTypeMmc: das ist eingelötetes eMMC (oft mit einem zweiten System darauf) */
static int usb_bus(int bus) { return bus == BusTypeUsb || bus == BusTypeSd; }

static HANDLE open_disk(int n, DWORD access) {
    WCHAR path[48];
    swprintf(path, 48, L"\\\\.\\PhysicalDrive%d", n);
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static void trim_copy(char *dst, size_t n, const char *src) {
    while (*src == ' ') src++;
    snprintf(dst, n, "%s", src);
    size_t l = strlen(dst);
    while (l && dst[l - 1] == ' ') dst[--l] = 0;
}

static int disk_info(int n, diskinfo_t *di) {
    memset(di, 0, sizeof(*di));
    di->num = n;
    snprintf(di->id, sizeof(di->id), "%d", n);
    HANDLE h = open_disk(n, 0);
    if (h == INVALID_HANDLE_VALUE) return -1;
    BYTE buf[1024];
    DWORD r;
    int ok = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, buf, sizeof(buf), &r, NULL)) {
        const DISK_GEOMETRY_EX *g = (const DISK_GEOMETRY_EX *)buf;
        di->size = (uint64_t)g->DiskSize.QuadPart;
        di->ss = g->Geometry.BytesPerSector;
        di->media = (unsigned long)g->Geometry.MediaType;
        ok = di->size > 0 && di->ss >= 512;
    }
    STORAGE_PROPERTY_QUERY q = {StorageDeviceProperty, PropertyStandardQuery, {0}};
    memset(buf, 0, sizeof(buf));
    if (ok && DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf) - 1, &r, NULL)) {
        const STORAGE_DEVICE_DESCRIPTOR *d = (const STORAGE_DEVICE_DESCRIPTOR *)buf;
        char vendor[64] = "", product[64] = "";
        di->bus = d->BusType;
        di->removable = d->RemovableMedia;
        if (d->VendorIdOffset && d->VendorIdOffset < r) trim_copy(vendor, sizeof(vendor), (const char *)buf + d->VendorIdOffset);
        if (d->ProductIdOffset && d->ProductIdOffset < r) trim_copy(product, sizeof(product), (const char *)buf + d->ProductIdOffset);
        snprintf(di->model, sizeof(di->model), "%s%s%s", vendor, vendor[0] && product[0] ? " " : "", product);
        if (d->SerialNumberOffset && d->SerialNumberOffset < r)
            trim_copy(di->serial, sizeof(di->serial), (const char *)buf + d->SerialNumberOffset);
        serial_clean(di->serial);
        di->serial_known = 1;
    }
    CloseHandle(h);
    return ok ? 0 : -1;
}

int plat_disk_serial(const char *disk, char *buf, size_t n) {
    diskinfo_t di;
    char *end;
    long num = strtol(disk, &end, 10);
    buf[0] = 0;
    if (end == disk || *end || num < 0 || num >= MAX_DISKS || disk_info((int)num, &di)) return -1;
    snprintf(buf, n, "%s", di.serial);
    return di.serial_known ? 0 : 1;
}

/* Datenträgernummern eines Volumes (Pfad ohne abschließenden \) */
static int volume_disks(const WCHAR *vol, int *disks, int max) {
    HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BYTE buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
    DWORD r;
    int n = 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof(buf), &r, NULL)) {
        const VOLUME_DISK_EXTENTS *e = (const VOLUME_DISK_EXTENTS *)buf;
        for (DWORD i = 0; i < e->NumberOfDiskExtents && n < max; i++) disks[n++] = (int)e->Extents[i].DiskNumber;
    }
    CloseHandle(h);
    return n;
}

static void strip_slash(const WCHAR *in, WCHAR *outp, size_t n) {
    wcsncpy(outp, in, n - 1);
    outp[n - 1] = 0;
    size_t l = wcslen(outp);
    if (l && outp[l - 1] == L'\\') outp[l - 1] = 0;
}

/* Alle Volumes auf Datenträger n */
static int disk_volumes(int n, volinfo_t *v, int max) {
    WCHAR name[64];
    int cnt = 0;
    HANDLE f = FindFirstVolumeW(name, 64);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        WCHAR bare[64];
        int disks[8];
        strip_slash(name, bare, 64);
        int nd = volume_disks(bare, disks, 8), on = 0;
        for (int i = 0; i < nd; i++) on |= disks[i] == n;
        if (!on || cnt >= max) continue;
        volinfo_t *x = &v[cnt++];
        memset(x, 0, sizeof(*x));
        for (int i = 0; i < nd; i++) x->spans |= disks[i] != n;
        wcscpy(x->guid, name);
        WCHAR paths[256], lbl[64], fsn[16];
        DWORD len = 0;
        if (GetVolumePathNamesForVolumeNameW(name, paths, 256, &len)) {
            for (WCHAR *p = paths; *p; p += wcslen(p) + 1) {
                if (wcslen(p) == 3 && p[1] == L':') {
                    size_t l = strlen(x->letters);
                    snprintf(x->letters + l, sizeof(x->letters) - l, "%s%c:", l ? " " : "", (char)p[0]);
                }
            }
        }
        if (GetVolumeInformationW(name, lbl, 64, &x->serial, NULL, NULL, fsn, 16)) {
            utf8(lbl, x->label, sizeof(x->label));
            utf8(fsn, x->fs, sizeof(x->fs));
        }
        ULARGE_INTEGER fr, tot;
        if (GetDiskFreeSpaceExW(name, &fr, &tot, NULL)) { x->free = fr.QuadPart; x->total = tot.QuadPart; }
    } while (FindNextVolumeW(f, name, 64));
    FindVolumeClose(f);
    return cnt;
}

/* Datenträger, auf dem Windows liegt - wird nie angeboten */
static int system_disk(int n) {
    WCHAR win[MAX_PATH], vol[8];
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return 0;
    swprintf(vol, 8, L"\\\\.\\%c:", win[0]);
    int disks[8], nd = volume_disks(vol, disks, 8);
    for (int i = 0; i < nd; i++) if (disks[i] == n) return 1;
    return 0;
}

int plat_list(void) {
    int first = 1;
    fprintf(out, "[");
    for (int n = 0; n < MAX_DISKS; n++) {
        diskinfo_t di;
        if (disk_info(n, &di) || !usb_bus(di.bus) || system_disk(n)) continue;
        volinfo_t v[MAX_VOLS];
        int nv = disk_volumes(n, v, MAX_VOLS);
        fprintf(out, "%s{\"disk\":%d,\"size\":%llu,\"ss\":%u,\"removable\":%s,\"model\":", first ? "" : ",", n,
                (unsigned long long)di.size, di.ss, di.removable ? "true" : "false");
        json_str(out, di.model);
        fprintf(out, ",\"serial\":");
        json_str(out, di.serial);
        fprintf(out, ",\"volumes\":[");
        for (int i = 0; i < nv; i++) {
            fprintf(out, "%s{\"letters\":", i ? "," : "");
            json_str(out, v[i].letters);
            fprintf(out, ",\"fs\":");
            json_str(out, v[i].fs);
            fprintf(out, ",\"label\":");
            json_str(out, v[i].label);
            fprintf(out, ",\"total\":%llu,\"free\":%llu}", (unsigned long long)v[i].total, (unsigned long long)v[i].free);
        }
        fprintf(out, "]}");
        first = 0;
    }
    fprintf(out, "]\n");
    return 0;
}

/* ------------------------------------------------------------------ Exklusiver Zugriff */

struct plat_session {
    HANDLE disk;
    HANDLE vols[MAX_VOLS];
    int    nvols;
};

/* Volumes sperren und abmelden, damit Windows nicht dazwischen schreibt. Hält ein Programm Dateien offen, wird das
 * Volume trotzdem abgemeldet (wie "format" nach Rückfrage) - die Rückfrage stellt das PC-Programm vorher. */
static int lock_volumes(session_t *s, char *err, size_t n) {
    volinfo_t v[MAX_VOLS];
    int nv = disk_volumes(s->info.num, v, MAX_VOLS);
    for (int i = 0; i < nv; i++)   /* Formatieren zerstörte auch den Teil auf dem anderen Datenträger */
        if (v[i].spans) {
            snprintf(err, n, T("Laufwerk %s liegt auch auf einem anderen Datenträger (übergreifendes Volume) – "
                               "abgelehnt."), v[i].letters[0] ? v[i].letters : v[i].label);
            return -1;
        }
    for (int i = 0; i < nv; i++) {
        WCHAR bare[64];
        strip_slash(v[i].guid, bare, 64);
        HANDLE h = CreateFileW(bare, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            win_error(err, n, v[i].letters[0] ? v[i].letters : N_("Volume öffnen"));
            return -1;
        }
        s->p->vols[s->p->nvols++] = h;
        DWORD r;
        int locked = 0;
        for (int t = 0; t < 10 && !locked; t++) {
            locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &r, NULL);
            if (!locked) Sleep(200);
        }
        DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &r, NULL);
        if (!locked && !DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &r, NULL)) {
            snprintf(err, n, T("Laufwerk %s wird gerade benutzt. Bitte Explorer-Fenster und Programme schließen, die "
                             "darauf zugreifen, und es noch einmal versuchen."), v[i].letters[0] ? v[i].letters : "");
            return -1;
        }
    }
    return 0;
}

void plat_session_close(session_t *s, int changed) {
    DWORD r;
    plat_session_t *p = s->p;
    if (!p) return;
    if (p->disk != INVALID_HANDLE_VALUE && p->disk) FlushFileBuffers(p->disk);
    for (int i = 0; i < p->nvols; i++) {
        DeviceIoControl(p->vols[i], FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &r, NULL);
        CloseHandle(p->vols[i]);
    }
    p->nvols = 0;
    if (p->disk != INVALID_HANDLE_VALUE && p->disk) {
        if (changed) DeviceIoControl(p->disk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &r, NULL);
        if (s->fd >= 0) _close(s->fd);   /* schließt auch das Handle */
        else CloseHandle(p->disk);
    }
    p->disk = INVALID_HANDLE_VALUE;
    s->fd = -1;
}

#ifdef USBFMT_TEST
static const WCHAR *image_path;   /* nur Test-Build: --image <Datei> statt eines Datenträgers (nie ausgeliefert) */
int plat_image_mode(void) { return image_path != NULL; }
#else
int plat_image_mode(void) { return 0; }
#endif

/* "list" nennt Datenträger bei ihrer Nummer: nur Ziffern, sonst -1 */
static int disk_number(const char *disk) {
    char *e;
    long v = strtol(disk, &e, 10);
    return *disk && !*e && v >= 0 && v < MAX_DISKS ? (int)v : -1;
}

int plat_session_open(session_t *s, const char *diskid, uint64_t size, int write, char *err, size_t n) {
    static plat_session_t ps;
    memset(s, 0, sizeof(*s));
    memset(&ps, 0, sizeof(ps));
    s->p = &ps;
    ps.disk = INVALID_HANDLE_VALUE;
    s->fd = s->tfd = -1;
    const int disk = disk_number(diskid);
#ifdef USBFMT_TEST
    if (image_path) {
        LARGE_INTEGER sz;
        s->p->disk = CreateFileW(image_path, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, 0, NULL);
        if (s->p->disk == INVALID_HANDLE_VALUE || !GetFileSizeEx(s->p->disk, &sz) || (uint64_t)sz.QuadPart != size) {
            snprintf(err, n, "%s", T("Abbild nicht passend"));
            plat_session_close(s, 0);
            return -1;
        }
        s->info.size = size;
        s->info.ss = 512;
        snprintf(s->info.model, sizeof(s->info.model), "Testabbild");
        s->fd = s->tfd = _open_osfhandle((intptr_t)s->p->disk, write ? _O_RDWR | _O_BINARY : _O_RDONLY | _O_BINARY);
        return 0;
    }
#endif
    if (disk < 0 || disk_info(disk, &s->info)) { snprintf(err, n, T("Datenträger %d nicht gefunden – bitte die Liste aktualisieren."), disk); return -1; }
    if (!usb_bus(s->info.bus)) { snprintf(err, n, T("Datenträger %d ist kein USB-Datenträger – abgelehnt."), disk); return -1; }
    if (system_disk(disk)) { snprintf(err, n, T("Datenträger %d enthält Windows – abgelehnt."), disk); return -1; }
    if (s->info.size != size) {
        snprintf(err, n, T("Datenträger %d hat sich geändert (andere Größe) – bitte die Liste aktualisieren."), disk);
        return -1;
    }
    if (write && lock_volumes(s, err, n)) { plat_session_close(s, 0); return -1; }
    s->p->disk = open_disk(disk, write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ);
    if (s->p->disk == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) snprintf(err, n, "%s", T("Zugriff verweigert – Administratorrechte nötig."));
        else win_error(err, n, "Datenträger öffnen");
        plat_session_close(s, 0);
        return -1;
    }
    DWORD r;
    if (write && !DeviceIoControl(s->p->disk, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &r, NULL)) {
        snprintf(err, n, "%s", T("Der Datenträger ist schreibgeschützt (Schalter am Stick oder an der SD-Karte?)."));
        plat_session_close(s, 0);
        return -1;
    }
    s->fd = _open_osfhandle((intptr_t)s->p->disk, write ? _O_RDWR | _O_BINARY : _O_RDONLY | _O_BINARY);
    if (s->fd < 0) { snprintf(err, n, "%s", T("Datenträger öffnen fehlgeschlagen.")); plat_session_close(s, 0); return -1; }
    s->tfd = s->fd;   /* Windows liest Datenträger ohne Zwischenspeicher */
    return 0;
}


/* ------------------------------------------------------------------ NTFS und FAT über Windows (fmifs.dll) */

/* FormatEx aus fmifs.dll: nicht dokumentiert, aber seit Windows XP unverändert - so formatieren auch der Explorer und
 * format.com. Nummern der Rückmeldungen wie in ReactOS und Rufus. */
enum {
    FCC_PROGRESS = 0, FCC_INCOMPATIBLE_FILE_SYSTEM = 3, FCC_ACCESS_DENIED = 6, FCC_MEDIA_WRITE_PROTECTED = 7,
    FCC_VOLUME_IN_USE = 8, FCC_CANT_QUICK_FORMAT = 9, FCC_DONE = 11, FCC_BAD_LABEL = 12, FCC_OUTPUT = 14,
    FCC_CLUSTER_SIZE_TOO_SMALL = 16, FCC_CLUSTER_SIZE_TOO_BIG = 17, FCC_VOLUME_TOO_SMALL = 18,
    FCC_VOLUME_TOO_BIG = 19, FCC_NO_MEDIA_IN_DRIVE = 20, FCC_DEVICE_NOT_READY = 24, FCC_READ_ONLY_MODE = 32
};
typedef BOOLEAN (WINAPI *fmifs_cb_t)(int cmd, DWORD action, PVOID data);
typedef VOID (WINAPI *format_ex_t)(PWCHAR root, DWORD media, PWCHAR fs, PWCHAR label, BOOL quick, ULONG cluster,
                                    fmifs_cb_t cb);
typedef struct { DWORD lines; PCHAR text; } fmifs_text_t;   /* FCC_OUTPUT: Meldung im OEM-Zeichensatz */

static struct {
    const char *step;
    int         lo, hi;        /* Bereich des Gesamtfortschritts für diesen Schritt */
    int         done, ok, in_use;
    char        err[200];
    char        out[300];      /* letzte Textmeldung von Windows (UTF-8) - Grund, falls es ohne Fehlercode scheitert */
} fx;

static BOOLEAN WINAPI fx_callback(int cmd, DWORD action, PVOID data) {
    (void)action;
    const char *e;
    switch (cmd) {
    case FCC_PROGRESS: {
        DWORD p = data ? *(const DWORD *)data : 0;
        on_progress(fx.step, fx.lo + (int)(p > 100 ? 100 : p) * (fx.hi - fx.lo) / 100);
        return TRUE;
    }
    case FCC_DONE:
        fx.done = 1;
        fx.ok = data && *(const BOOLEAN *)data;
        return TRUE;
    case FCC_OUTPUT: {
        const fmifs_text_t *t = data;
        WCHAR w[300];
        if (t && t->text && MultiByteToWideChar(CP_OEMCP, 0, t->text, -1, w, 300)) {
            w[299] = 0;
            char u[300];
            if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL)) {
                u[sizeof(u) - 1] = 0;
                char *e = u + strlen(u);
                while (e > u && strchr("\r\n ", e[-1])) *--e = 0;
                char *b = u;
                while (*b == '\r' || *b == '\n' || *b == ' ') b++;
                if (*b) snprintf(fx.out, sizeof(fx.out), "%s", b);
            }
        }
        return TRUE;
    }
    case FCC_VOLUME_IN_USE:
        fx.in_use = 1;
        e = N_("Das Laufwerk wird gerade benutzt. Bitte Explorer-Fenster und Programme schließen, die darauf zugreifen.");
        break;
    case FCC_ACCESS_DENIED:           e = N_("Zugriff verweigert."); break;
    case FCC_MEDIA_WRITE_PROTECTED:   e = N_("Schreibgeschützt oder Dateisystem nicht unterstützt."); break;
    case FCC_READ_ONLY_MODE:          e = N_("Der Datenträger ist schreibgeschützt."); break;
    case FCC_INCOMPATIBLE_FILE_SYSTEM: e = N_("Windows kann dieses Dateisystem hier nicht anlegen."); break;
    case FCC_CANT_QUICK_FORMAT:       e = N_("Schnellformatierung ist hier nicht möglich."); break;
    case FCC_BAD_LABEL:               e = N_("Windows lehnt die Bezeichnung ab."); break;
    case FCC_CLUSTER_SIZE_TOO_SMALL:  e = N_("Die Zuordnungseinheit ist für diese Größe zu klein."); break;
    case FCC_CLUSTER_SIZE_TOO_BIG:    e = N_("Die Zuordnungseinheit ist für diese Größe zu groß."); break;
    case FCC_VOLUME_TOO_SMALL:        e = N_("Der Datenträger ist für dieses Dateisystem zu klein."); break;
    case FCC_VOLUME_TOO_BIG:          e = N_("Der Datenträger ist für dieses Dateisystem zu groß."); break;
    case FCC_NO_MEDIA_IN_DRIVE:
    case FCC_DEVICE_NOT_READY:        e = N_("Der Datenträger ist nicht bereit (abgezogen?)."); break;
    default: return TRUE;   /* Textausgaben, Zwischenstände */
    }
    if (!fx.err[0]) snprintf(fx.err, sizeof(fx.err), "%s", T(e));
    return FALSE;           /* abbrechen */
}

/* Volume auf Datenträger disk mit diesem Dateisystem (und dieser Seriennummer, falls != 0), sobald Windows es nach
 * der neuen Partitionstabelle eingebunden hat */
static int wait_volume(int disk, const char *fsname, DWORD serial, volinfo_t *v, int seconds) {
    for (int t = 0; t < seconds * 4; t++) {
        volinfo_t vs[MAX_VOLS];
        int n = disk_volumes(disk, vs, MAX_VOLS);
        for (int i = 0; i < n; i++)
            if (!_stricmp(vs[i].fs, fsname) && (!serial || vs[i].serial == serial)) { *v = vs[i]; return 0; }
        Sleep(250);
    }
    return -1;
}

/* Explorer & Co. das frisch eingebundene Volume entziehen: abmelden - beim nächsten Zugriff bindet Windows es neu ein,
 * FormatEx sperrt es dann selbst */
static void dismount(const volinfo_t *v) {
    WCHAR bare[64];
    strip_slash(v->guid, bare, 64);
    HANDLE h = CreateFileW(bare, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD r;
    DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &r, NULL);
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &r, NULL);
    CloseHandle(h);
}

/* Partitionstyp im MBR (Kontrolle für die Ergebnis-Daten), -1 = GPT/unbekannt */
int plat_read_mbr_type(const diskinfo_t *di) {
    if (plat_image_mode()) return -1;
    const uint32_t ss = di->ss;
    HANDLE h = open_disk(di->num, GENERIC_READ);
    if (h == INVALID_HANDLE_VALUE) return -1;
    uint8_t *b = malloc(ss);
    DWORD r = 0;
    int t = -1;
    if (b && ReadFile(h, b, ss, &r, NULL) && r >= 512 && b[510] == 0x55 && b[511] == 0xAA && b[450] != 0xEE) t = b[450];
    free(b);
    CloseHandle(h);
    return t;
}

/* Das vorläufige exFAT (Seriennummer serial) mit Windows' FormatEx in NTFS bzw. FAT umformatieren; Fortschritt im
 * Bereich lo..hi. summary erhält z. B. "NTFS, 4 KB Cluster". */
static int windows_format(const diskinfo_t *di, int fs, const char *label, uint32_t cluster, DWORD serial,
                          int lo, int hi, char *summary, size_t sn, char *err, size_t n) {
    HMODULE m = LoadLibraryExW(L"fmifs.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    format_ex_t format_ex = m ? (format_ex_t)(void *)GetProcAddress(m, "FormatEx") : NULL;
    if (!format_ex) { snprintf(err, n, "%s", T("Die Formatierfunktion von Windows (fmifs.dll) fehlt.")); return -1; }
    const char *name = fs == FS_NTFS ? "NTFS" : "FAT";
    volinfo_t v;
    on_progress("Warten, bis Windows die Partition einbindet", lo);
    if (wait_volume(di->num, "exFAT", serial, &v, 30)) {
        snprintf(err, n, "%s", T("Windows hat die neue Partition nicht eingebunden."));
        return -1;
    }
    WCHAR wfs[8], wlabel[40];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wfs, 8);
    if (!MultiByteToWideChar(CP_UTF8, 0, label, -1, wlabel, 40)) wlabel[0] = 0;
    DWORD media = di->media == RemovableMedia || di->media == FixedMedia ? (DWORD)di->media
                : di->removable ? RemovableMedia : FixedMedia;
    /* FormatEx will den GUID-Pfad OHNE \ am Ende (mit \ endet es sofort erfolglos - am echten Stick gesehen, Rufus
     * vermerkt dasselbe); GetVolumeInformationW unten braucht ihn dagegen MIT \ */
    WCHAR bare[64];
    strip_slash(v.guid, bare, 64);
    for (int attempt = 0;; attempt++) {
        memset(&fx, 0, sizeof(fx));
        fx.step = fs == FS_NTFS ? N_("NTFS schreiben") : N_("FAT schreiben");
        fx.lo = lo + (hi - lo) / 10;
        fx.hi = hi - (hi - lo) / 20;
        on_progress(fx.step, fx.lo);
        dismount(&v);
        format_ex(bare, media, wfs, wlabel, TRUE, cluster, fx_callback);
        if (fx.done && fx.ok && !fx.err[0]) break;
        if (!fx.in_use || attempt == 2) {
            if (fx.err[0]) snprintf(err, n, "%s", fx.err);
            else if (fx.out[0]) snprintf(err, n, T("Windows hat das Formatieren nicht abgeschlossen: %s"), fx.out);
            else snprintf(err, n, "%s", T("Windows hat das Formatieren nicht abgeschlossen."));
            return -1;
        }
        Sleep(1500);   /* Explorer hatte das Laufwerk gerade geöffnet: kurz warten, erneut */
    }
    /* dasselbe Volume (GUID bleibt) muss jetzt das neue Dateisystem melden - bei mehreren Partitionen eindeutig */
    on_progress("Ergebnis prüfen", hi - (hi - lo) / 20);
    WCHAR got[16] = L"";
    for (int t = 0; t < 60; t++) {
        if (GetVolumeInformationW(v.guid, NULL, 0, NULL, NULL, NULL, got, 16) && !_wcsicmp(got, wfs)) break;
        got[0] = 0;
        Sleep(250);
    }
    if (!got[0]) { snprintf(err, n, "%s", T("Windows meldet das neue Dateisystem nicht.")); return -1; }
    DWORD spc = 0, bps = 0, nfree = 0, ntotal = 0;
    char cs[24];
    if (!GetDiskFreeSpaceW(v.guid, &spc, &bps, &nfree, &ntotal)) spc = bps = 0;
    const char *kind = fs == FS_NTFS ? "NTFS" : ntotal && ntotal < 4085 ? "FAT12" : "FAT16";
    if (spc && bps) snprintf(summary, sn, T("%s, %s Cluster"), kind, size_text((uint64_t)spc * bps, cs, sizeof(cs)));
    else snprintf(summary, sn, "%s", kind);
    on_progress(fx.step, hi);
    return 0;
}

/* ------------------------------------------------------------------ ext2/ext3/ext4 über mke2fs.exe */

#if __has_include("mke2fs_hash.h")
#include "mke2fs_hash.h"      /* von build_helper.ps1: MKE2FS_SHA256 der mitgelieferten mke2fs.exe */
#endif

#ifdef MKE2FS_SHA256
/* SHA-256 einer geöffneten Datei (bcrypt.dll nur aus System32) */
static int file_sha256(HANDLE f, char hex[65]) {
    typedef LONG (WINAPI *open_fn)(void **, LPCWSTR, LPCWSTR, ULONG);
    typedef LONG (WINAPI *create_fn)(void *, void **, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    typedef LONG (WINAPI *data_fn)(void *, PUCHAR, ULONG, ULONG);
    typedef LONG (WINAPI *finish_fn)(void *, PUCHAR, ULONG, ULONG);
    typedef LONG (WINAPI *close_fn)(void *, ULONG);
    typedef LONG (WINAPI *destroy_fn)(void *);
    HMODULE m = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return -1;
    open_fn op = (open_fn)(void *)GetProcAddress(m, "BCryptOpenAlgorithmProvider");
    create_fn cr = (create_fn)(void *)GetProcAddress(m, "BCryptCreateHash");
    data_fn hd = (data_fn)(void *)GetProcAddress(m, "BCryptHashData");
    finish_fn fin = (finish_fn)(void *)GetProcAddress(m, "BCryptFinishHash");
    close_fn cl = (close_fn)(void *)GetProcAddress(m, "BCryptCloseAlgorithmProvider");
    destroy_fn ds = (destroy_fn)(void *)GetProcAddress(m, "BCryptDestroyHash");
    if (!op || !cr || !hd || !fin || !cl || !ds) return -1;
    void *alg = NULL, *h = NULL;
    int rc = -1;
    UCHAR dig[32];
    static UCHAR buf[64 * 1024];
    if (op(&alg, L"SHA256", NULL, 0) < 0) return -1;
    if (cr(alg, &h, NULL, 0, NULL, 0, 0) >= 0) {
        LARGE_INTEGER z = {0};
        SetFilePointerEx(f, z, NULL, FILE_BEGIN);
        DWORD got;
        rc = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got)
            if (hd(h, buf, got, 0) < 0) { rc = -1; break; }
        if (!rc && fin(h, dig, 32, 0) < 0) rc = -1;
        ds(h);
    }
    cl(alg, 0);
    if (!rc) for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", dig[i]);
    return rc;
}
#endif

/* ext2/3/4 mit mke2fs.exe (neben dem Helfer, vorher per Prüfsumme kontrolliert) in den Bereich offset..offset+bytes
 * von dev schreiben (\\.\PhysicalDriveN bzw. im Test eine Abbild-Datei). Fortschritt lo..hi. */
static int run_mke2fs(const WCHAR *dev, uint64_t offset, uint64_t bytes, const char *type, uint32_t block,
                      const char *label, int lo, int hi, char *summary, size_t sn, char *err, size_t n) {
#ifndef MKE2FS_SHA256
    (void)dev; (void)offset; (void)bytes; (void)type; (void)block; (void)label; (void)lo; (void)hi; (void)summary;
    (void)sn;
    snprintf(err, n, "%s", T("Diese Fassung kann kein ext2/ext3/ext4 (mke2fs.exe fehlt)."));
    return -1;
#else
    WCHAR exe[MAX_PATH + 16];
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR *slash = len && len < MAX_PATH ? wcsrchr(exe, L'\\') : NULL;
    if (!slash) { snprintf(err, n, "%s", T("Ordner des Helfers unbekannt.")); return -1; }
    wcscpy(slash + 1, L"mke2fs.exe");
    /* nur lesend teilen: bis der Prozess läuft, kann niemand die Datei austauschen */
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { snprintf(err, n, "%s", T("mke2fs.exe fehlt neben dem Helfer.")); return -1; }
    char hex[65] = "";
    if (file_sha256(f, hex) || strcmp(hex, MKE2FS_SHA256)) {
        CloseHandle(f);
        snprintf(err, n, "%s", T("mke2fs.exe wurde verändert – aus Sicherheitsgründen nicht gestartet."));
        return -1;
    }

    WCHAR wlabel[40] = L"", opt[160] = L"", cmd[2048], env[700], win[MAX_PATH];
    if (label && *label) MultiByteToWideChar(CP_UTF8, 0, label, -1, wlabel, 40);
    if (block) swprintf(opt, 160, L" -b %u", block);
    if (wlabel[0]) {
        size_t ol = wcslen(opt);
        swprintf(opt + ol, 160 - ol, L" -L \"%ls\"", wlabel);
    }
    swprintf(cmd, 2048, L"\"%ls\" -F -F -t %hs -E offset=%llu,lazy_itable_init=1,lazy_journal_init=1,nodiscard%ls "
             L"\"%ls\" %lluk", exe, type, (unsigned long long)offset, opt, dev, (unsigned long long)(bytes / 1024));
    /* eigene, kleine Umgebung: keine mke2fs.conf von irgendwo (MKE2FS_CONFIG zeigt ins Leere -> eingebaute Werte) */
    if (!GetWindowsDirectoryW(win, MAX_PATH)) wcscpy(win, L"C:\\Windows");
    int el = swprintf(env, 700, L"MKE2FS_CONFIG=%ls\\System32\\usbfmt-keine-mke2fs.conf", win);
    el += 1 + swprintf(env + el + 1, 700 - el - 2, L"SystemRoot=%ls", win);
    env[el + 1] = 0;

    static char out[32768];
    size_t olen = 0;
    int ok = 0;
    for (int attempt = 0; attempt < 5 && !ok; attempt++) {
        SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
        HANDLE rd = NULL, wr = NULL;
        if (!CreatePipe(&rd, &wr, &sa, 0)) { CloseHandle(f); win_error(err, n, "Pipe"); return -1; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        STARTUPINFOW si;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = wr;
        si.hStdError = wr;
        PROCESS_INFORMATION pi;
        /* stirbt der Helfer, stirbt mke2fs mit (kein halb fertiges Dateisystem im Hintergrund) */
        HANDLE job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
        memset(&jl, 0, sizeof(jl));
        jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
        BOOL started = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                      CREATE_SUSPENDED, env, NULL, &si, &pi);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!started) {
            CloseHandle(rd);
            if (job) CloseHandle(job);
            CloseHandle(f);
            win_error(err, n, "mke2fs starten");
            return -1;
        }
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);

        /* Ausgabe lesen: Fortschritt steht in Zeilen und hinter Rückschritten ("Writing inode tables: 3/49\b\b\b") */
        const char *step = NULL;
        int step_pct = 0;
        char seg[256], buf[4096];
        size_t sl = 0;
        DWORD got;
        olen = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) {
            for (DWORD i = 0; i < got; i++) {
                char c = buf[i];
                if (olen < sizeof(out) - 1) out[olen++] = c;
                if (c != '\n' && c != '\r' && c != '\b') { if (sl < sizeof(seg) - 1) seg[sl++] = c; continue; }
                seg[sl] = 0;
                if (sl) {
                    int pct;
                    unsigned a, b;
                    const char *st = extfs_step(FS_EXT4, seg, &pct);
                    if (st) { step = st; step_pct = pct; on_progress(st, lo + pct * (hi - lo) / 100); }
                    else if (step && sscanf(seg, " %u/%u", &a, &b) == 2 && b && a <= b) {
                        int pc = step_pct + (step_pct < 80 ? 20 : 100 - step_pct) * (int)a / (int)b;
                        on_progress(step, lo + pc * (hi - lo) / 100);
                    }
                }
                sl = 0;
            }
        }
        out[olen] = 0;
        CloseHandle(rd);
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        ok = code == 0 && extfs_succeeded(FS_EXT4, out);
        /* nur "belegt" lohnt einen neuen Versuch. Öffnet Windows gerade eine eben angelegte Partition (FormatEx,
         * Explorer), scheitert schon das Öffnen des Datenträgers - mke2fs meldet das als "malformed device name"
         * (am echten Stick gesehen: FAT16 per FormatEx, direkt danach ext3 auf Partition 2) */
        if (!ok && !strstr(out, "busy") && !strstr(out, "in use") && !strstr(out, "malformed device name")) break;
        if (!ok) Sleep(2000);
    }
    CloseHandle(f);
    if (!ok) {
        /* letzte Textzeile als Grund */
        char *e = out + strlen(out);
        while (e > out && strchr("\r\n\b ", e[-1])) *--e = 0;
        char *ln = e;
        while (ln > out && ln[-1] != '\n' && ln[-1] != '\r' && ln[-1] != '\b') ln--;
        snprintf(err, n, "mke2fs: %s", *ln ? ln : T("unbekannter Fehler"));
        return -1;
    }
    unsigned long long blocks = 0;
    unsigned kb = block ? block / 1024 : 0;
    const char *p = strstr(out, "Creating filesystem with ");
    if (!kb && p) sscanf(p + 25, "%llu %uk", &blocks, &kb);
    snprintf(summary, sn, T("%s, %u KB Blöcke"), type, kb ? kb : 4);
    return 0;
#endif
}

int plat_os_format(const diskinfo_t *di, int fs, int index, uint64_t start, uint64_t count, const char *label,
                   uint32_t cluster, uint32_t serial, int lo, int hi, char *summary, size_t sn, char *err, size_t n) {
    (void)index; (void)start; (void)count;   /* Windows findet das Volume am vorläufigen exFAT (serial) */
    return windows_format(di, fs, label, cluster, (DWORD)serial, lo, hi, summary, sn, err, n);
}

int plat_mke2fs(const diskinfo_t *di, int index, uint64_t start, uint64_t count, const char *type, uint32_t block,
                const char *label, int lo, int hi, char *summary, size_t sn, char *err, size_t n) {
    (void)index;
    WCHAR dev[MAX_PATH];
#ifdef USBFMT_TEST
    if (image_path) { wcsncpy(dev, image_path, MAX_PATH - 1); dev[MAX_PATH - 1] = 0; }
    else
#endif
        swprintf(dev, MAX_PATH, L"\\\\.\\PhysicalDrive%d", di->num);
    return run_mke2fs(dev, start * di->ss, count * di->ss, type, block, label, lo, hi, summary, sn, err, n);
}

/* ------------------------------------------------------------------ Umbenennen */

static int is_windows_fs(const char *fs) { return !strcmp(fs, "NTFS") || !strcmp(fs, "FAT"); }

/* NTFS und FAT16 benennt Windows selbst um (das Volume bleibt dabei eingebunden). Dieselben Prüfungen wie beim
 * Öffnen zum Schreiben: nur USB, nie die Systemplatte, Größe unverändert. */
static int label_by_windows(int disk, uint64_t size, const char *label, diskinfo_t *di, char *err, size_t n) {
    if (disk < 0 || disk_info(disk, di) || !usb_bus(di->bus) || system_disk(disk) || di->size != size) {
        snprintf(err, n, T("Datenträger %d hat sich geändert – bitte die Liste aktualisieren."), disk);
        return -1;
    }
    volinfo_t v[MAX_VOLS];
    int nv = disk_volumes(disk, v, MAX_VOLS), hit = -1;
    for (int i = 0; i < nv; i++) {
        if (!is_windows_fs(v[i].fs)) continue;
        if (hit != -1) { snprintf(err, n, "%s", T("Mehrere NTFS-/FAT-Laufwerke auf dem Datenträger.")); return -1; }
        hit = i;
    }
    if (hit < 0) { snprintf(err, n, "%s", T("Kein NTFS- oder FAT-Laufwerk gefunden.")); return -1; }
    const int ntfs = !strcmp(v[hit].fs, "NTFS");
    if (!label_valid(label, ntfs ? FS_NTFS : FS_FAT32)) {
        snprintf(err, n, T("Ungültige Bezeichnung: max. %d Zeichen, nur A-Z, 0-9, Leerzeichen, - und _"), ntfs ? 32 : 11);
        return -1;
    }
    WCHAR wl[40];
    if (!MultiByteToWideChar(CP_UTF8, 0, label, -1, wl, 40)) wl[0] = 0;
    if (!SetVolumeLabelW(v[hit].guid, label[0] ? wl : NULL)) { win_error(err, n, "Bezeichnung setzen"); return -1; }
    return 0;
}

int plat_os_label(const char *diskid, uint64_t size, const char *label, diskinfo_t *di, int *handled, char *err,
                  size_t n) {
    const int disk = disk_number(diskid);
    *handled = 0;
    if (disk < 0) return 0;   /* das Öffnen meldet den Fehler */
    volinfo_t v[MAX_VOLS];
    int nv = disk_volumes(disk, v, MAX_VOLS), windows = 0;
    for (int i = 0; i < nv; i++) windows |= is_windows_fs(v[i].fs);
    if (!windows) return 0;
    *handled = 1;
    return label_by_windows(disk, size, label, di, err, n);
}

/* ------------------------------------------------------------------ Dateien der Image-Sicherung */

static int wide(const char *path, WCHAR *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n) > 0; }

FILE *plat_fopen(const char *path, int write, char *err, size_t n) {
    WCHAR w[1024];
    if (!wide(path, w, 1024)) { snprintf(err, n, "%s", T("Ungültiger Dateiname.")); return NULL; }
    HANDLE h = CreateFileW(w, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, NULL,
                           write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) { win_error(err, n, write ? N_("Datei anlegen") : N_("Datei öffnen")); return NULL; }
    if (GetFileType(h) != FILE_TYPE_DISK) {   /* nur gewöhnliche Dateien - keine Geräte oder Pipes */
        CloseHandle(h);
        snprintf(err, n, "%s", T("Keine gültige Image-Datei (.img.zst)."));
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)h, (write ? _O_WRONLY : _O_RDONLY) | _O_BINARY);
    if (fd < 0) { CloseHandle(h); snprintf(err, n, "%s", T("Datei öffnen")); return NULL; }
    FILE *f = _fdopen(fd, write ? "wb" : "rb");
    if (!f) { _close(fd); snprintf(err, n, "%s", T("Datei öffnen")); }
    return f;
}

int plat_rename(const char *from, const char *to) {
    WCHAR a[1024], b[1024];
    if (!wide(from, a, 1024) || !wide(to, b, 1024)) return -1;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

void plat_remove(const char *path) {
    WCHAR w[1024];
    if (wide(path, w, 1024)) DeleteFileW(w);
}

/* ------------------------------------------------------------------ Start */

/* Ausgabedatei als Administrator anlegen - aber nur NEU und nur genau dort, wo verlangt: Ein Schadprogramm ohne
 * Adminrechte könnte den Ordner sonst per Verknüpfung (Junction) umleiten und uns eine geschützte Datei anlegen oder
 * leeren lassen. CREATE_NEW scheitert an vorhandenen Dateien/Verknüpfungen; danach wird der echte Ort verglichen. */
/* Exit-Codes, wenn die Ausgabedatei nicht angelegt werden kann (das Programm zeigt sie an - ohne Datei gibt es
 * sonst keinen Weg, den Grund zu melden) */
enum { EXIT_OUT_CREATE = 10, EXIT_OUT_PATH = 11, EXIT_OUT_FD = 12 };
static int out_error = EXIT_OUT_CREATE;
static DWORD out_win_error;

static FILE *open_out(const WCHAR *path) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { out_error = EXIT_OUT_CREATE; out_win_error = GetLastError(); return NULL; }
    out_error = EXIT_OUT_PATH;
    WCHAR fin[1024], want[1024], longp[1024];
    DWORD nf = GetFinalPathNameByHandleW(h, fin, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    DWORD nw = GetFullPathNameW(path, 1024, want, NULL);
    DWORD nl = nw && nw < 1024 ? GetLongPathNameW(want, longp, 1024) : 0;   /* 8.3-Kurznamen auflösen */
    const WCHAR *f = fin;
    if (nf > 4 && !wcsncmp(fin, L"\\\\?\\", 4)) f = fin + 4;
    if (!nf || nf >= 1024 || !nl || nl >= 1024 || _wcsicmp(f, longp) != 0) {
        CloseHandle(h);
        if (nf && nf < 1024) DeleteFileW(fin);   /* umgeleitet: die gerade angelegte Datei wieder entfernen */
        return NULL;
    }
    out_error = EXIT_OUT_FD;
    int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(h); return NULL; }
    FILE *fp = _fdopen(fd, "wb");
    if (!fp) _close(fd);
    return fp;
}

int wmain(int argc, WCHAR **wargv) {
    /* Nachgeladene DLLs (fmifs.dll und deren Formatierer untfs.dll, ufat.dll ...) nur aus System32 - nie aus dem
     * Ordner des Helfers (Temp, für jeden beschreibbar), denn der Helfer läuft mit Administratorrechten */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    static char abuf[32][512];
    char *argv[32];
    if (argc > 32) argc = 32;
    for (int i = 0; i < argc; i++) { utf8(wargv[i], abuf[i], sizeof(abuf[i])); argv[i] = abuf[i]; }

    out = stdout;
    int i = 1;
    for (; i + 1 < argc && !strncmp(argv[i], "--", 2); i += 2) {
        if (!strcmp(argv[i], "--out")) {
            FILE *f = open_out(wargv[i + 1]);
            /* Anlegen gescheitert: 10000 + Windows-Fehler (z. B. 10005 = Zugriff verweigert), sonst 11/12 */
            if (!f) return out_error == EXIT_OUT_CREATE ? 10000 + (int)out_win_error : out_error;
            out = f;
        } else if (!strcmp(argv[i], "--cancel")) {
            cancel_path = wargv[i + 1];
        } else if (!strcmp(argv[i], "--lang")) {
            i18n_set(i18n_parse(argv[i + 1]) < 0 ? LANG_DE : i18n_parse(argv[i + 1]));
#ifdef USBFMT_TEST
        } else if (!strcmp(argv[i], "--image")) {
            image_path = wargv[i + 1];
#endif
        } else {
            fprintf(stderr, "Unbekannte Option %s\n", argv[i]);   /* nichts still überspringen */
            return 2;
        }
    }
    if (i >= argc) { fprintf(stderr, "Aufruf: usbfmt_win list | format | layout | speed | label ...\n"); return 2; }
    if (cancel_path) {   /* schon vor dem Start abgebrochen: sofort wissen, nicht erst nach dem ersten Schreiben */
        if (GetFileAttributesW(cancel_path) != INVALID_FILE_ATTRIBUTES) __atomic_store_n(&cancel_requested, 1, __ATOMIC_RELAXED);
        CreateThread(NULL, 0, cancel_watch, NULL, 0, NULL);
    }
    return helper_command(argc - i, argv + i);
}
