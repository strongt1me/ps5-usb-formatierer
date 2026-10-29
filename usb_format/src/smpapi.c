/* Kleiner HTTP-Client für die API von ShadowMountPlus (nur 127.0.0.1) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "smpapi.h"

#define SMP_MAX (8 * 1024 * 1024)

/* Liegt die ganze Antwort schon vor? Nur mit Content-Length erkennbar - sonst gilt das Ende der Verbindung */
static int complete(const char *buf, size_t n) {
    const char *end = NULL;
    for (size_t i = 0; i + 4 <= n && !end; i++)
        if (!memcmp(buf + i, "\r\n\r\n", 4)) end = buf + i + 4;
    if (!end) return 0;
    static const char key[] = "content-length:";
    for (const char *p = buf; p + sizeof(key) - 1 < end; p++) {
        size_t k = 0;
        while (k < sizeof(key) - 1 && (p[k] | 0x20) == key[k]) k++;   /* ohne Groß-/Kleinschreibung */
        if (k < sizeof(key) - 1 || (p != buf && p[-1] != '\n')) continue;
        unsigned long long want = strtoull(p + k, NULL, 10);
        return want <= n && (size_t)(end - buf) + (size_t)want <= n;   /* kein Überlauf bei absurden Längen */
    }
    return 0;
}

/* body NULL: GET (Spielbilder), sonst POST mit JSON */
static char *smp_http(int port, const char *path, const char *body, int *http, size_t *len) {
    char head[320];
    size_t blen = body ? strlen(body) : 0;
    int hl = body ? snprintf(head, sizeof(head), "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                             "Content-Length: %zu\r\nConnection: close\r\n\r\n", path, blen)
                  : snprintf(head, sizeof(head), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    *len = 0;
    *http = 0;
    if (hl <= 0 || hl >= (int)sizeof(head) || port <= 0) return NULL;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {10, 0};
    if (s < 0) return NULL;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0 || send(s, head, (size_t)hl, 0) != hl ||
        (blen && send(s, body, blen, 0) != (ssize_t)blen)) {
        close(s);
        return NULL;   /* ShadowMountPlus läuft nicht bzw. ohne API */
    }
    size_t cap = 64 * 1024, n = 0;
    char *buf = malloc(cap + 1);
    const time_t deadline = time(NULL) + 20;   /* ganze Antwort, nicht nur je recv (hängendes ShadowMountPlus) */
    while (buf) {
        if (time(NULL) > deadline) { free(buf); close(s); return NULL; }
        /* abgeschnittene Antwort (zu groß, kein Speicher, Zeitüberschreitung) gilt als Fehler, nie als vollständig:
         * eine halbe Spieleliste ließe ein eingebundenes Spiel wie eine zweite Kopie aussehen */
        if (n == cap) {
            char *nb = cap < SMP_MAX ? realloc(buf, cap * 2 + 1) : NULL;
            if (!nb) { free(buf); close(s); return NULL; }
            buf = nb;
            cap *= 2;
        }
        ssize_t k = recv(s, buf + n, cap - n, 0);
        if (k < 0) { free(buf); close(s); return NULL; }
        if (k == 0) break;   /* Ende (Connection: close) */
        n += (size_t)k;
        if (complete(buf, n)) break;   /* Antwort vollständig, auch wenn die Verbindung offen bliebe */
    }
    close(s);
    if (!buf) return NULL;
    buf[n] = 0;
    char *b = strstr(buf, "\r\n\r\n");
    if (!b || sscanf(buf, "HTTP/%*s %d", http) != 1) { free(buf); return NULL; }
    b += 4;
    *len = n - (size_t)(b - buf);
    memmove(buf, b, *len + 1);
    return buf;
}

char *smp_http_post(int port, const char *path, const char *body, int *http, size_t *len) {
    return smp_http(port, path, body, http, len);
}

char *smp_http_get(int port, const char *path, int *http, size_t *len) {
    return smp_http(port, path, NULL, http, len);
}
