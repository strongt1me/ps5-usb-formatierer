/* Wird beim Bau von lib/ext2fs/windows_io.c vorangestellt (build_mke2fs.ps1, -include).
 * msvcrt-fstat() scheitert bei einem rohen Datenträger (\\.\PhysicalDriveN) mit EINVAL - mke2fs bricht dann schon in
 * check_plausibility() mit "stat: Invalid argument" ab (am echten Stick gesehen; Abbild-Dateien gehen). Hier wird so
 * ein Datenträger als Blockgerät gemeldet; alles andere geht unverändert an fstat(). */
#ifndef USBFMT_WIN_FSTAT_H
#define USBFMT_WIN_FSTAT_H
#include "config.h"   /* vor sys/stat.h: der Ersatzkopf aus include/mingw bindet den echten nur mit HAVE_SYS_STAT_H ein */
#include <errno.h>
#include <io.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

static inline int usbfmt_fstat(int fd, struct stat *st) {
    int saved = errno;
    if (fstat(fd, st) == 0) return 0;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    BY_HANDLE_FILE_INFORMATION fi;
    if (h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_DISK && !GetFileInformationByHandle(h, &fi)) {
        memset(st, 0, sizeof(*st));   /* Größe 0: mke2fs bekommt die Größe ohnehin vom Helfer */
        st->st_mode = S_IFBLK | 0660;
        errno = saved;
        return 0;
    }
    return -1;
}
#define fstat(fd, st) usbfmt_fstat(fd, st)
#endif
