/* usbfmt_win.exe - Windows-Helfer des PS5-USB-Formatierers: formatiert USB-Datenträger am PC mit demselben Code wie
 * der PS5-Payload (exFAT 64 KB / FAT32, MBR, ab 2 TiB GPT), misst das Tempo, prüft die Echtheit und benennt um.
 * Für "Am PC Standard" zusätzlich jede Zuordnungseinheit, MBR/GPT nach Wahl sowie NTFS und FAT (FAT16) über die
 * Formatierfunktion von Windows.
 *
 *   usbfmt_win list                                   USB-Datenträger als JSON (ohne Administratorrechte)
 *   usbfmt_win [--out F] [--cancel F] format <disk> <size> <exfat|fat32|ntfs|fat> <label> [verify] [full] [folders]
 *                                     [limit=<B>] [cluster=<B>] [scheme=auto|mbr|gpt] [plain]
 *   usbfmt_win [--out F] [--cancel F] speed  <disk> <size> [plain]
 *   usbfmt_win [--out F] [--cancel F] label  <disk> <size> <label>
 *
 * <size> ist die Größe aus "list": Passt sie nicht mehr (Stick getauscht), wird nichts angefasst. Ausgabe (UTF-8,
 * zeilenweise, nach F oder stdout): "P<TAB>prozent<TAB>schritt" und zum Schluss "R<TAB>0|1<TAB>meldung<TAB>json".
 * Existiert die Datei --cancel, werden Prüfung und Tempomessung abgebrochen. Nur USB- und SD-Datenträger, nie die
 * Systemplatte. cluster: Zuordnungseinheit in Bytes (fehlt/0 = Standard). plain: Meldungen ohne PS5-Bezug. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>

#include "../devtest.h"
#include "../extfs.h"
#include "../i18n.h"
#include "../mkfs.h"
#include "../mkfs_int.h"
#include "../vollabel.h"

#undef strerror

#define MIB         (1024ull * 1024ull)
#define MAX_DISKS   64
#define MAX_VOLS    16

static FILE *out;
static const WCHAR *cancel_path;   /* Unicode: Temp-Pfade mit Umlauten (C:\Users\Jürgen\...) */
static volatile int cancel_requested;
static const char *const TEMPLATE_DIRS[] = {"homebrew", "etaHEN/games", "pkg", NULL};

/* ------------------------------------------------------------------ Hilfen */

static void utf8(const WCHAR *w, char *buf, size_t n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL)) buf[0] = 0;
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static const char *size_text(uint64_t b, char *buf, size_t n) {
    static const char *const unit[] = {N_("Bytes"), N_("KB"), N_("MB"), N_("GB"), N_("TB")};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    snprintf(buf, n, i ? "%.1f %s" : "%.0f %s", v, T(unit[i]));
    char *dot = strchr(buf, '.');
    if (dot && i18n_get() != LANG_EN) *dot = ',';   /* Dezimalkomma außer im Englischen */
    return buf;
}

static const char *rating_text(double read_mbps) {
    const char *r = dt_rating(read_mbps);
    return !strcmp(r, "schnell") ? T("schnell genug für Spiele") : !strcmp(r, "ausreichend")
           ? T("für Spiele ausreichend") : T("langsam – Spiele laden länger");
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

/* step ist der deutsche Schrittname (oder schon übersetzt) - ausgegeben wird er in der gewählten Sprache */
static void on_progress(const char *step, int pct) {
    static char last_step[96];
    step = T(step);
    static int last_pct = -1;
    if (pct == last_pct && !strcmp(step, last_step)) return;
    last_pct = pct;
    snprintf(last_step, sizeof(last_step), "%s", step);
    fprintf(out, "P\t%d\t%s\n", pct, step);
    fflush(out);
}

static void finish(int ok, const char *msg, const char *json) {
    fprintf(out, "R\t%d\t%s\t%s\n", ok, T(msg), json && *json ? json : "{}");
    fflush(out);
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
    int      disk;
    uint64_t size;
    uint32_t ss;
    int      bus;          /* STORAGE_BUS_TYPE */
    int      removable;
    DWORD    media;        /* MEDIA_TYPE: RemovableMedia oder FixedMedia (für FormatEx) */
    char     model[128];
} diskinfo_t;

typedef struct {
    WCHAR    guid[64];     /* \\?\Volume{...}\ */
    char     letters[32];  /* "E:" bzw. "E: F:" */
    char     fs[16], label[64];
    DWORD    serial;       /* Seriennummer des Dateisystems */
    uint64_t total, free;
} volinfo_t;

static int usb_bus(int bus) { return bus == BusTypeUsb || bus == BusTypeSd || bus == BusTypeMmc; }

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
    di->disk = n;
    HANDLE h = open_disk(n, 0);
    if (h == INVALID_HANDLE_VALUE) return -1;
    BYTE buf[1024];
    DWORD r;
    int ok = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, buf, sizeof(buf), &r, NULL)) {
        const DISK_GEOMETRY_EX *g = (const DISK_GEOMETRY_EX *)buf;
        di->size = (uint64_t)g->DiskSize.QuadPart;
        di->ss = g->Geometry.BytesPerSector;
        di->media = (DWORD)g->Geometry.MediaType;
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
    }
    CloseHandle(h);
    return ok ? 0 : -1;
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

static int cmd_list(void) {
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

typedef struct {
    diskinfo_t info;
    HANDLE     disk;
    int        fd;
    HANDLE     vols[MAX_VOLS];
    int        nvols;
} session_t;

/* Volumes sperren und abmelden, damit Windows nicht dazwischen schreibt. Hält ein Programm Dateien offen, wird das
 * Volume trotzdem abgemeldet (wie "format" nach Rückfrage) - die Rückfrage stellt das PC-Programm vorher. */
static int lock_volumes(session_t *s, char *err, size_t n) {
    volinfo_t v[MAX_VOLS];
    int nv = disk_volumes(s->info.disk, v, MAX_VOLS);
    for (int i = 0; i < nv; i++) {
        WCHAR bare[64];
        strip_slash(v[i].guid, bare, 64);
        HANDLE h = CreateFileW(bare, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            win_error(err, n, v[i].letters[0] ? v[i].letters : N_("Volume öffnen"));
            return -1;
        }
        s->vols[s->nvols++] = h;
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

static void session_close(session_t *s, int changed) {
    DWORD r;
    if (s->disk != INVALID_HANDLE_VALUE && s->disk) FlushFileBuffers(s->disk);
    for (int i = 0; i < s->nvols; i++) {
        DeviceIoControl(s->vols[i], FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &r, NULL);
        CloseHandle(s->vols[i]);
    }
    s->nvols = 0;
    if (s->disk != INVALID_HANDLE_VALUE && s->disk) {
        if (changed) DeviceIoControl(s->disk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &r, NULL);
        if (s->fd >= 0) _close(s->fd);   /* schließt auch das Handle */
        else CloseHandle(s->disk);
    }
    s->disk = INVALID_HANDLE_VALUE;
    s->fd = -1;
}

#ifdef USBFMT_TEST
static const WCHAR *image_path;   /* nur Test-Build: --image <Datei> statt eines Datenträgers (nie ausgeliefert) */
static int image_mode(void) { return image_path != NULL; }
#else
static int image_mode(void) { return 0; }
#endif

static int session_open(session_t *s, int disk, uint64_t size, int write, char *err, size_t n) {
    memset(s, 0, sizeof(*s));
    s->disk = INVALID_HANDLE_VALUE;
    s->fd = -1;
#ifdef USBFMT_TEST
    if (image_path) {
        LARGE_INTEGER sz;
        s->disk = CreateFileW(image_path, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, 0, NULL);
        if (s->disk == INVALID_HANDLE_VALUE || !GetFileSizeEx(s->disk, &sz) || (uint64_t)sz.QuadPart != size) {
            snprintf(err, n, "%s", T("Abbild nicht passend"));
            session_close(s, 0);
            return -1;
        }
        s->info.size = size;
        s->info.ss = 512;
        snprintf(s->info.model, sizeof(s->info.model), "Testabbild");
        s->fd = _open_osfhandle((intptr_t)s->disk, write ? _O_RDWR | _O_BINARY : _O_RDONLY | _O_BINARY);
        return 0;
    }
#endif
    if (disk_info(disk, &s->info)) { snprintf(err, n, T("Datenträger %d nicht gefunden – bitte die Liste aktualisieren."), disk); return -1; }
    if (!usb_bus(s->info.bus)) { snprintf(err, n, T("Datenträger %d ist kein USB-Datenträger – abgelehnt."), disk); return -1; }
    if (system_disk(disk)) { snprintf(err, n, T("Datenträger %d enthält Windows – abgelehnt."), disk); return -1; }
    if (s->info.size != size) {
        snprintf(err, n, T("Datenträger %d hat sich geändert (andere Größe) – bitte die Liste aktualisieren."), disk);
        return -1;
    }
    if (write && lock_volumes(s, err, n)) { session_close(s, 0); return -1; }
    s->disk = open_disk(disk, write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ);
    if (s->disk == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) snprintf(err, n, "%s", T("Zugriff verweigert – Administratorrechte nötig."));
        else win_error(err, n, "Datenträger öffnen");
        session_close(s, 0);
        return -1;
    }
    DWORD r;
    if (write && !DeviceIoControl(s->disk, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &r, NULL)) {
        snprintf(err, n, "%s", T("Der Datenträger ist schreibgeschützt (Schalter am Stick oder an der SD-Karte?)."));
        session_close(s, 0);
        return -1;
    }
    s->fd = _open_osfhandle((intptr_t)s->disk, write ? _O_RDWR | _O_BINARY : _O_RDONLY | _O_BINARY);
    if (s->fd < 0) { snprintf(err, n, "%s", T("Datenträger öffnen fehlgeschlagen.")); session_close(s, 0); return -1; }
    return 0;
}

static const char *title(const session_t *s) { return s->info.model[0] ? s->info.model : T("USB-Datenträger"); }

/* ------------------------------------------------------------------ Aufträge */

/* ------------------------------------------------------------------ NTFS und FAT über Windows (fmifs.dll) */

enum { WFS_FAT16 = 100 };   /* nur im Helfer: FAT (FAT12/FAT16) schreibt Windows selbst */

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
static int read_mbr_type(int disk, uint32_t ss) {
    HANDLE h = open_disk(disk, GENERIC_READ);
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
    if (wait_volume(di->disk, "exFAT", serial, &v, 30)) {
        snprintf(err, n, "%s", T("Windows hat die neue Partition nicht eingebunden."));
        return -1;
    }
    WCHAR wfs[8], wlabel[40];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wfs, 8);
    if (!MultiByteToWideChar(CP_UTF8, 0, label, -1, wlabel, 40)) wlabel[0] = 0;
    DWORD media = di->media == RemovableMedia || di->media == FixedMedia ? di->media
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
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
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
        if (!ok && !strstr(out, "busy") && !strstr(out, "in use")) break;   /* nur "belegt" lohnt einen neuen Versuch */
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

/* ------------------------------------------------------------------ Aufträge */

enum { WFS_EXT2 = 101, WFS_EXT3 = 102, WFS_NONE = 103 };   /* weitere nur im Helfer */

typedef struct {
    int      fs;          /* FS_EXFAT, FS_FAT32, FS_NTFS, FS_EXT4, FS_HFSPLUS, WFS_FAT16, WFS_EXT2, WFS_EXT3, WFS_NONE */
    uint64_t bytes;       /* 0 = Rest */
    uint32_t cluster;     /* 0 = Standard */
    int      ptype, active;
    char     label[40];
} wpart_t;

typedef struct {
    int      verify, full, folders, plain;
    int      scheme;      /* LAYOUT_* */
    uint64_t limit;
    int      n;
    wpart_t  p[MAX_PARTS];
} fmtreq_t;

static int is_ext(int fs) { return fs == FS_EXT4 || fs == WFS_EXT2 || fs == WFS_EXT3; }
static int by_windows(int fs) { return fs == FS_NTFS || fs == WFS_FAT16; }
static const char *ext_name(int fs) { return fs == WFS_EXT2 ? "ext2" : fs == WFS_EXT3 ? "ext3" : "ext4"; }
static const char *fs_title(int fs) {
    return fs == FS_EXFAT ? "exFAT" : fs == FS_FAT32 ? "FAT32" : fs == FS_NTFS ? "NTFS" : fs == WFS_FAT16 ? "FAT"
         : fs == FS_HFSPLUS ? "HFS+" : is_ext(fs) ? ext_name(fs) : T("ohne Dateisystem");
}

/* Bezeichnung und Zuordnungseinheit passend zum Dateisystem? (exFAT/FAT32/HFS+ prüft mkfs selbst genauer) */
static const char *check_part(const wpart_t *p) {
    int lfs = p->fs == FS_NTFS ? FS_NTFS : is_ext(p->fs) ? FS_EXT4 : p->fs == FS_HFSPLUS ? FS_HFSPLUS : FS_FAT32;
    if (p->fs != WFS_NONE && !label_valid(p->label, lfs)) return N_("Ungültige Bezeichnung.");
    uint32_t c = p->cluster;
    if (!c) return NULL;
    if (c & (c - 1)) return N_("Ungültige Zuordnungseinheit.");
    if (is_ext(p->fs) && (c < 1024 || c > 4096)) return N_("Blockgröße für ext: 1, 2 oder 4 KB.");
    if (by_windows(p->fs) && (c < 512 || c > (p->fs == FS_NTFS ? 2 * MIB : 64 * 1024))) return N_("Ungültige Zuordnungseinheit.");
    return NULL;
}

/* Baustein der Ergebnismeldung anhängen (fmt: deutscher Text, ausgegeben in der gewählten Sprache) */
static int add_text(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, T(fmt), ap);
    va_end(ap);
    return r < 0 ? 0 : r;
}

/* Fortschritt eines Einzelschritts (mkfs_*) in den Bereich prog_lo..prog_hi einpassen */
static int prog_lo, prog_hi;
static const char *prog_name;
static void on_scaled(const char *step, int pct) {
    on_progress(prog_name ? prog_name : step, prog_lo + pct * (prog_hi - prog_lo) / 100);
}

static int cmd_format(int disk, uint64_t size, fmtreq_t *q) {
    char err[512] = "", msg[1536], json[512], s1[24], s2[24];
    static char summary[MAX_PARTS][160];
    for (int i = 0; i < q->n; i++) {
        const char *e = check_part(&q->p[i]);
        if (e) {
            if (q->n > 1) snprintf(msg, sizeof(msg), T("Partition %d: %s"), i + 1, T(e));
            else snprintf(msg, sizeof(msg), "%s", T(e));
            finish(0, msg, NULL);
            return 1;
        }
        if (by_windows(q->p[i].fs) && image_mode()) {
            finish(0, "NTFS/FAT schreibt Windows - im Abbild-Test nicht möglich.", NULL);
            return 1;
        }
    }
    if (q->limit && (q->limit < 64 * MIB || q->limit > size)) {
        finish(0, "Ungültige Grenze für den echten Bereich.", NULL);
        return 1;
    }
    /* eine exFAT-/FAT32-Partition: genau der Weg der PS5 (auch die Ordnervorlage) */
    const int classic = q->n == 1 && q->p[0].ptype == PT_AUTO && !q->p[0].active && !q->p[0].bytes &&
                        (q->p[0].fs == FS_EXFAT || q->p[0].fs == FS_FAT32);

    session_t s;
    on_progress("Datenträger vorbereiten", 0);
    if (session_open(&s, disk, size, 1, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    const uint64_t use = q->limit ? q->limit : size;

    devtest_t t;
    memset(&t, 0, sizeof(t));
    int tested = 0, test_cancelled = 0;
    if (q->verify || q->full) {
        tested = q->full ? 2 : 1;
        int rc = q->full ? dt_full(s.fd, s.info.ss, use, on_progress, &cancel_requested, &t, err, sizeof(err))
                         : dt_verify(s.fd, s.info.ss, use, on_progress, &cancel_requested, &t, err, sizeof(err));
        if (rc == 0 && t.cancelled) {
            test_cancelled = 1;
        } else if (rc != 0 || t.fake || t.bad) {
            session_close(&s, 1);
            snprintf(json, sizeof(json), "{\"test\":\"%s\",\"fake\":%s,\"bad\":%s,\"reported\":%llu,\"real\":%llu,"
                     "\"bad_at\":%llu}", tested == 2 ? "full" : "quick", t.fake ? "true" : "false",
                     t.bad ? "true" : "false", (unsigned long long)size, (unsigned long long)t.real_bytes,
                     (unsigned long long)t.bad_at);
            if (rc != 0)
                snprintf(msg, sizeof(msg), T("Prüfung fehlgeschlagen: %s Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben."), err);
            else if (t.fake)
                snprintf(msg, sizeof(msg), T("%s meldet %s, hat aber nur etwa %s echten Speicher. Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben – "
                         "%s, höchstens den echten Bereich nutzen."), title(&s), size_text(size, s1, sizeof(s1)),
                         size_text(t.real_bytes, s2, sizeof(s2)),
                         q->plain ? T("für wichtige Daten nicht empfohlen") : T("für Spiele an der PS5 nicht empfohlen"));
            else
                snprintf(msg, sizeof(msg), T("Fehlerhafte Bereiche ab %llu MB: %s arbeitet nicht zuverlässig. "
                         "Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben."), (unsigned long long)(t.bad_at / MIB), title(&s));
            finish(0, msg, json);
            return 1;
        }
    }

    blkdev_t d = {0};
    d.fd = s.fd;
    d.ss = s.info.ss;
    d.sectors = size / s.info.ss;
    d.progress = on_progress;
    mkfs_opts_t o = {0};
    o.limit = q->limit;
    o.scheme = q->scheme;
    o.dirs = q->folders && classic ? TEMPLATE_DIRS : NULL;
    part_t parts[MAX_PARTS];
    uint32_t serial[MAX_PARTS];
    int rc = 0, gpt = 0, later = 0;    /* later: Partitionen, die nach dem Freigeben Windows/mke2fs schreiben */
    memset(parts, 0, sizeof(parts));
    for (int i = 0; i < q->n; i++) { serial[i] = arc4random(); summary[i][0] = 0; }

    if (classic) {
        rc = format_disk(&d, q->p[0].fs, q->p[0].label, serial[0], q->p[0].cluster, &o);
        if (!rc) snprintf(summary[0], sizeof(summary[0]), "%s", mkfs_summary());
        gpt = o.is_gpt;
    } else {
        for (int i = 0; i < q->n; i++) {
            const wpart_t *w = &q->p[i];
            parts[i].fs = w->fs == WFS_FAT16 ? FS_EXFAT : is_ext(w->fs) ? FS_EXT4 : w->fs == WFS_NONE ? 0 : w->fs;
            /* ohne Dateisystem: Typ Linux, sonst zeigte Windows eine leere Partition mit "Formatieren?"-Frage */
            parts[i].ptype = w->ptype != PT_AUTO ? w->ptype : w->fs == WFS_NONE ? PT_LINUX : PT_AUTO;
            parts[i].mbr_type = w->fs == WFS_FAT16 && w->ptype == PT_AUTO ? 0x0E : 0;
            parts[i].active = w->active;
            parts[i].bytes = w->bytes;
            parts[i].name = w->label;
            later += by_windows(w->fs) || is_ext(w->fs);
        }
        rc = layout_disk(&d, q->scheme, q->limit, parts, q->n, serial[0], &gpt);
        /* eigene Dateisysteme jetzt (bei NTFS/FAT ein vorläufiges exFAT), Fortschritt 10 .. 40 bzw. 95 % */
        const int span_hi = later ? 40 : 95;
        for (int i = 0; i < q->n && !rc; i++) {
            const wpart_t *w = &q->p[i];
            if (is_ext(w->fs) || w->fs == WFS_NONE) continue;
            prog_lo = 10 + (span_hi - 10) * i / q->n;
            prog_hi = 10 + (span_hi - 10) * (i + 1) / q->n;
            prog_name = by_windows(w->fs) ? N_("Partition anlegen") : NULL;
            d.progress = on_scaled;
            d.base = parts[i].start * d.ss;
            mkfs_progress_reset();
            if (by_windows(w->fs)) rc = mkfs_exfat(&d, parts[i].start, parts[i].count, "", serial[i], 0, NULL);
            else if (w->fs == FS_HFSPLUS) rc = mkfs_hfsplus(&d, parts[i].count, w->label, serial[i], w->cluster);
            else if (w->fs == FS_EXFAT) rc = mkfs_exfat(&d, parts[i].start, parts[i].count, w->label, serial[i], w->cluster, NULL);
            else rc = mkfs_fat32(&d, parts[i].start, parts[i].count, w->label, serial[i], w->cluster, NULL);
            d.base = 0;
            if (!rc && !by_windows(w->fs)) snprintf(summary[i], sizeof(summary[i]), "%s", mkfs_summary());
            if (rc && q->n > 1) {
                char e2[300];
                snprintf(e2, sizeof(e2), "%s", mkfs_error());
                mk_fail("Partition %d: %s", i + 1, e2);
            }
        }
        mkfs_progress_reset();
        prog_name = NULL;
    }
    session_close(&s, 1);
    if (rc != 0) {
        snprintf(msg, sizeof(msg), T("Formatieren fehlgeschlagen: %s"), mkfs_error());
        finish(0, msg, NULL);
        return 1;
    }

    /* Windows (NTFS/FAT) und mke2fs (ext) schreiben erst jetzt - der Datenträger ist wieder freigegeben */
    int done_later = 0, failed = 0;
    char failmsg[700] = "";
    WCHAR dev[MAX_PATH];
#ifdef USBFMT_TEST
    if (image_path) { wcsncpy(dev, image_path, MAX_PATH - 1); dev[MAX_PATH - 1] = 0; }
    else
#endif
        swprintf(dev, MAX_PATH, L"\\\\.\\PhysicalDrive%d", disk);
    for (int i = 0; i < q->n && !classic; i++) {
        const wpart_t *w = &q->p[i];
        if (!by_windows(w->fs) && !is_ext(w->fs)) continue;
        const int lo = 40 + 55 * done_later / later, hi = 40 + 55 * (done_later + 1) / later;
        done_later++;
        int r = by_windows(w->fs)
              ? windows_format(&s.info, w->fs, w->label, w->cluster, serial[i], lo, hi,
                               summary[i], sizeof(summary[i]), err, sizeof(err))
              : run_mke2fs(dev, parts[i].start * s.info.ss, parts[i].count * s.info.ss, ext_name(w->fs), w->cluster,
                           w->label, lo, hi, summary[i], sizeof(summary[i]), err, sizeof(err));
        if (r) {
            failed++;
            size_t fl = strlen(failmsg);
            if (q->n > 1) snprintf(failmsg + fl, sizeof(failmsg) - fl, T("%sPartition %d (%s): %s"), fl ? " " : "", i + 1,
                                   fs_title(w->fs), err);
            else snprintf(failmsg + fl, sizeof(failmsg) - fl, T("%s konnte nicht angelegt werden: %s"), fs_title(w->fs), err);
        }
    }
    if (failed) {
        snprintf(msg, sizeof(msg), "%s%s", failmsg, q->n == 1 && by_windows(q->p[0].fs)
                 ? T(" Der Datenträger ist jetzt leer und vorläufig mit exFAT formatiert.") : "");
        finish(0, msg, NULL);
        return 1;
    }
    on_progress("Fertig", 100);

    int l;
#define ADD(...) do { if (l < (int)sizeof(msg)) l += add_text(msg + l, sizeof(msg) - l, __VA_ARGS__); } while (0)
    if (q->n == 1) {
        l = snprintf(msg, sizeof(msg), T("%s erfolgreich formatiert: %s"), title(&s),
                     q->p[0].fs == WFS_NONE ? T("Partition ohne Dateisystem") : summary[0]);
        if (q->p[0].label[0]) ADD(", Bezeichnung %s", q->p[0].label);
        if (q->plain) ADD(", Partitionstabelle %s", gpt ? "GPT" : "MBR");
        else if (gpt) ADD(", Partitionstabelle GPT");
    } else {
        l = snprintf(msg, sizeof(msg), T("%s: %d Partitionen angelegt (%s):"), title(&s), q->n, gpt ? "GPT" : "MBR");
        for (int i = 0; i < q->n; i++) {
            ADD(" %d) %s, %s", i + 1, q->p[i].fs == WFS_NONE ? T("ohne Dateisystem") : summary[i],
                size_text(parts[i].count * s.info.ss, s1, sizeof(s1)));
            if (q->p[i].label[0]) ADD(", „%s“", q->p[i].label);
            if (i + 1 < q->n) ADD(";");
        }
    }
    if (q->limit) ADD(", nur die echten %s genutzt", size_text(q->limit, s1, sizeof(s1)));
    if (o.dirs) ADD(", Ordner homebrew, etaHEN/games und pkg angelegt");
    ADD(".");
    int has_ext = 0, has_hfs = 0, first_ext = 0;
    for (int i = q->n - 1; i >= 0; i--) {
        if (is_ext(q->p[i].fs)) { has_ext = 1; first_ext = q->p[i].fs; }
        has_hfs |= q->p[i].fs == FS_HFSPLUS;
    }
    if (has_ext || has_hfs)   /* nur, was wirklich angelegt wurde */
        ADD(" Windows selbst kann %s nicht öffnen – dafür erscheint kein Laufwerksbuchstabe.",
            has_ext && has_hfs ? T("ext und HFS+") : has_hfs ? "HFS+" : q->n > 1 ? "ext" : fs_title(first_ext));
    if (tested && !test_cancelled && t.read_mbps > 0) {
        ADD(" %s: echt%s, lesen %.0f MB/s, schreiben %.0f MB/s", tested == 2 ? T("Jede Stelle geprüft") : T("Prüfung"),
            tested == 2 ? T(", keine Fehler") : "", t.read_mbps, t.write_mbps);
        if (q->plain) ADD(".");
        else ADD(" – %s.", rating_text(t.read_mbps));
    }
    if (test_cancelled) ADD(" Die Prüfung wurde abgebrochen – formatiert wurde trotzdem.");
    if (!q->plain) ADD(" Jetzt einfach abziehen und an die PS5 stecken.");
#undef ADD
    snprintf(json, sizeof(json), "{\"scheme\":\"%s\",\"read\":%.1f,\"write\":%.1f,\"mbr_type\":%d}",
             gpt ? "GPT" : "MBR", t.read_mbps, t.write_mbps, image_mode() ? -1 : read_mbr_type(disk, s.info.ss));
    finish(1, msg, json);
    return 0;
}

static int cmd_speed(int disk, uint64_t size, int plain) {
    char err[512] = "", msg[512], json[128];
    session_t s;
    on_progress("Lesetempo messen", 0);
    if (session_open(&s, disk, size, 0, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    devtest_t t;
    int rc = dt_read_speed(s.fd, s.info.ss, size, on_progress, &cancel_requested, &t, err, sizeof(err));
    session_close(&s, 0);
    if (rc != 0) { snprintf(msg, sizeof(msg), T("Tempo messen fehlgeschlagen: %s"), err); finish(0, msg, NULL); return 1; }
    if (t.cancelled) { finish(0, "Abgebrochen.", NULL); return 1; }
    if (plain) snprintf(msg, sizeof(msg), T("Lesetempo von %s: %.0f MB/s."), title(&s), t.read_mbps);
    else snprintf(msg, sizeof(msg), T("Lesetempo von %s: %.0f MB/s – %s."), title(&s), t.read_mbps, rating_text(t.read_mbps));
    snprintf(json, sizeof(json), "{\"read\":%.1f,\"rating\":\"%s\"}", t.read_mbps, dt_rating(t.read_mbps));
    finish(1, msg, json);
    return 0;
}

static int is_windows_fs(const char *fs) { return !strcmp(fs, "NTFS") || !strcmp(fs, "FAT"); }

/* NTFS und FAT16 benennt Windows selbst um (das Volume bleibt dabei eingebunden). Dieselben Prüfungen wie beim
 * Öffnen zum Schreiben: nur USB, nie die Systemplatte, Größe unverändert. */
static int label_by_windows(int disk, uint64_t size, const char *label, diskinfo_t *di, char *err, size_t n) {
    if (disk_info(disk, di) || !usb_bus(di->bus) || system_disk(disk) || di->size != size) {
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

static int cmd_label(int disk, uint64_t size, const char *label) {
    char err[512] = "", msg[512];
    session_t s;
    on_progress("Bezeichnung schreiben", 30);
    volinfo_t v[MAX_VOLS];
    int nv = image_mode() ? 0 : disk_volumes(disk, v, MAX_VOLS), windows = 0, rc;
    for (int i = 0; i < nv; i++) windows |= is_windows_fs(v[i].fs);
    if (windows) {
        memset(&s, 0, sizeof(s));
        rc = label_by_windows(disk, size, label, &s.info, err, sizeof(err));
    } else {
        if (session_open(&s, disk, size, 1, err, sizeof(err))) { finish(0, err, NULL); return 1; }
        rc = vol_set_label(s.fd, s.info.ss, s.info.size, label, err, sizeof(err));
        session_close(&s, 1);
    }
    if (rc != 0) { snprintf(msg, sizeof(msg), T("Umbenennen fehlgeschlagen: %s"), err); finish(0, msg, NULL); return 1; }
    if (label[0]) snprintf(msg, sizeof(msg), T("Neue Bezeichnung für %s: „%s“."), title(&s), label);
    else snprintf(msg, sizeof(msg), T("Bezeichnung von %s entfernt."), title(&s));
    finish(1, msg, NULL);
    return 0;
}

/* ------------------------------------------------------------------ Befehlszeile */

static int parse_fs(const char *s) {
    static const struct { const char *name; int fs; } map[] = {
        {"exfat", FS_EXFAT}, {"fat32", FS_FAT32}, {"ntfs", FS_NTFS}, {"fat", WFS_FAT16}, {"hfsplus", FS_HFSPLUS},
        {"ext2", WFS_EXT2}, {"ext3", WFS_EXT3}, {"ext4", FS_EXT4}, {"none", WFS_NONE},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(s, map[i].name)) return map[i].fs;
    return 0;
}

static int parse_type(const char *s) {
    static const char *const names[] = {"auto", "windows", "linux", "swap", "efi", "apple"};   /* = PT_* */
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

/* "part=" <fs>,<bytes|rest>,<cluster>,<typ>,<aktiv 0|1>,<bezeichnung> - die Bezeichnung zuletzt (darf Leerzeichen
 * enthalten, nie ein Komma: nur A-Z, 0-9, Leerzeichen, - und _) */
static int parse_part(const char *spec, wpart_t *p) {
    char buf[128], *f[6] = {0};
    if (strlen(spec) >= sizeof(buf)) return -1;
    strcpy(buf, spec);
    char *s = buf;
    for (int i = 0; i < 6; i++) {
        f[i] = s;
        char *c = i < 5 ? strchr(s, ',') : NULL;
        if (i < 5) { if (!c) return -1; *c = 0; s = c + 1; }
    }
    char *end;
    p->fs = parse_fs(f[0]);
    if (!p->fs) return -1;
    if (!strcmp(f[1], "rest")) p->bytes = 0;
    else {
        p->bytes = strtoull(f[1], &end, 10);
        if (*end || !p->bytes) return -1;
    }
    unsigned long long c = strtoull(f[2], &end, 10);
    if (*end || c > 32 * MIB) return -1;
    p->cluster = (uint32_t)c;
    if ((p->ptype = parse_type(f[3])) < 0) return -1;
    if (strcmp(f[4], "0") && strcmp(f[4], "1")) return -1;
    p->active = f[4][0] == '1';
    if (strlen(f[5]) >= sizeof(p->label)) return -1;
    strcpy(p->label, f[5]);
    return 0;
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
    if (i >= argc) { fprintf(stderr, "Aufruf: usbfmt_win list | format | speed | label ...\n"); return 2; }
    if (cancel_path) CreateThread(NULL, 0, cancel_watch, NULL, 0, NULL);

    const char *cmd = argv[i];
    int rest = argc - i - 1;
    char **a = argv + i + 1;
    if (!strcmp(cmd, "list")) return cmd_list();
    if (rest < 2) { finish(0, "Aufruf unvollständig.", NULL); return 2; }
    int disk = atoi(a[0]);
    uint64_t size = strtoull(a[1], NULL, 10);
    if (!strcmp(cmd, "speed")) return cmd_speed(disk, size, rest >= 3 && !strcmp(a[2], "plain"));
    if (!strcmp(cmd, "label") && rest >= 3) return cmd_label(disk, size, a[2]);
    if ((!strcmp(cmd, "format") && rest >= 4) || !strcmp(cmd, "layout")) {
        static fmtreq_t q;
        memset(&q, 0, sizeof(q));
        int k = 2;
        if (!strcmp(cmd, "format")) {   /* eine Partition: format <disk> <size> <fs> <label> [...] */
            q.n = 1;
            q.p[0].fs = parse_fs(a[2]);
            if (!q.p[0].fs || q.p[0].fs == WFS_NONE) { finish(0, "Unbekanntes Dateisystem.", NULL); return 2; }
            if (strlen(a[3]) >= sizeof(q.p[0].label)) { finish(0, "Ungültige Bezeichnung.", NULL); return 2; }
            strcpy(q.p[0].label, a[3]);
            k = 4;
        }
        for (; k < rest; k++) {
            if (!strcmp(a[k], "verify")) q.verify = 1;
            else if (!strcmp(a[k], "full")) q.full = 1;
            else if (!strcmp(a[k], "folders")) q.folders = 1;
            else if (!strcmp(a[k], "plain")) q.plain = 1;
            else if (!strncmp(a[k], "limit=", 6)) q.limit = strtoull(a[k] + 6, NULL, 10);
            else if (!strncmp(a[k], "cluster=", 8) && !strcmp(cmd, "format")) {
                unsigned long long c = strtoull(a[k] + 8, NULL, 10);
                if (c > 32 * MIB) { finish(0, "Ungültige Zuordnungseinheit.", NULL); return 2; }
                q.p[0].cluster = (uint32_t)c;
            } else if (!strcmp(a[k], "scheme=auto")) q.scheme = LAYOUT_AUTO;
            else if (!strcmp(a[k], "scheme=mbr")) q.scheme = LAYOUT_MBR;
            else if (!strcmp(a[k], "scheme=gpt")) q.scheme = LAYOUT_GPT;
            else if (!strncmp(a[k], "part=", 5) && !strcmp(cmd, "layout")) {
                if (q.n >= MAX_PARTS) { finish(0, "Zu viele Partitionen.", NULL); return 2; }
                if (parse_part(a[k] + 5, &q.p[q.n])) { finish(0, "Ungültige Partitionsangabe.", NULL); return 2; }
                q.n++;
            } else { finish(0, "Unbekannte Angabe beim Formatieren.", NULL); return 2; }   /* nichts still übergehen */
        }
        if (!q.n) { finish(0, "Keine Partition angegeben.", NULL); return 2; }
        return cmd_format(disk, size, &q);
    }
    finish(0, "Unbekannter Auftrag.", NULL);
    return 2;
}
