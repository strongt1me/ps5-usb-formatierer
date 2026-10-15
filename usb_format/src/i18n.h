#ifndef I18N_H
#define I18N_H

/* Sprachen der Meldungen: Deutsch (die Texte im Code), Englisch, Italienisch, Spanisch, Französisch.
 * T("deutscher Text") liefert die Übersetzung in der Sprache des aufrufenden Threads (Standard: Deutsch).
 * Die Tabelle (i18n_table.c) erzeugt tools/i18n_gen.py aus i18n/core.json; dort sind %-Platzhalter in allen
 * Sprachen gleich (sonst bricht der Bau ab). */

enum { LANG_DE, LANG_EN, LANG_IT, LANG_ES, LANG_FR, LANG_COUNT };

void        i18n_set(int lang);            /* für den aktuellen Thread */
int         i18n_get(void);
int         i18n_parse(const char *code);   /* "de", "en", "it", "es", "fr" (auch "en-US") -> LANG_*, sonst -1 */
const char *i18n_code(int lang);
const char *T(const char *de);

/* Kennzeichnung für tools/i18n_gen.py in Tabellen, die zur Laufzeit erst über T() übersetzt werden */
#define N_(s) (s)

/* Eintrag der erzeugten Tabelle, nach dem deutschen Text sortiert */
typedef struct {
    const char *text[LANG_COUNT];
} i18n_entry_t;

extern const i18n_entry_t i18n_table[];
extern const unsigned i18n_count;

#endif
