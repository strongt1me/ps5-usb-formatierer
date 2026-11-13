/*
 * usb_format - formatiert USB-Sticks an der PS5 als exFAT (64 KB) oder FAT32.
 * Bedienung über Browser (http://<PS5-IP>:8090) oder das Windows-Programm.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "platform.h"
#include "server.h"

#define ENDS_LOG_MAX (16 * 1024)

static void stamp(time_t t, char *out, size_t n) {
    struct tm tm;
    snprintf(out, n, "?");
    if (localtime_r(&t, &tm)) strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

/* Lag <datenordner>/running beim Start noch da, endete der letzte Lauf nicht sauber. Warum, steht danach als Zeile in
 * ends.log ("start  version  ended: ...  (noticed ...)"), die der Diagnose-Bericht zeigt:
 *  - Stopp-Befehl nach dem Start in quit.log: beim Beenden unterbrochen
 *  - andere Startzeit der PS5 und kleinere Prozessnummer: die PS5 wurde neu gestartet oder ausgeschaltet (sie zählt
 *    ab dem Start hoch; bei gleicher Startzeit ist der Zähler nur übergelaufen)
 *  - andere Startzeit, größere Prozessnummer: Neustart, Ruhemodus oder gestellte Uhr (FreeBSD verschiebt dann
 *    kern.boottime) - nicht zu trennen, die PS5 vergibt nach dem Start nur langsam neue Prozessnummern
 *  - gleiche Startzeit: abgestürzt oder von außen beendet (ein anderer Payload, Speichermangel)
 * Ohne Prozessnummer (running-Datei älterer Fassungen) entscheidet nur die Startzeit der PS5. */
static void note_last_end(void) {
    char path[256], run[160] = "", quit[400] = "", line[400];
    snprintf(path, sizeof(path), "%s/running", plat_data_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(run, sizeof(run), f)) run[0] = 0;
    fclose(f);
    run[strcspn(run, "\r\n")] = 0;
    if (strlen(run) < 19) return;
    snprintf(path, sizeof(path), "%s/quit.log", plat_data_dir());
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof(line), f)) snprintf(quit, sizeof(quit), "%s", line);
        fclose(f);
    }
    char *b = strstr(run, "  boot "), *p = strstr(run, "  pid ");
    long long was = b ? strtoll(b + 7, NULL, 10) : 0, now_boot = plat_boot_time();
    long old_pid = p ? strtol(p + 6, NULL, 10) : 0, pid = (long)getpid();
    if (b) *b = 0;   /* in ends.log nur "zeit  version" */
    if (p) *p = 0;
    int known = was && now_boot, moved = known && llabs(now_boot - was) > 120;
    struct tm st = {0};   /* running älterer Fassungen: Startzeit des Laufs gegen die Startzeit der PS5 */
    if (!was && now_boot && old_pid <= 0 && sscanf(run, "%d-%d-%d %d:%d:%d", &st.tm_year, &st.tm_mon, &st.tm_mday,
                                                     &st.tm_hour, &st.tm_min, &st.tm_sec) == 6) {
        st.tm_year -= 1900;
        st.tm_mon -= 1;
        st.tm_isdst = -1;
        time_t t = mktime(&st);
        if (t != (time_t)-1) { known = 1; moved = (long long)t + 120 < now_boot; }
    }
    const char *why;
    /* gleiche Uhr, gleiches Format: als Text vergleichbar. Echt später: in derselben Sekunde gestoppt und neu gestartet
     * (tools/ps5_deploy.py update), gehört der Befehl zum Lauf davor */
    if (strlen(quit) >= 19 && strncmp(quit, run, 19) > 0)
        why = "stop command, but did not finish";
    else if (old_pid > 0 && pid < old_pid && (moved || !known))   /* gleiche Startzeit: Zähler nur übergelaufen */
        why = "PS5 restarted or switched off";
    else if (moved)   /* größere Prozessnummer beweist nichts: die PS5 vergibt nach dem Start nur langsam neue */
        why = old_pid > 0 ? "PS5 restarted, switched off, in rest mode or its clock was set"
                          : "PS5 restarted, switched off or in rest mode";
    else if (old_pid > 0 || known)
        why = "crashed or killed (PS5 kept running)";
    else
        why = "unknown (no PS5 start time)";
    char now[32];
    stamp(time(NULL), now, sizeof(now));
    snprintf(path, sizeof(path), "%s/ends.log", plat_data_dir());
    struct stat sb;
    if (stat(path, &sb) == 0 && sb.st_size > ENDS_LOG_MAX) {
        char old[264];
        snprintf(old, sizeof(old), "%s.1", path);
        rename(path, old);
    }
    if ((f = fopen(path, "a"))) {
        fprintf(f, "%s  ended: %s  (noticed %s)\n", run, why, now);
        fclose(f);
    }
}

/* <datenordner>/running: liegt da, solange der Formatierer läuft (Startzeit, Version, Startzeit der PS5,
 * Prozessnummer). Ist Port 8090 zu und die Datei noch da, beendet er sich gerade oder ist abgestürzt - quit.log sagt,
 * welches (tools/ps5_deploy.py status); beim nächsten Start hält note_last_end() fest, wie er endete. */
static void mark_running(int on) {
    char path[256];
    snprintf(path, sizeof(path), "%s/running", plat_data_dir());
    if (!on) { unlink(path); return; }
    mkdir(plat_data_dir(), 0755);
    FILE *f = fopen(path, "w");
    if (!f) return;
    char when[32];
    stamp(time(NULL), when, sizeof(when));
    long long boot = plat_boot_time();
    if (boot) fprintf(f, "%s  %s  boot %lld  pid %ld\n", when, USBFMT_VERSION, boot, (long)getpid());
    else fprintf(f, "%s  %s  pid %ld\n", when, USBFMT_VERSION, (long)getpid());
    fclose(f);
}

int main(void) {
    char ip[64], msg[320];
    int s;

    signal(SIGPIPE, SIG_IGN);
    /* zur Sicherheit: ein unbekannter Systemaufruf (Bereitschaftsabfrage von kstuff, platform_ps5.c) darf den Server
     * nie beenden - FreeBSD kürzt die Nummer zwar auf 32 Bit, ein anderer Kernel schickte sonst SIGSYS */
    signal(SIGSYS, SIG_IGN);
    /* Meldungen in der Systemsprache der PS5, gibt es sie nicht: Englisch (eine in der App gewählte setzt server_listen) */
    i18n_default(plat_system_lang());

    if ((s = server_listen(SERVER_PORT)) < 0) {
        snprintf(msg, sizeof(msg), T("USB-Formatierer: Port %d belegt (läuft er schon?)"), SERVER_PORT);
        printf("%s\n", msg);
        plat_notify(msg);
        return 1;
    }

    plat_local_ip(ip, sizeof(ip));
    snprintf(msg, sizeof(msg), T("USB-Formatierer bereit: http://%s:%d"), ip, SERVER_PORT);
    printf("%s\n", msg);
    fflush(stdout);
    plat_notify(msg);
    note_last_end();
    mark_running(1);

    server_serve(s);
    mark_running(0);
    server_quit_message(msg, sizeof(msg));
    plat_notify(msg);
    return 0;
}
