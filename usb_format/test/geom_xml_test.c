/* Test für geom_disk_descr()/geom_disk_ident() mit einem kern.geom.confxml-Auszug im Aufbau wie auf der PS5 */
#include <stdio.h>
#include <string.h>

#include "../src/geom_xml.h"

static const char *XML =
    "<mesh>\n"
    "  <class id=\"0x1\">\n    <name>MD</name>\n"
    "    <geom id=\"0x2\"><name>da1</name><provider><config><descr>Falsche Klasse</descr></config></provider></geom>\n"
    "  </class>\n"
    "  <class id=\"0x10\">\n    <name>DISK</name>\n"
    "    <geom id=\"0x11\">\n      <class ref=\"0x10\"/>\n      <name>da1</name>\n      <rank>1</rank>\n"
    "      <provider id=\"0x12\">\n        <geom ref=\"0x11\"/>\n        <mode>r1w0e1</mode>\n"
    "        <name>da1.sched.</name>\n        <mediasize>512110190592</mediasize>\n"
    "        <config>\n\t  <ident>012345678903</ident>\n\t  <descr>Realtek RTL9210C NVME</descr>\n"
    "        </config>\n      </provider>\n    </geom>\n"
    "    <geom id=\"0x20\">\n      <name>da10</name>\n      <provider id=\"0x21\"><geom ref=\"0x20\"/>\n"
    "        <config><descr>  SanDisk   Ultra &amp; Co  </descr></config></provider>\n    </geom>\n"
    "    <geom id=\"0x30\">\n      <name>da2</name>\n      <provider id=\"0x31\"><geom ref=\"0x30\"/>\n"
    "        <config><ident>X</ident></config></provider>\n    </geom>\n"
    "    <geom id=\"0x40\">\n      <name>da3</name>\n      <provider id=\"0x41\"><geom ref=\"0x40\"/>\n"
    "        <config><descr></descr></config></provider>\n    </geom>\n"
    "    <geom id=\"0x50\">\n      <name>ssd0</name>\n      <provider id=\"0x51\"><geom ref=\"0x50\"/>\n"
    "        <config><descr>Intern</descr></config></provider>\n    </geom>\n"
    "  </class>\n"
    "  <class id=\"0x60\">\n    <name>PART</name>\n"
    "    <geom id=\"0x61\"><name>da2</name><provider><config><descr>Nicht DISK</descr></config></provider></geom>\n"
    "  </class>\n"
    "</mesh>\n";

int main(void) {
    static const struct { const char *name, *want; } cases[] = {
        {"da1", "Realtek RTL9210C NVME"},
        {"da10", "SanDisk Ultra & Co"},
        {"da2", ""},      /* kein descr -> nicht aus PART-Klasse nehmen */
        {"da3", ""},      /* leeres descr */
        {"da4", ""},      /* nicht vorhanden */
        {"ssd0", "Intern"},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[96] = "x";
        geom_disk_descr(XML, cases[i].name, out, sizeof(out));
        int ok = strcmp(out, cases[i].want) == 0;
        fails += !ok;
        printf("%s %-5s -> '%s'%s\n", ok ? "OK    " : "FEHLER", cases[i].name, out, ok ? "" : " (erwartet anders)");
    }
    char tiny[8];
    geom_disk_descr(XML, "da1", tiny, sizeof(tiny));
    int ok = strcmp(tiny, "Realtek") == 0;
    fails += !ok;
    printf("%s kleiner Puffer -> '%s'\n", ok ? "OK    " : "FEHLER", tiny);

    /* Seriennummer (<ident>) */
    static const struct { const char *name, *want; } idents[] = {
        {"da1", "012345678903"}, {"da2", "X"}, {"da10", ""}, {"da4", ""},
    };
    for (size_t i = 0; i < sizeof(idents) / sizeof(idents[0]); i++) {
        char out[96] = "x";
        geom_disk_ident(XML, idents[i].name, out, sizeof(out));
        ok = strcmp(out, idents[i].want) == 0;
        fails += !ok;
        printf("%s ident %-5s -> '%s'%s\n", ok ? "OK    " : "FEHLER", idents[i].name, out, ok ? "" : " (erwartet anders)");
    }
    printf(fails ? "%d Fehler\n" : "Alle GEOM-Tests bestanden.\n", fails);
    return fails != 0;
}
