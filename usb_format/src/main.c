/*
 * usb_format - formatiert USB-Sticks an der PS5 als exFAT (64 KB) oder FAT32.
 * Bedienung über Browser (http://<PS5-IP>:8090) oder das Windows-Programm.
 */
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "i18n.h"
#include "platform.h"
#include "server.h"

/* <datenordner>/running: liegt da, solange der Formatierer läuft (Version, Startzeit). Ist Port 8090 zu und die Datei
 * noch da, beendet er sich gerade oder ist abgestürzt - quit.log sagt, welches (tools/ps5_deploy.py status). */
static void mark_running(int on) {
    char path[256];
    snprintf(path, sizeof(path), "%s/running", plat_data_dir());
    if (!on) { unlink(path); return; }
    mkdir(plat_data_dir(), 0755);
    FILE *f = fopen(path, "w");
    if (!f) return;
    char when[32] = "?";
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s  %s\n", when, USBFMT_VERSION);
    fclose(f);
}

int main(void) {
    char ip[64], msg[320];
    int s;

    signal(SIGPIPE, SIG_IGN);
    /* zur Sicherheit: ein unbekannter Systemaufruf (Bereitschaftsabfrage von kstuff, platform_ps5.c) darf den Server
     * nie beenden - FreeBSD kürzt die Nummer zwar auf 32 Bit, ein anderer Kernel schickte sonst SIGSYS */
    signal(SIGSYS, SIG_IGN);

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
    mark_running(1);

    server_serve(s);
    mark_running(0);
    server_quit_message(msg, sizeof(msg));
    plat_notify(msg);
    return 0;
}
