#ifndef SMPAPI_H
#define SMPAPI_H

#include <stddef.h>

/* POST <path> mit JSON-Körper an die HTTP-API von ShadowMountPlus (127.0.0.1:port). Rückgabe: Antwortkörper
 * (mit free() freigeben) und HTTP-Status in *http, oder NULL, wenn ShadowMountPlus nicht erreichbar ist. */
char *smp_http_post(int port, const char *path, const char *body, int *http, size_t *len);

#endif
