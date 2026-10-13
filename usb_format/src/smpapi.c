/* Kleiner HTTP-Client für die API von ShadowMountPlus (nur 127.0.0.1) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "smpapi.h"

#define SMP_MAX (8 * 1024 * 1024)

char *smp_http_post(int port, const char *path, const char *body, int *http, size_t *len) {
    char head[256];
    size_t blen = strlen(body);
    int hl = snprintf(head, sizeof(head), "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n", path, blen);
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
    while (buf) {
        if (n == cap) {
            if (cap >= SMP_MAX) break;
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        ssize_t k = recv(s, buf + n, cap - n, 0);
        if (k <= 0) break;
        n += (size_t)k;
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
