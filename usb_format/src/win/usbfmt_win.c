/* usbfmt_win.exe - Windows-Helfer des PS5-USB-Formatierers: formatiert USB-Datenträger am PC mit demselben Code wie
 * der PS5-Payload (exFAT 64 KB / FAT32, MBR, ab 2 TiB GPT), misst das Tempo, prüft die Echtheit und benennt um.
 *
 *   usbfmt_win list                                   USB-Datenträger als JSON (ohne Administratorrechte)
 *   usbfmt_win [--out F] [--cancel F] format <disk> <size> <exfat|fat32> <label> [verify] [full] [folders] [limit=<B>]
 *   usbfmt_win [--out F] [--cancel F] speed  <disk> <size>
 *   usbfmt_win [--out F] [--cancel F] label  <disk> <size> <label>
 *
 * <size> ist die Größe aus "list": Passt sie nicht mehr (Stick getauscht), wird nichts angefasst. Ausgabe (UTF-8,
 * zeilenweise, nach F oder stdout): "P<TAB>prozent<TAB>schritt" und zum Schluss "R<TAB>0|1<TAB>meldung<TAB>json".
 * Existiert die Datei --cancel, werden Prüfung und Tempomessung abgebrochen. Nur USB- und SD-Datenträger, nie die
 * Systemplatte. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>

#include "../devtest.h"
#include "../mkfs.h"
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
    static const char *const unit[] = {"Bytes", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    snprintf(buf, n, i ? "%.1f %s" : "%.0f %s", v, unit[i]);
    char *dot = strchr(buf, '.');
    if (dot) *dot = ',';
    return buf;
}

static const char *rating_text(double read_mbps) {
    const char *r = dt_rating(read_mbps);
    return !strcmp(r, "schnell") ? "schnell genug für Spiele" : !strcmp(r, "ausreichend") ? "für Spiele ausreichend"
                                                                                          : "langsam – Spiele laden länger";
}

static void win_error(char *buf, size_t n, const char *what) {
    DWORD code = GetLastError();
    WCHAR w[200];
    char m[256] = "";
    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, w, 200, NULL))
        utf8(w, m, sizeof(m));
    size_t l = strlen(m);
    while (l && strchr("\r\n. ", m[l - 1])) m[--l] = 0;
    snprintf(buf, n, "%s: %s (Windows-Fehler %lu)", what, m, (unsigned long)code);
}

static void on_progress(const char *step, int pct) {
    static char last_step[96];
    static int last_pct = -1;
    if (pct == last_pct && !strcmp(step, last_step)) return;
    last_pct = pct;
    snprintf(last_step, sizeof(last_step), "%s", step);
    fprintf(out, "P\t%d\t%s\n", pct, step);
    fflush(out);
}

static void finish(int ok, const char *msg, const char *json) {
    fprintf(out, "R\t%d\t%s\t%s\n", ok, msg, json && *json ? json : "{}");
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
    char     model[128];
} diskinfo_t;

typedef struct {
    WCHAR    guid[64];     /* \\?\Volume{...}\ */
    char     letters[32];  /* "E:" bzw. "E: F:" */
    char     fs[16], label[64];
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
        if (GetVolumeInformationW(name, lbl, 64, NULL, NULL, NULL, fsn, 16)) {
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
            win_error(err, n, v[i].letters[0] ? v[i].letters : "Volume öffnen");
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
            snprintf(err, n, "Laufwerk %s wird gerade benutzt. Bitte Explorer-Fenster und Programme schließen, die "
                             "darauf zugreifen, und es noch einmal versuchen.", v[i].letters[0] ? v[i].letters : "");
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
            snprintf(err, n, "Abbild nicht passend");
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
    if (disk_info(disk, &s->info)) { snprintf(err, n, "Datenträger %d nicht gefunden – bitte die Liste aktualisieren.", disk); return -1; }
    if (!usb_bus(s->info.bus)) { snprintf(err, n, "Datenträger %d ist kein USB-Datenträger – abgelehnt.", disk); return -1; }
    if (system_disk(disk)) { snprintf(err, n, "Datenträger %d enthält Windows – abgelehnt.", disk); return -1; }
    if (s->info.size != size) {
        snprintf(err, n, "Datenträger %d hat sich geändert (andere Größe) – bitte die Liste aktualisieren.", disk);
        return -1;
    }
    if (write && lock_volumes(s, err, n)) { session_close(s, 0); return -1; }
    s->disk = open_disk(disk, write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ);
    if (s->disk == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) snprintf(err, n, "Zugriff verweigert – Administratorrechte nötig.");
        else win_error(err, n, "Datenträger öffnen");
        session_close(s, 0);
        return -1;
    }
    DWORD r;
    if (write && !DeviceIoControl(s->disk, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &r, NULL)) {
        snprintf(err, n, "Der Datenträger ist schreibgeschützt (Schalter am Stick oder an der SD-Karte?).");
        session_close(s, 0);
        return -1;
    }
    s->fd = _open_osfhandle((intptr_t)s->disk, write ? _O_RDWR | _O_BINARY : _O_RDONLY | _O_BINARY);
    if (s->fd < 0) { snprintf(err, n, "Datenträger öffnen fehlgeschlagen."); session_close(s, 0); return -1; }
    return 0;
}

static const char *title(const session_t *s) { return s->info.model[0] ? s->info.model : "USB-Datenträger"; }

/* ------------------------------------------------------------------ Aufträge */

static int cmd_format(int disk, uint64_t size, const char *fsname, const char *label, int verify, int full,
                      int folders, uint64_t limit) {
    char err[512] = "", msg[1024], json[512], s1[24], s2[24];
    int fs = !strcmp(fsname, "fat32") ? FS_FAT32 : FS_EXFAT;
    if (!label_valid(label, fs)) { finish(0, "Ungültige Bezeichnung.", NULL); return 1; }
    if (limit && (limit < 64 * MIB || limit > size)) { finish(0, "Ungültige Grenze für den echten Bereich.", NULL); return 1; }

    session_t s;
    on_progress("Datenträger vorbereiten", 0);
    if (session_open(&s, disk, size, 1, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    const uint64_t use = limit ? limit : size;

    devtest_t t;
    memset(&t, 0, sizeof(t));
    int tested = 0, test_cancelled = 0;
    if (verify || full) {
        tested = full ? 2 : 1;
        int rc = full ? dt_full(s.fd, s.info.ss, use, on_progress, &cancel_requested, &t, err, sizeof(err))
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
                snprintf(msg, sizeof(msg), "Prüfung fehlgeschlagen: %s Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben.", err);
            else if (t.fake)
                snprintf(msg, sizeof(msg), "%s meldet %s, hat aber nur etwa %s echten Speicher. Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben – "
                         "für Spiele an der PS5 nicht empfohlen, höchstens den echten Bereich nutzen.", title(&s), size_text(size, s1, sizeof(s1)),
                         size_text(t.real_bytes, s2, sizeof(s2)));
            else
                snprintf(msg, sizeof(msg), "Fehlerhafte Bereiche ab %llu MB: %s arbeitet nicht zuverlässig. "
                         "Formatiert wurde noch nicht – die bisherigen Daten wurden bei der Prüfung aber schon überschrieben.", (unsigned long long)(t.bad_at / MIB), title(&s));
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
    o.limit = limit;
    o.dirs = folders ? TEMPLATE_DIRS : NULL;
    int rc = format_disk(&d, fs, label, arc4random(), 0, &o);
    session_close(&s, 1);
    if (rc != 0) {
        snprintf(msg, sizeof(msg), "Formatieren fehlgeschlagen: %s", mkfs_error());
        finish(0, msg, NULL);
        return 1;
    }
    int l = snprintf(msg, sizeof(msg), "%s erfolgreich formatiert: %s", title(&s), mkfs_summary());
#define ADD(...) do { if (l < (int)sizeof(msg)) l += snprintf(msg + l, sizeof(msg) - l, __VA_ARGS__); } while (0)
    if (label[0]) ADD(", Bezeichnung %s", label);
    if (o.is_gpt) ADD(", Partitionstabelle GPT");
    if (limit) ADD(", nur die echten %s genutzt", size_text(limit, s1, sizeof(s1)));
    if (o.dirs) ADD(", Ordner homebrew, etaHEN/games und pkg angelegt");
    ADD(".");
    if (tested && !test_cancelled && t.read_mbps > 0)
        ADD(" %s: echt%s, lesen %.0f MB/s, schreiben %.0f MB/s – %s.", tested == 2 ? "Jede Stelle geprüft" : "Prüfung",
            tested == 2 ? ", keine Fehler" : "", t.read_mbps, t.write_mbps, rating_text(t.read_mbps));
    if (test_cancelled) ADD(" Die Prüfung wurde abgebrochen – formatiert wurde trotzdem.");
    ADD(" Jetzt einfach abziehen und an die PS5 stecken.");
#undef ADD
    snprintf(json, sizeof(json), "{\"scheme\":\"%s\",\"read\":%.1f,\"write\":%.1f}", o.is_gpt ? "GPT" : "MBR",
             t.read_mbps, t.write_mbps);
    finish(1, msg, json);
    return 0;
}

static int cmd_speed(int disk, uint64_t size) {
    char err[512] = "", msg[512], json[128];
    session_t s;
    on_progress("Lesetempo messen", 0);
    if (session_open(&s, disk, size, 0, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    devtest_t t;
    int rc = dt_read_speed(s.fd, s.info.ss, size, on_progress, &cancel_requested, &t, err, sizeof(err));
    session_close(&s, 0);
    if (rc != 0) { snprintf(msg, sizeof(msg), "Tempo messen fehlgeschlagen: %s", err); finish(0, msg, NULL); return 1; }
    if (t.cancelled) { finish(0, "Abgebrochen.", NULL); return 1; }
    snprintf(msg, sizeof(msg), "Lesetempo von %s: %.0f MB/s – %s.", title(&s), t.read_mbps, rating_text(t.read_mbps));
    snprintf(json, sizeof(json), "{\"read\":%.1f,\"rating\":\"%s\"}", t.read_mbps, dt_rating(t.read_mbps));
    finish(1, msg, json);
    return 0;
}

static int cmd_label(int disk, uint64_t size, const char *label) {
    char err[512] = "", msg[512];
    session_t s;
    on_progress("Bezeichnung schreiben", 30);
    if (session_open(&s, disk, size, 1, err, sizeof(err))) { finish(0, err, NULL); return 1; }
    int rc = vol_set_label(s.fd, s.info.ss, s.info.size, label, err, sizeof(err));
    session_close(&s, 1);
    if (rc != 0) { snprintf(msg, sizeof(msg), "Umbenennen fehlgeschlagen: %s", err); finish(0, msg, NULL); return 1; }
    if (label[0]) snprintf(msg, sizeof(msg), "Neue Bezeichnung für %s: „%s“.", title(&s), label);
    else snprintf(msg, sizeof(msg), "Bezeichnung von %s entfernt.", title(&s));
    finish(1, msg, NULL);
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
    if (!strcmp(cmd, "speed")) return cmd_speed(disk, size);
    if (!strcmp(cmd, "label") && rest >= 3) return cmd_label(disk, size, a[2]);
    if (!strcmp(cmd, "format") && rest >= 4) {
        int verify = 0, full = 0, folders = 0;
        uint64_t limit = 0;
        for (int k = 4; k < rest; k++) {
            if (!strcmp(a[k], "verify")) verify = 1;
            else if (!strcmp(a[k], "full")) full = 1;
            else if (!strcmp(a[k], "folders")) folders = 1;
            else if (!strncmp(a[k], "limit=", 6)) limit = strtoull(a[k] + 6, NULL, 10);
        }
        return cmd_format(disk, size, a[2], a[3], verify, full, folders, limit);
    }
    finish(0, "Unbekannter Auftrag.", NULL);
    return 2;
}
