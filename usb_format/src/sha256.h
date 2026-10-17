#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4) - dieselbe kleine Fassung auf PS5, Windows und Linux */
typedef struct {
    uint32_t h[8];
    uint64_t len;        /* Bytes bisher */
    uint8_t  buf[64];
    size_t   used;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t n);
void sha256_final(sha256_t *s, uint8_t out[32]);
/* 64 Hex-Zeichen + Nullbyte */
void sha256_hex(const uint8_t d[32], char out[65]);

#endif
