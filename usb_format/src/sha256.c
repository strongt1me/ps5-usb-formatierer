/* SHA-256 nach FIPS 180-4. Auf x86-64 mit SHA-Befehlen (SHA-NI: AMD Zen - auch die PS5 -, Intel ab Ice Lake/Goldmont)
 * rechnet der Prozessor die Runden selbst - etwa fünfmal schneller; sonst die einfache Fassung. */
#include "sha256.h"

#include <string.h>

#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#include <immintrin.h>
#define SHA_NI 1
#endif

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

#ifdef SHA_NI
/* n Blöcke mit SHA-NI (Aufbau wie in Intels Beschreibung der SHA-Erweiterungen): Zustand als ABEF/CDGH, je vier
 * Runden mit zwei sha256rnds2, Nachrichtenplan W[t] = msg2(msg1(W[t-4], W[t-3]) + W[t-1:t-2], W[t-1]) */
__attribute__((target("sha,sse4.1,ssse3")))
static void blocks_ni(uint32_t h[8], const uint8_t *p, size_t n) {
    const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[0]), 0xB1);      /* CDAB */
    __m128i st1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[4]), 0x1B);      /* EFGH */
    __m128i st0 = _mm_alignr_epi8(tmp, st1, 8);                                           /* ABEF */
    st1 = _mm_blend_epi16(st1, tmp, 0xF0);                                                /* CDGH */
    for (; n; n--, p += 64) {
        const __m128i abef = st0, cdgh = st1;
        __m128i w[4];
        for (int t = 0; t < 16; t++) {
            __m128i m;
            if (t < 4) {
                m = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * t)), swap);
            } else {
                m = _mm_sha256msg1_epu32(w[(t - 4) & 3], w[(t - 3) & 3]);
                m = _mm_add_epi32(m, _mm_alignr_epi8(w[(t - 1) & 3], w[(t - 2) & 3], 4));
                m = _mm_sha256msg2_epu32(m, w[(t - 1) & 3]);
            }
            w[t & 3] = m;
            m = _mm_add_epi32(m, _mm_loadu_si128((const __m128i *)&K[4 * t]));
            st1 = _mm_sha256rnds2_epu32(st1, st0, m);
            st0 = _mm_sha256rnds2_epu32(st0, st1, _mm_shuffle_epi32(m, 0x0E));
        }
        st0 = _mm_add_epi32(st0, abef);
        st1 = _mm_add_epi32(st1, cdgh);
    }
    tmp = _mm_shuffle_epi32(st0, 0x1B);                                                   /* FEBA */
    st1 = _mm_shuffle_epi32(st1, 0xB1);                                                   /* DCHG */
    _mm_storeu_si128((__m128i *)&h[0], _mm_blend_epi16(tmp, st1, 0xF0));                  /* DCBA */
    _mm_storeu_si128((__m128i *)&h[4], _mm_alignr_epi8(st1, tmp, 8));                     /* HGFE */
}

static int hw = -1;   /* -1: noch nicht nachgesehen */

int sha256_hw(void) {
    if (hw < 0) {
        unsigned a, b, c, d;
        int ok = __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 19)) && (c & (1u << 9));   /* SSE4.1, SSSE3 */
        hw = ok && __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 29));             /* SHA */
    }
    return hw;
}

void sha256_use_hw(int on) { hw = on ? -1 : 0; }
#else
int sha256_hw(void) { return 0; }
void sha256_use_hw(int on) { (void)on; }
#endif

static void blocks(sha256_t *s, const uint8_t *p, size_t n) {
#ifdef SHA_NI
    if (sha256_hw()) { blocks_ni(s->h, p, n); return; }
#endif
    for (; n; n--, p += 64) block(s, p);
}

void sha256_init(sha256_t *s) {
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, H0, sizeof(H0));
    s->len = 0;
    s->used = 0;
}

void sha256_update(sha256_t *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    if (s->used) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used < 64) return;
        blocks(s, s->buf, 1);
        s->used = 0;
    }
    blocks(s, p, n / 64);
    p += n / 64 * 64;
    n %= 64;
    memcpy(s->buf, p, n);
    s->used = n;
}

void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    sha256_update(s, &pad, 1);
    while (s->used != 56) sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void sha256_hex(const uint8_t d[32], char out[65]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[64] = 0;
}
