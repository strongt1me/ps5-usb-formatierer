/* Übersetzung der Meldungen (siehe i18n.h). Sprache je Thread: Windows-Helfer einfach global (ein Auftrag je
 * Prozess), sonst über einen pthread-Schlüssel - die PS5-Payloads richten keinen Thread-lokalen Speicher (__thread)
 * ein, pthread_getspecific geht dagegen überall. */
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"

static const char *const codes[LANG_COUNT] = {"de", "en", "it", "es", "fr"};

#ifdef _WIN32
static int current;
void i18n_set(int lang) { current = lang >= 0 && lang < LANG_COUNT ? lang : LANG_DE; }
int i18n_get(void) { return current; }
#else
#include <pthread.h>
static pthread_key_t key;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void make_key(void) { pthread_key_create(&key, NULL); }

void i18n_set(int lang) {
    pthread_once(&once, make_key);
    pthread_setspecific(key, (void *)(intptr_t)(lang >= 0 && lang < LANG_COUNT ? lang + 1 : LANG_DE + 1));
}

int i18n_get(void) {
    pthread_once(&once, make_key);
    intptr_t v = (intptr_t)pthread_getspecific(key);
    return v > 0 && v <= LANG_COUNT ? (int)v - 1 : LANG_DE;   /* nie gesetzt: Deutsch */
}
#endif

int i18n_parse(const char *code) {
    if (!code) return -1;
    for (int i = 0; i < LANG_COUNT; i++)
        if (!strncasecmp(code, codes[i], 2) && (code[2] == 0 || strchr("-_,;& ", code[2])))
            return i;
    return -1;
}

const char *i18n_code(int lang) { return codes[lang >= 0 && lang < LANG_COUNT ? lang : LANG_DE]; }

const char *T(const char *de) {
    int lang = i18n_get();
    if (lang == LANG_DE || !de) return de;
    unsigned lo = 0, hi = i18n_count;   /* binäre Suche: Tabelle nach dem deutschen Text sortiert */
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(i18n_table[mid].text[LANG_DE], de);
        if (!c) return i18n_table[mid].text[lang] ? i18n_table[mid].text[lang] : de;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return de;   /* nicht übersetzt: Deutsch statt nichts */
}
