/*
 * PS5-Spielstand-Sicherung, Wiederherstellung und Neusignierung
 * Nur für den PS5-ELF-Payload; läuft nach dem Kernel-Jailbreak.
 * Neusignierung: verwendet sceFsMountSaveData aus libSceFsInternalForVsh.sprx
 * und /dev/pfsmgr (ioctl 0xC0845302) zum Entschlüsseln des Sealed Key.
 * Lizenzen: garlic-worker (GPL-3.0) als Referenz für Mount-Ablauf/Offsets.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "i18n.h"
#include "library.h"
#include "platform.h"
#include "saves.h"

/* ── Konstanten ───────────────────────────────────────────────────────── */
#define SAVE_HOME       "/user/home"
#define SAVE_PROSPERO   "savedata_prospero"
#define SAVE_TMPDIR     "/data/usb_format/save_tmp"
#define SAVE_MNTDIR     "/data/usb_format/save_mnt"
#define SAVE_AUTHID     0x4800000000000010ULL
#define PFSMGR_PATH     "/dev/pfsmgr"
#define PFSMGR_DECRYPT  0xC0845302UL
#define SEALED_KEY_OFF  0x800    /* Offset des Sealed Key im Container-Image */
#define SEALED_KEY_LEN  0x60

/* ── Laufzeit-Bibliothek für Save-Mount ──────────────────────────────── */
typedef struct { uint8_t reserved; char *budgetid; } MountOpt;
typedef struct { uint8_t dummy; } UmountOpt;
typedef int (*fn_mount_init_t)(MountOpt *);
typedef int (*fn_mount_t)(MountOpt *, const char *, const char *, uint8_t *);
typedef int (*fn_umount_init_t)(UmountOpt *);
typedef int (*fn_umount_t)(UmountOpt *, const char *, int, int);

static fn_mount_init_t  g_mount_init  = NULL;
static fn_mount_t       g_mount       = NULL;
static fn_umount_init_t g_umount_init = NULL;
static fn_umount_t      g_umount      = NULL;
static int g_fs_ready = 0;

static int fs_lib_init(char *err, size_t errsz) {
    if (g_fs_ready) return 0;
    const char *lib = "libSceFsInternalForVsh.sprx";
    g_mount_init  = (fn_mount_init_t)  plat_dynsym(lib, "sceFsInitMountSaveDataOpt");
    g_mount       = (fn_mount_t)       plat_dynsym(lib, "sceFsMountSaveData");
    g_umount_init = (fn_umount_init_t) plat_dynsym(lib, "sceFsInitUmountSaveDataOpt");
    g_umount      = (fn_umount_t)      plat_dynsym(lib, "sceFsUmountSaveData");
    if (!g_mount_init || !g_mount || !g_umount_init || !g_umount) {
        snprintf(err, errsz, "%s", T("libSceFsInternalForVsh.sprx nicht gefunden"));
        return -1;
    }
    g_fs_ready = 1;
    return 0;
}

/* ── Registry für Benutzer/Account-IDs ──────────────────────────────── */
typedef int (*fn_regstr_t)(int key, char *buf, size_t sz);
typedef int (*fn_regbin_t)(int key, void *buf, size_t sz);

static fn_regstr_t g_reg_str = NULL;
static fn_regbin_t g_reg_bin = NULL;
static int g_reg_ready = 0;

static int reg_init(void) {
    if (g_reg_ready) return 0;
    const char *lib = "libSceRegMgr.sprx";
    g_reg_str = (fn_regstr_t)plat_dynsym(lib, "sceRegMgrGetStr");
    g_reg_bin = (fn_regbin_t)plat_dynsym(lib, "sceRegMgrGetBin");
    g_reg_ready = (g_reg_str && g_reg_bin) ? 1 : -1;
    return g_reg_ready == 1 ? 0 : -1;
}

/* Account-ID für den Benutzer lesen: Benutzernamen-Datei → Registry */
static int user_account_id(const char *uid, const char *username, char *out16, size_t outsz) {
    if (reg_init() < 0 || !username[0]) return -1;
    for (int slot = 1; slot <= 16; slot++) {
        int key_name = (slot - 1) * 65536 + 125829632;
        char reg_name[32] = "";
        if (g_reg_str(key_name, reg_name, sizeof(reg_name)) < 0 || !reg_name[0]) continue;
        if (strcmp(reg_name, username) != 0) continue;
        int key_aid = (slot - 1) * 65536 + 125830400;
        uint64_t aid = 0;
        if (g_reg_bin(key_aid, &aid, sizeof(aid)) < 0) return -1;
        snprintf(out16, outsz, "%016llx", (unsigned long long)aid);
        (void)uid;
        return 0;
    }
    return -1;
}

/* ── Benutzerauflistung ───────────────────────────────────────────────── */
int saves_scan_users(save_user_t **out, int *count) {
    *out = NULL; *count = 0;
    DIR *d = opendir(SAVE_HOME);
    if (!d) return -1;
    save_user_t *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char upath[256];
        snprintf(upath, sizeof(upath), "%s/%s/username.dat", SAVE_HOME, ent->d_name);
        int fd = open(upath, O_RDONLY);
        if (fd < 0) continue;
        char username[17] = "";
        ssize_t nr = read(fd, username, 16);
        close(fd);
        if (nr <= 0 || !username[0]) continue;
        username[16] = '\0';
        if (n >= cap) {
            int nc = cap ? cap * 2 : 8;
            save_user_t *p = realloc(arr, nc * sizeof(*p));
            if (!p) break;
            arr = p; cap = nc;
        }
        save_user_t *u = &arr[n++];
        memset(u, 0, sizeof(*u));
        snprintf(u->uid, sizeof(u->uid), "%s", ent->d_name);
        snprintf(u->username, sizeof(u->username), "%s", username);
        user_account_id(ent->d_name, username, u->account_id, sizeof(u->account_id));
    }
    closedir(d);
    *out = arr; *count = n;
    return 0;
}

/* ── Spielname aus param.sfo des installierten Spiels ──────────────── */
static void lookup_title_name(const char *title_id, char *name, size_t namesz) {
    name[0] = '\0';
    static const char *const paths[] = {
        "/user/appmeta/%s/param.sfo",
        "/system_data/priv/appmeta/%s/param.sfo",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), paths[i], title_id);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        uint8_t buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n <= 0) continue;
        char tid[32] = "";
        if (lib_parse_param_sfo(buf, (size_t)n, tid, sizeof(tid), name, namesz) == 0 && name[0])
            return;
        name[0] = '\0';
    }
}

/* ── Spielstand-Scan ─────────────────────────────────────────────────── */
int saves_scan(save_info_t **out, int *count) {
    *out = NULL; *count = 0;
    DIR *home = opendir(SAVE_HOME);
    if (!home) return -1;
    save_info_t *arr = NULL;
    int n = 0, cap = 0;
    struct dirent *ue;
    while ((ue = readdir(home))) {
        if (ue->d_name[0] == '.') continue;
        char uid[16];
        snprintf(uid, sizeof(uid), "%s", ue->d_name);

        /* Benutzername */
        char upath[256], username[64] = "";
        snprintf(upath, sizeof(upath), "%s/%s/username.dat", SAVE_HOME, uid);
        int ufd = open(upath, O_RDONLY);
        if (ufd >= 0) { read(ufd, username, 63); username[63] = '\0'; close(ufd); }

        /* PS5-Spielstände */
        char pdir[256];
        snprintf(pdir, sizeof(pdir), "%s/%s/%s", SAVE_HOME, uid, SAVE_PROSPERO);
        DIR *pd = opendir(pdir);
        if (!pd) continue;
        struct dirent *te;
        while ((te = readdir(pd))) {
            if (te->d_name[0] == '.') continue;
            char tdir[512];
            snprintf(tdir, sizeof(tdir), "%s/%s", pdir, te->d_name);
            DIR *sd = opendir(tdir);
            if (!sd) continue;
            struct dirent *se;
            while ((se = readdir(sd))) {
                if (se->d_name[0] == '.') continue;
                char fpath[512];
                snprintf(fpath, sizeof(fpath), "%s/%s", tdir, se->d_name);
                struct stat st;
                if (stat(fpath, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                if (n >= cap) {
                    int nc = cap ? cap * 2 : 32;
                    save_info_t *p = realloc(arr, nc * sizeof(*p));
                    if (!p) goto done_scan;
                    arr = p; cap = nc;
                }
                save_info_t *s = &arr[n++];
                memset(s, 0, sizeof(*s));
                snprintf(s->path,      sizeof(s->path),      "%s", fpath);
                snprintf(s->title_id,  sizeof(s->title_id),  "%s", te->d_name);
                snprintf(s->save_dir,  sizeof(s->save_dir),  "%s", se->d_name);
                snprintf(s->uid,       sizeof(s->uid),       "%s", uid);
                snprintf(s->username,  sizeof(s->username),  "%s", username);
                lookup_title_name(te->d_name, s->title_name, sizeof(s->title_name));
                if (!s->title_name[0])
                    snprintf(s->title_name, sizeof(s->title_name), "%s", te->d_name);
            }
            closedir(sd);
        }
        closedir(pd);
    }
done_scan:
    closedir(home);
    *out = arr; *count = n;
    return 0;
}

/* ── CRC32 für ZIP ───────────────────────────────────────────────────── */
static uint32_t g_crc_tbl[256];
static int g_crc_ready = 0;
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tbl[i] = c;
    }
    g_crc_ready = 1;
}
static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len) {
    if (!g_crc_ready) crc32_init();
    const uint8_t *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = g_crc_tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static uint32_t crc32_fd(int fd, size_t size) {
    uint32_t crc = 0;
    uint8_t buf[65536];
    size_t rem = size;
    off_t pos = 0;
    while (rem) {
        size_t chunk = rem > sizeof(buf) ? sizeof(buf) : rem;
        ssize_t n = pread(fd, buf, chunk, pos);
        if (n <= 0) break;
        crc = crc32_update(crc, buf, (size_t)n);
        pos += n; rem -= (size_t)n;
    }
    return crc;
}

/* ── Einfacher ZIP-Stream-Schreiber (gespeichert, kein Deflate) ───────── */
typedef struct {
    int    sock;
    int    broken;
    char   http_head[512];
    int    head_sent;
    /* Zentralverzeichnis – max. 2 Einträge */
    struct { char name[512]; uint32_t crc, size, off; } cd[2];
    int    cd_n;
    uint32_t offset;
} zstream_t;

static void zs_send(zstream_t *z, const void *buf, size_t len) {
    if (z->broken) return;
    if (!z->head_sent) {
        if (send(z->sock, z->http_head, strlen(z->http_head), 0) < 0) { z->broken = 1; return; }
        z->head_sent = 1;
    }
    /* chunked encoding */
    char hdr[32];
    int hl = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    if (send(z->sock, hdr, (size_t)hl, 0) < 0 ||
        send(z->sock, buf, len, 0) < 0 ||
        send(z->sock, "\r\n", 2, 0) < 0) z->broken = 1;
}

static void zs_u16(uint8_t *b, uint16_t v) { b[0]=v&0xFF; b[1]=v>>8; }
static void zs_u32(uint8_t *b, uint32_t v) { b[0]=v&0xFF; b[1]=(v>>8)&0xFF; b[2]=(v>>16)&0xFF; b[3]=v>>24; }

/* Lokalen Datei-Header schreiben und Slot im Zentralverzeichnis reservieren */
static void zs_start_file(zstream_t *z, const char *name, uint32_t crc, uint32_t size) {
    if (z->cd_n >= 2 || z->broken) return;
    uint16_t nlen = (uint16_t)strlen(name);
    uint8_t lhdr[30]; memset(lhdr, 0, sizeof(lhdr));
    lhdr[0]=0x50; lhdr[1]=0x4B; lhdr[2]=0x03; lhdr[3]=0x04;
    zs_u16(lhdr+4, 20);          /* Version benötigt */
    zs_u16(lhdr+6, 0);           /* Flags */
    zs_u16(lhdr+8, 0);           /* Methode: gespeichert */
    zs_u32(lhdr+14, crc);
    zs_u32(lhdr+18, size);
    zs_u32(lhdr+22, size);
    zs_u16(lhdr+26, nlen);
    z->cd[z->cd_n].crc  = crc;
    z->cd[z->cd_n].size = size;
    z->cd[z->cd_n].off  = z->offset;
    snprintf(z->cd[z->cd_n].name, sizeof(z->cd[z->cd_n].name), "%s", name);
    z->cd_n++;
    zs_send(z, lhdr, 30);
    zs_send(z, name, nlen);
    z->offset += 30 + nlen;
}

static void zs_file_data(zstream_t *z, int fd, size_t size) {
    uint8_t buf[65536];
    size_t rem = size;
    off_t pos = 0;
    while (rem && !z->broken) {
        size_t chunk = rem > sizeof(buf) ? sizeof(buf) : rem;
        ssize_t n = pread(fd, buf, chunk, pos);
        if (n <= 0) break;
        zs_send(z, buf, (size_t)n);
        pos += n; rem -= (size_t)n;
    }
    z->offset += (uint32_t)size;
}

static void zs_file_buf(zstream_t *z, const void *buf, size_t size) {
    zs_send(z, buf, size);
    z->offset += (uint32_t)size;
}

static void zs_finish(zstream_t *z) {
    uint32_t cd_off = z->offset;
    uint32_t cd_size = 0;
    for (int i = 0; i < z->cd_n; i++) {
        uint16_t nlen = (uint16_t)strlen(z->cd[i].name);
        uint8_t cde[46]; memset(cde, 0, sizeof(cde));
        cde[0]=0x50; cde[1]=0x4B; cde[2]=0x01; cde[3]=0x02;
        zs_u16(cde+4, 20); zs_u16(cde+6, 20);
        zs_u32(cde+16, z->cd[i].crc);
        zs_u32(cde+20, z->cd[i].size);
        zs_u32(cde+24, z->cd[i].size);
        zs_u16(cde+28, nlen);
        zs_u32(cde+42, z->cd[i].off);
        zs_send(z, cde, 46);
        zs_send(z, z->cd[i].name, nlen);
        cd_size += 46 + nlen;
    }
    uint8_t eocd[22]; memset(eocd, 0, sizeof(eocd));
    eocd[0]=0x50; eocd[1]=0x4B; eocd[2]=0x05; eocd[3]=0x06;
    zs_u16(eocd+8,  (uint16_t)z->cd_n);
    zs_u16(eocd+10, (uint16_t)z->cd_n);
    zs_u32(eocd+12, cd_size);
    zs_u32(eocd+16, cd_off);
    zs_send(z, eocd, 22);
    /* chunked end */
    if (!z->broken && z->head_sent)
        send(z->sock, "0\r\n\r\n", 5, 0);
}

/* ── Spielstand als ZIP herunterladen ────────────────────────────────── */
void saves_download(int sock, const save_info_t *s, const char *filename) {
    int fd = open(s->path, O_RDONLY);
    if (fd < 0) {
        const char *r = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(sock, r, strlen(r), 0);
        return;
    }
    struct stat st;
    fstat(fd, &st);
    size_t imgsz = (size_t)st.st_size;
    uint32_t img_crc = crc32_fd(fd, imgsz);

    /* Metadaten-JSON zusammenstellen */
    char meta[1024];
    time_t now = time(NULL);
    int metalen = snprintf(meta, sizeof(meta),
        "{\"title_id\":\"%s\",\"title_name\":\"%s\","
        "\"save_dir\":\"%s\",\"uid\":\"%s\",\"username\":\"%s\","
        "\"timestamp\":%lld}",
        s->title_id, s->title_name, s->save_dir,
        s->uid, s->username, (long long)now);
    uint32_t meta_crc = crc32_update(0, meta, (size_t)metalen);

    /* Dateinamen im ZIP */
    char imgname[128];
    snprintf(imgname, sizeof(imgname), "%s_%s.img", s->title_id, s->save_dir);

    /* HTTP-Kopf */
    zstream_t z; memset(&z, 0, sizeof(z));
    z.sock = sock;
    snprintf(z.http_head, sizeof(z.http_head),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/zip\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n",
        filename);

    zs_start_file(&z, imgname, img_crc, (uint32_t)imgsz);
    zs_file_data(&z, fd, imgsz);
    zs_start_file(&z, "metadata.json", meta_crc, (uint32_t)metalen);
    zs_file_buf(&z, meta, (size_t)metalen);
    zs_finish(&z);
    close(fd);
}

/* ── Spielstand aus ZIP wiederherstellen ─────────────────────────────── */

/* Minimaler ZIP-Parser: sucht Local File Header, extrahiert name + Inhalt */
static int zip_find_file(const char *body, size_t len, const char *name,
                         const char **data_out, size_t *size_out) {
    size_t pos = 0;
    while (pos + 30 < len) {
        if (memcmp(body + pos, "PK\x03\x04", 4) != 0) { pos++; continue; }
        uint16_t flags, method, nlen, xlen;
        uint32_t csize, usize;
        memcpy(&flags,  body + pos + 6,  2);
        memcpy(&method, body + pos + 8,  2);
        memcpy(&csize,  body + pos + 18, 4);
        memcpy(&usize,  body + pos + 22, 4);
        memcpy(&nlen,   body + pos + 26, 2);
        memcpy(&xlen,   body + pos + 28, 2);
        size_t data_off = pos + 30 + nlen + xlen;
        if (data_off > len) return -1;
        char ename[512] = "";
        size_t ncopy = nlen < sizeof(ename) - 1 ? nlen : sizeof(ename) - 1;
        memcpy(ename, body + pos + 30, ncopy);
        ename[ncopy] = '\0';
        /* bei Datendeskriptor (bit3) ist usize möglicherweise 0; wir kennen das Format selbst */
        size_t datasz = (flags & 0x0008) ? (len - data_off) : usize;
        if (method != 0) { pos = data_off + csize; continue; }   /* nur Stored */
        if (strcmp(ename, name) == 0) {
            *data_out = body + data_off;
            *size_out = (flags & 0x0008) ? datasz : (size_t)usize;
            return 0;
        }
        pos = data_off + (usize > 0 ? usize : csize);
    }
    return -1;
}

/* Einfacher JSON-String-Extraktor (ohne Bibliothek) */
static int json_str(const char *json, const char *key, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) out[n++] = *p++;
    out[n] = '\0';
    return *p == '"' ? 0 : -1;
}

int saves_restore(const char *body, size_t bodylen, char *err, size_t errsz) {
    /* metadata.json aus ZIP lesen */
    const char *meta_data; size_t meta_len;
    if (zip_find_file(body, bodylen, "metadata.json", &meta_data, &meta_len) != 0) {
        snprintf(err, errsz, "metadata.json nicht im ZIP gefunden");
        return -1;
    }
    char meta[1024] = "";
    size_t cp = meta_len < sizeof(meta) - 1 ? meta_len : sizeof(meta) - 1;
    memcpy(meta, meta_data, cp);

    char title_id[32] = "", save_dir[256] = "", uid[16] = "";
    json_str(meta, "title_id", title_id, sizeof(title_id));
    json_str(meta, "save_dir", save_dir, sizeof(save_dir));
    json_str(meta, "uid",      uid,      sizeof(uid));

    if (!title_id[0] || !save_dir[0] || !uid[0]) {
        snprintf(err, errsz, "Metadaten unvollständig (title_id/save_dir/uid fehlt)");
        return -1;
    }

    /* Image-Datei aus ZIP lesen */
    char imgname[128];
    snprintf(imgname, sizeof(imgname), "%s_%s.img", title_id, save_dir);
    const char *img_data; size_t img_len;
    if (zip_find_file(body, bodylen, imgname, &img_data, &img_len) != 0) {
        snprintf(err, errsz, "Image-Datei (%s) nicht im ZIP gefunden", imgname);
        return -1;
    }

    /* Zielordner anlegen */
    char destdir[512];
    snprintf(destdir, sizeof(destdir), "%s/%s/%s/%s",
             SAVE_HOME, uid, SAVE_PROSPERO, title_id);
    mkdir(SAVE_HOME, 0755); /* darf existieren */
    char tmp[512]; snprintf(tmp, sizeof(tmp), "%s/%s", SAVE_HOME, uid); mkdir(tmp, 0755);
    snprintf(tmp, sizeof(tmp), "%s/%s/%s", SAVE_HOME, uid, SAVE_PROSPERO); mkdir(tmp, 0755);
    mkdir(destdir, 0755);

    /* Image schreiben */
    char dest[512];
    snprintf(dest, sizeof(dest), "%s/%s", destdir, save_dir);
    int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(err, errsz, "Zieldatei konnte nicht geöffnet werden: %s", strerror(errno));
        return -1;
    }
    size_t written = 0, rem = img_len;
    while (rem) {
        ssize_t n = write(fd, img_data + written, rem);
        if (n <= 0) break;
        written += (size_t)n; rem -= (size_t)n;
    }
    if (fsync(fd) < 0) {}
    close(fd);
    if (written != img_len) {
        snprintf(err, errsz, "Schreibfehler (%zu/%zu Bytes)", written, img_len);
        return -1;
    }
    return 0;
}

/* ── SFO: Offset von ACCOUNT_ID im Datei-Deskriptor ────────────────── */
static int sfo_account_offset(int fd, off_t *off) {
    uint8_t hdr[20];
    if (pread(fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) return -1;
    if (memcmp(hdr, "\0PSF", 4) != 0) return -1;
    uint32_t kt, dt, cnt;
    memcpy(&kt,  hdr + 8,  4);
    memcpy(&dt,  hdr + 12, 4);
    memcpy(&cnt, hdr + 16, 4);
    if (cnt > 256 || kt < 20 || dt < kt) return -1;
    for (uint32_t i = 0; i < cnt; i++) {
        uint8_t e[16];
        if (pread(fd, e, sizeof(e), 20 + (off_t)i * 16) != (ssize_t)sizeof(e)) return -1;
        uint16_t koff; uint32_t doff;
        memcpy(&koff, e,      2);
        memcpy(&doff, e + 12, 4);
        char key[32] = "";
        pread(fd, key, sizeof(key) - 1, (off_t)kt + koff);
        if (strcmp(key, "ACCOUNT_ID") != 0) continue;
        *off = (off_t)dt + doff;
        return 0;
    }
    return -1;
}

/* ── Spielstand neusignieren ─────────────────────────────────────────── */
int saves_resign(const save_info_t *s, const char *new_aid_hex, char *err, size_t errsz) {
    /* Account-ID parsen */
    if (strlen(new_aid_hex) != 16) {
        snprintf(err, errsz, "Account-ID muss 16 Hex-Zeichen haben");
        return -1;
    }
    uint8_t new_aid[8];
    uint64_t aid_val = 0;
    for (int i = 0; i < 16; i++) {
        char c = new_aid_hex[i];
        unsigned d = (c >= '0' && c <= '9') ? (unsigned)(c - '0') :
                     (c >= 'a' && c <= 'f') ? (unsigned)(c - 'a' + 10) :
                     (c >= 'A' && c <= 'F') ? (unsigned)(c - 'A' + 10) : 255u;
        if (d > 15) { snprintf(err, errsz, "Ungültige Account-ID"); return -1; }
        aid_val = (aid_val << 4) | d;
    }
    for (int i = 0; i < 8; i++) new_aid[i] = (uint8_t)(aid_val >> (i * 8));   /* little-endian */

    /* Bibliothek laden */
    if (fs_lib_init(err, errsz) < 0) return -1;

    /* Privilegien erhöhen */
    kernel_set_ucred_authid(getpid(), SAVE_AUTHID);

    /* Arbeitsverzeichnisse anlegen */
    mkdir("/data/usb_format", 0755);
    mkdir(SAVE_TMPDIR, 0755);
    mkdir(SAVE_MNTDIR, 0755);

    /* Image in /data/ kopieren (direktes Einhängen aus savedata_prospero verursacht EPIPE) */
    char work[512];
    snprintf(work, sizeof(work), "%s/resign_%s_%s", SAVE_TMPDIR, s->title_id, s->save_dir);
    {
        int src = open(s->path, O_RDONLY);
        if (src < 0) {
            snprintf(err, errsz, "Spielstand konnte nicht geöffnet werden: %s", strerror(errno));
            return -1;
        }
        struct stat st; fstat(src, &st);
        int dst = open(work, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst < 0) { close(src); snprintf(err, errsz, "Arbeitskopie konnte nicht erstellt werden"); return -1; }
        uint8_t buf[65536]; ssize_t n;
        while ((n = read(src, buf, sizeof(buf))) > 0) write(dst, buf, (size_t)n);
        fsync(dst); close(dst); close(src);
        (void)st;
    }

    /* Sealed Key aus Image lesen */
    uint8_t sealed[SEALED_KEY_LEN];
    {
        int fd = open(work, O_RDONLY);
        if (fd < 0 || pread(fd, sealed, SEALED_KEY_LEN, SEALED_KEY_OFF) != SEALED_KEY_LEN) {
            if (fd >= 0) close(fd);
            unlink(work);
            snprintf(err, errsz, "Sealed Key konnte nicht gelesen werden");
            return -1;
        }
        close(fd);
    }

    /* /dev/pfsmgr: Schlüssel entschlüsseln */
    uint8_t key[0x20] = {0};
    {
        struct { uint8_t sk[0x60]; uint8_t key[0x20]; uint32_t res; } ioc;
        memset(&ioc, 0, sizeof(ioc));
        memcpy(ioc.sk, sealed, SEALED_KEY_LEN);
        int pfsd = open(PFSMGR_PATH, O_RDWR);
        if (pfsd < 0 || ioctl(pfsd, PFSMGR_DECRYPT, &ioc) < 0 || ioc.res != 0) {
            if (pfsd >= 0) close(pfsd);
            unlink(work);
            snprintf(err, errsz, "Schlüssel konnte nicht entschlüsselt werden (pfsmgr)");
            return -1;
        }
        close(pfsd);
        memcpy(key, ioc.key, 0x20);
    }

    /* Einhängen */
    MountOpt mopt; char budgetid[] = "system";
    g_mount_init(&mopt);
    mopt.reserved = 0; mopt.budgetid = budgetid;
    /* SIGPIPE ignorieren während des Einhängens */
    struct sigaction sa_old, sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_DFL;
    sigaction(SIGPIPE, &sa_ign, &sa_old);
    int rc = g_mount(&mopt, work, SAVE_MNTDIR, key);
    if (rc != 0) {
        sigaction(SIGPIPE, &sa_old, NULL);
        unlink(work);
        snprintf(err, errsz, "Einhängen fehlgeschlagen (0x%08x)", (unsigned)rc);
        return -1;
    }
    sigaction(SIGPIPE, &sa_old, NULL);

    /* param.sfo patchen */
    char sfo_path[512];
    snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", SAVE_MNTDIR);
    int ok = 0;
    {
        int fd = open(sfo_path, O_RDWR);
        if (fd >= 0) {
            off_t aid_off = 0;
            if (sfo_account_offset(fd, &aid_off) == 0 &&
                pwrite(fd, new_aid, 8, aid_off) == 8) {
                fsync(fd);
                uint8_t check[8];
                ok = pread(fd, check, 8, aid_off) == 8 && memcmp(check, new_aid, 8) == 0;
            }
            close(fd);
        }
    }

    /* Aushängen */
    UmountOpt uopt; g_umount_init(&uopt);
    g_umount(&uopt, SAVE_MNTDIR, 0, 0);

    if (!ok) {
        unlink(work);
        snprintf(err, errsz, "Signierung fehlgeschlagen – param.sfo konnte nicht geändert werden");
        return -1;
    }

    /* Arbeitskopie zurück an Original-Stelle kopieren */
    {
        int src = open(work, O_RDONLY);
        int dst = open(s->path, O_WRONLY | O_TRUNC, 0644);
        if (src < 0 || dst < 0) {
            if (src >= 0) close(src);
            if (dst >= 0) close(dst);
            unlink(work);
            snprintf(err, errsz, "Zurückkopieren fehlgeschlagen");
            return -1;
        }
        uint8_t buf[65536]; ssize_t n;
        while ((n = read(src, buf, sizeof(buf))) > 0) write(dst, buf, (size_t)n);
        fsync(dst); close(dst); close(src);
    }
    unlink(work);
    return 0;
}
