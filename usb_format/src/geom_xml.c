#include <stdio.h>
#include <string.h>

#include "geom_xml.h"

/* XML-Text kopieren: Entities auflösen, Leerraum zusammenfassen, trimmen */
static void xml_text(char *dst, size_t n, const char *src, size_t len) {
    static const struct { const char *ent; char c; } ents[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; ) {
        char c = src[i];
        size_t adv = 1;
        if (c == '&') {
            for (size_t k = 0; k < sizeof(ents) / sizeof(ents[0]); k++) {
                size_t el = strlen(ents[k].ent);
                if (i + el <= len && strncmp(src + i, ents[k].ent, el) == 0) { c = ents[k].c; adv = el; break; }
            }
        }
        i += adv;
        if ((unsigned char)c < 0x20) c = ' ';
        if (c == ' ' && (o == 0 || dst[o - 1] == ' ')) continue;
        dst[o++] = c;
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

/* Feld <field> aus dem Provider-config von Laufwerk "name" in der Klasse DISK */
static int geom_disk_field(const char *xml, const char *name, const char *field, char *out, size_t n) {
    char tag[48], open[32], close[32];
    const char *cls, *end, *g, *next, *d, *de;

    out[0] = 0;
    if (!(cls = strstr(xml, "<name>DISK</name>")) || !(end = strstr(cls, "</class>"))) return 0;
    snprintf(tag, sizeof(tag), "<name>%s</name>", name);
    snprintf(open, sizeof(open), "<%s>", field);
    snprintf(close, sizeof(close), "</%s>", field);
    for (g = strstr(cls, tag); g && g < end; g = strstr(g + 1, tag)) {
        next = strstr(g, "<geom id=");  /* Beginn des nächsten Laufwerks ("<geom ref=" steckt im Provider) */
        if (!next || next > end) next = end;
        d = strstr(g, open);
        if (!d || d > next) continue;
        d += strlen(open);
        de = strstr(d, close);
        if (!de || de > next) continue;
        xml_text(out, n, d, (size_t)(de - d));
        return out[0] != 0;
    }
    return 0;
}

int geom_disk_descr(const char *xml, const char *name, char *out, size_t n) {
    return geom_disk_field(xml, name, "descr", out, n);
}

void ident_clean(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) *s = '_';
}

int geom_disk_ident(const char *xml, const char *name, char *out, size_t n) {
    int r = geom_disk_field(xml, name, "ident", out, n);
    ident_clean(out);
    return r;
}
