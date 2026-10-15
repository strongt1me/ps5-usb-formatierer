/*
 * usb_format - formatiert USB-Sticks an der PS5 als exFAT (64 KB) oder FAT32.
 * Bedienung über Browser (http://<PS5-IP>:8090) oder das Windows-Programm.
 */
#include <signal.h>
#include <stdio.h>

#include "i18n.h"
#include "platform.h"
#include "server.h"

int main(void) {
    char ip[64], msg[320];
    int s;

    signal(SIGPIPE, SIG_IGN);

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

    server_serve(s);
    plat_notify(T("USB-Formatierer beendet"));
    return 0;
}
