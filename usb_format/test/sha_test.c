/* SHA-256: bekannte Prüfwerte und Hardware-Fassung (SHA-NI) gegen die einfache - zufällige Längen und Teilstücke.
 *   sha_test   -> "hw=1|0", Fehler mit Exit 1 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/sha256.h"

static void hex(int hw, const void *p, size_t n, size_t piece, char out[65]) {
    sha256_use_hw(hw);
    sha256_t s;
    uint8_t d[32];
    sha256_init(&s);
    for (size_t o = 0; o < n; o += piece) sha256_update(&s, (const uint8_t *)p + o, n - o < piece ? n - o : piece);
    sha256_final(&s, d);
    sha256_hex(d, out);
}

int main(void) {
    int fails = 0, have = (sha256_use_hw(1), sha256_hw());
    struct { const char *in; size_t reps; const char *want; } kv[] = {
        {"", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (size_t k = 0; k < sizeof(kv) / sizeof(kv[0]); k++) {
        size_t l = strlen(kv[k].in), n = l * kv[k].reps;
        char *b = malloc(n + 1), got[65];
        for (size_t r = 0; r < kv[k].reps; r++) memcpy(b + r * l, kv[k].in, l);
        for (int hw = 0; hw <= have; hw++) {
            hex(hw, b, n, n ? n : 1, got);
            if (strcmp(got, kv[k].want)) { printf("FEHLER Prüfwert %zu (hw=%d): %s\n", k, hw, got); fails++; }
        }
        free(b);
    }
    srand(12345);
    size_t max = 1 << 20;
    unsigned char *b = malloc(max);
    for (size_t i = 0; i < max; i++) b[i] = (unsigned char)rand();
    for (int t = 0; t < 300 && have; t++) {
        size_t n = t < 130 ? (size_t)t : (size_t)rand() % max, piece = 1 + (size_t)rand() % (t % 3 ? 200 : 70000);
        char a[65], c[65];
        hex(0, b, n, piece, a);
        hex(1, b, n, n ? n : 1, c);
        if (strcmp(a, c)) { printf("FEHLER Länge %zu Stück %zu\n", n, piece); fails++; }
    }
    free(b);
    printf("hw=%d %s\n", have, fails ? "FEHLER" : "SHA-256 stimmt");
    return fails != 0;
}
