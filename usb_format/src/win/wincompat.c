/* POSIX-Ersatz für den Windows-Helfer: Lese-/Schreibzugriffe mit Position auf Datenträger-Handles */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <stdio.h>

#include "wincompat.h"
#include "../i18n.h"

#undef strerror

static DWORD last_win_error;

static HANDLE handle_of(int fd) { return (HANDLE)_get_osfhandle(fd); }

static ssize_t rw(int fd, void *buf, size_t len, uint64_t off, int wr) {
    HANDLE h = handle_of(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (len > 0x40000000u) len = 0x40000000u;   /* höchstens 1 GiB je Aufruf (DWORD) */
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD n = 0;
    BOOL ok = wr ? WriteFile(h, buf, (DWORD)len, &n, &ov) : ReadFile(h, buf, (DWORD)len, &n, &ov);
    if (!ok) {
        last_win_error = GetLastError();
        errno = EIO;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t usbfmt_pread(int fd, void *buf, size_t len, uint64_t off) { return rw(fd, buf, len, off, 0); }

ssize_t usbfmt_pwrite(int fd, const void *buf, size_t len, uint64_t off) { return rw(fd, (void *)buf, len, off, 1); }

int usbfmt_fsync(int fd) {
    HANDLE h = handle_of(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EINVAL; return -1; }
    if (!FlushFileBuffers(h)) {   /* "geht hier nicht" ist kein Fehler - ein gescheitertes Zurückschreiben schon */
        DWORD e = GetLastError();
        errno = e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED ? EINVAL : EIO;
        return -1;
    }
    return 0;
}

/* Rohzugriffe ohne FILE_FLAG_NO_BUFFERING brauchen keinen ausgerichteten Speicher - der Code gibt mit free() frei */
int usbfmt_memalign(void **p, size_t align, size_t len) {
    (void)align;
    *p = malloc(len ? len : 1);
    return *p ? 0 : ENOMEM;
}

typedef NTSTATUS (WINAPI *gen_random_fn)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);

/* Seriennummern und GUIDs. bcrypt.dll gehört nicht zu den KnownDLLs: Fest eingebunden würde Windows sie zuerst neben
 * dem Helfer suchen (Temp-Ordner, für jeden beschreibbar) und dort mit Administratorrechten laden - daher nur aus
 * System32 nachladen. */
void usbfmt_random_buf(void *buf, size_t len) {
    static gen_random_fn gen;
    if (!gen) {
        HMODULE m = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m) gen = (gen_random_fn)(void *)GetProcAddress(m, "BCryptGenRandom");
    }
    if (gen && gen(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0) return;
    /* Notfall (geheim muss nichts davon sein): Zeitgeber durchmischen (splitmix64) */
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t x = (uint64_t)q.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64();
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        if (i % 8 == 0) {
            x += 0x9E3779B97F4A7C15ull;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            q.QuadPart = (LONGLONG)(z ^ (z >> 31));
        }
        p[i] = (uint8_t)((uint64_t)q.QuadPart >> (8 * (i % 8)));
    }
}

uint32_t usbfmt_random(void) {
    uint32_t v;
    usbfmt_random_buf(&v, sizeof(v));
    return v;
}

int usbfmt_clock_gettime(int clk, struct timespec *ts) {
    (void)clk;
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}

const char *usbfmt_strerror(int e) {
    static char buf[256];
    if (e == EIO && last_win_error) {
        DWORD code = last_win_error;
        WCHAR w[200];
        if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, w, 200, NULL)) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 40, NULL, NULL);
            size_t n = strlen(buf);
            while (n && strchr("\r\n. ", buf[n - 1])) buf[--n] = 0;
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), T(" (Windows-Fehler %lu)"), (unsigned long)code);
            return buf;
        }
    }
    return strerror(e);
}
