/* Erkennt PS4-fPKG am Image-Schlüssel (siehe pkgsign.h). Ablauf wie in shadPS4 (core/file_format/pkg.cpp):
 *  1. Eintrag 0x10 (ENTRY_KEYS): Schlüssel Nr. 3 mit dem öffentlich bekannten Schlüssel "DK3" per RSA (PKCS#1 v1.5) entschlüsseln
 *  2. Eintrag 0x20 (IMAGE_KEY): AES-128-CBC mit Schlüssel/IV aus SHA-256(Tabelleneintrag + DK3), dann RSA mit dem Fake-Schlüssel
 *  3. gültige Polsterung mit 32 Byte Nutzdaten (EKPFS) = fPKG, sonst offiziell
 * Eigene kleine Fassung von RSA-2048 (Chinesischer Restsatz, Montgomery) und AES-128, ohne Fremdbibliothek. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "pkgsign.h"
#include "pkgsign_keys.h"
#include "sha256.h"

#define L 32   /* 1024 Bit = 32 Wörter zu 32 Bit */
typedef uint32_t bn[L];

static void bn_from_be(bn r, const uint8_t *b, size_t n) {   /* n <= 128 */
    memset(r, 0, sizeof(bn));
    for (size_t i = 0; i < n; i++) r[(n - 1 - i) / 4] |= (uint32_t)b[i] << (8 * ((n - 1 - i) % 4));
}

static void bn_to_be(const bn a, uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[n - 1 - i] = (uint8_t)(a[i / 4] >> (8 * (i % 4)));
}

static int bn_cmp(const bn a, const bn b) {
    for (int i = L - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

static uint32_t bn_sub(bn r, const bn a, const bn b) {   /* r = a - b, Rückgabe: Borrow */
    uint64_t br = 0;
    for (int i = 0; i < L; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - br;
        r[i] = (uint32_t)d;
        br = (d >> 32) & 1;
    }
    return (uint32_t)br;
}

static uint32_t bn_add(bn r, const bn a, const bn b) {
    uint64_t c = 0;
    for (int i = 0; i < L; i++) {
        uint64_t s = (uint64_t)a[i] + b[i] + c;
        r[i] = (uint32_t)s;
        c = s >> 32;
    }
    return (uint32_t)c;
}

/* r = 2r + bit (mod n) */
static void bn_dbl_mod(bn r, unsigned bit, const bn n) {
    uint32_t carry = bit;
    for (int i = 0; i < L; i++) {
        uint32_t next = r[i] >> 31;
        r[i] = r[i] << 1 | carry;
        carry = next;
    }
    if (carry || bn_cmp(r, n) >= 0) bn_sub(r, r, n);
}

/* r = Zahl aus be[0..len) mod n */
static void bn_mod_be(bn r, const uint8_t *be, size_t len, const bn n) {
    memset(r, 0, sizeof(bn));
    for (size_t i = 0; i < len; i++)
        for (int b = 7; b >= 0; b--) bn_dbl_mod(r, (be[i] >> b) & 1, n);
}

typedef struct {
    bn       n, rr;   /* Modul, R^2 mod n (R = 2^1024) */
    uint32_t n0;      /* -n^-1 mod 2^32 */
} mctx;

static void mctx_init(mctx *c, const bn n) {
    memcpy(c->n, n, sizeof(bn));
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - n[0] * inv;   /* Newton: n^-1 mod 2^32 */
    c->n0 = (uint32_t)0 - inv;
    memset(c->rr, 0, sizeof(bn));
    c->rr[0] = 1;
    for (int i = 0; i < 2 * 32 * L; i++) bn_dbl_mod(c->rr, 0, n);   /* 2^2048 mod n */
}

/* r = a * b / R mod n (CIOS) */
static void mont_mul(const mctx *c, bn r, const bn a, const bn b) {
    uint32_t t[L + 2];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < L; i++) {
        uint64_t carry = 0, s;
        for (int j = 0; j < L; j++) {
            s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
        }
        s = (uint64_t)t[L] + carry;
        t[L] = (uint32_t)s;
        t[L + 1] = (uint32_t)(s >> 32);
        const uint32_t m = t[0] * c->n0;
        carry = ((uint64_t)t[0] + (uint64_t)m * c->n[0]) >> 32;
        for (int j = 1; j < L; j++) {
            s = (uint64_t)t[j] + (uint64_t)m * c->n[j] + carry;
            t[j - 1] = (uint32_t)s;
            carry = s >> 32;
        }
        s = (uint64_t)t[L] + carry;
        t[L - 1] = (uint32_t)s;
        t[L] = t[L + 1] + (uint32_t)(s >> 32);
    }
    bn res;
    memcpy(res, t, sizeof(bn));
    if (t[L] || bn_cmp(res, c->n) >= 0) bn_sub(res, res, c->n);
    memcpy(r, res, sizeof(bn));
}

/* r = base^e mod n; e: 128 Byte Big-Endian, base < n */
static void mod_exp(const mctx *c, bn r, const bn base, const uint8_t e[128]) {
    bn x, acc, one;
    memset(one, 0, sizeof(bn));
    one[0] = 1;
    mont_mul(c, x, base, c->rr);
    mont_mul(c, acc, one, c->rr);
    for (int i = 0; i < 128; i++)
        for (int b = 7; b >= 0; b--) {
            mont_mul(c, acc, acc, acc);
            if ((e[i] >> b) & 1) mont_mul(c, acc, acc, x);
        }
    mont_mul(c, r, acc, one);
}

typedef struct {
    const uint8_t *p, *q, *dp, *dq, *qi;
} rsakey_t;

/* m = c^d mod (p*q) über den Chinesischen Restsatz; c, m: 256 Byte Big-Endian */
static void rsa_private(const rsakey_t *k, const uint8_t c[256], uint8_t m[256]) {
    bn p, q, c1, c2, m1, m2, m2p, diff, qi, h, t;
    mctx mp, mq;
    uint8_t tmp[128];
    bn_from_be(p, k->p, 128);
    bn_from_be(q, k->q, 128);
    bn_from_be(qi, k->qi, 128);
    mctx_init(&mp, p);
    mctx_init(&mq, q);
    bn_mod_be(c1, c, 256, p);
    bn_mod_be(c2, c, 256, q);
    mod_exp(&mp, m1, c1, k->dp);
    mod_exp(&mq, m2, c2, k->dq);
    bn_to_be(m2, tmp, 128);
    bn_mod_be(m2p, tmp, 128, p);
    if (bn_sub(diff, m1, m2p)) bn_add(diff, diff, p);   /* (m1 - m2) mod p */
    mont_mul(&mp, t, diff, mp.rr);                       /* diff * R */
    mont_mul(&mp, h, t, qi);                             /* diff * qinv mod p */
    /* m = m2 + h * q (2048 Bit) */
    uint32_t w[2 * L + 1];
    memset(w, 0, sizeof(w));
    for (int i = 0; i < L; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < L; j++) {
            uint64_t s = (uint64_t)w[i + j] + (uint64_t)h[i] * q[j] + carry;
            w[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        w[i + L] += (uint32_t)carry;
    }
    uint64_t carry = 0;
    for (int i = 0; i < 2 * L; i++) {
        uint64_t s = (uint64_t)w[i] + (i < L ? m2[i] : 0) + carry;
        w[i] = (uint32_t)s;
        carry = s >> 32;
    }
    for (int i = 0; i < 256; i++) m[255 - i] = (uint8_t)(w[i / 4] >> (8 * (i % 4)));
}

/* PKCS#1 v1.5 (Typ 2): Nutzdaten und ihre Länge, sonst NULL */
static const uint8_t *pkcs1_unpad(const uint8_t m[256], size_t *len) {
    if (m[0] != 0 || m[1] != 2) return NULL;
    size_t i = 2;
    while (i < 256 && m[i] != 0) i++;
    if (i < 10 || i >= 256) return NULL;
    *len = 256 - i - 1;
    return m + i + 1;
}

/* ---------------------------------------------------------------- AES-128 (nur Entschlüsseln, CBC) */

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t ISBOX[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static uint8_t xt(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void aes_expand(const uint8_t key[16], uint8_t rk[11][16]) {
    uint8_t w[44][4];
    uint8_t rc = 1;
    memcpy(w, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, w[i - 1], 4);
        if (i % 4 == 0) {
            const uint8_t t0 = t[0];
            t[0] = SBOX[t[1]] ^ rc;
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[t0];
            rc = xt(rc);
        }
        for (int j = 0; j < 4; j++) w[i][j] = w[i - 4][j] ^ t[j];
    }
    for (int r = 0; r < 11; r++) memcpy(rk[r], w[r * 4], 16);
}

static void aes_dec_block(uint8_t rk[11][16], const uint8_t in[16], uint8_t out[16]) {
    static const uint8_t inv_shift[16] = {0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3};
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[10][i];
    for (int r = 9; r >= 0; r--) {
        for (int i = 0; i < 16; i++) t[i] = ISBOX[s[inv_shift[i]]] ^ rk[r][i];
        if (r) {
            for (int c = 0; c < 4; c++) {
                const uint8_t *a = t + c * 4;
                s[c * 4 + 0] = gmul(a[0], 14) ^ gmul(a[1], 11) ^ gmul(a[2], 13) ^ gmul(a[3], 9);
                s[c * 4 + 1] = gmul(a[0], 9) ^ gmul(a[1], 14) ^ gmul(a[2], 11) ^ gmul(a[3], 13);
                s[c * 4 + 2] = gmul(a[0], 13) ^ gmul(a[1], 9) ^ gmul(a[2], 14) ^ gmul(a[3], 11);
                s[c * 4 + 3] = gmul(a[0], 11) ^ gmul(a[1], 13) ^ gmul(a[2], 9) ^ gmul(a[3], 14);
            }
        } else memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

/* ---------------------------------------------------------------- Paket */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int read_at(int fd, void *buf, size_t n, uint64_t off) {
    return pread(fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}

int pkg4_is_fake(int fd) {
    uint8_t h[0x20], ent[32 * 64];
    if (read_at(fd, h, sizeof(h), 0) != 0 || memcmp(h, "\x7f" "CNT", 4) != 0) return -1;
    const uint32_t count = be32(h + 0x10), table = be32(h + 0x18);
    uint32_t off10 = 0, size10 = 0, off20 = 0, size20 = 0;
    uint8_t raw20[32];
    int have10 = 0, have20 = 0;
    if (!count || count > 4096 || table < 0x20 || table > 0x100000) return -1;
    for (uint32_t i = 0; i < count && !(have10 && have20); i += 64) {   /* Tabelle in Stücken zu 64 Einträgen */
        const uint32_t n = count - i < 64 ? count - i : 64;
        if (read_at(fd, ent, (size_t)n * 32, (uint64_t)table + (uint64_t)i * 32) != 0) return -1;
        for (uint32_t j = 0; j < n; j++) {
            const uint8_t *e = ent + j * 32;
            if (be32(e) == 0x10) { have10 = 1; off10 = be32(e + 16); size10 = be32(e + 20); }
            if (be32(e) == 0x20) { have20 = 1; memcpy(raw20, e, 32); off20 = be32(e + 16); size20 = be32(e + 20); }
        }
    }
    if (!have10 || !have20 || size10 < 32 + 7 * 32 + 7 * 256 || size20 < 256) return -1;
    uint8_t c[256], m[256], img[256], dec[256];
    static const rsakey_t DK3 = {DK3_P, DK3_Q, DK3_DP, DK3_DQ, DK3_QI}, FAKE = {FAKE_P, FAKE_Q, FAKE_DP, FAKE_DQ, FAKE_QI};
    if (read_at(fd, c, 256, (uint64_t)off10 + 32 + 7 * 32 + 3 * 256) != 0 || read_at(fd, img, 256, off20) != 0) return -1;
    rsa_private(&DK3, c, m);
    size_t len = 0;
    const uint8_t *dk3 = pkcs1_unpad(m, &len);
    if (!dk3 || len < 32) return -1;   /* Schlüssel 3 nicht entschlüsselbar: kein erkennbares Paket */
    uint8_t cat[64], key[32];
    sha256_t sh;
    memcpy(cat, raw20, 32);
    memcpy(cat + 32, dk3, 32);
    sha256_init(&sh);
    sha256_update(&sh, cat, 64);
    sha256_final(&sh, key);
    uint8_t rk[11][16];
    aes_expand(key + 16, rk);
    const uint8_t *prev = key;   /* IV = erste 16 Byte */
    for (int i = 0; i < 256; i += 16) {
        uint8_t blk[16];
        aes_dec_block(rk, img + i, blk);
        for (int j = 0; j < 16; j++) dec[i + j] = blk[j] ^ prev[j];
        prev = img + i;
    }
    rsa_private(&FAKE, dec, m);
    const uint8_t *ek = pkcs1_unpad(m, &len);
    return ek && len == 32 ? 1 : 0;
}
