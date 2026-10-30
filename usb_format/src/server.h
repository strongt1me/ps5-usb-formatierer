#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#ifndef SERVER_PORT
#define SERVER_PORT 8090
#endif

/* Version aus der Datei VERSION im Projekt (Makefile bzw. test/host_build.sh), über /api/info gemeldet */
#ifndef USBFMT_VERSION
#define USBFMT_VERSION "unbekannt"
#endif

int  server_listen(int port);   /* Socket oder -1, wenn der Port belegt ist */
void server_serve(int sock);    /* blockiert, bis /api/quit aufgerufen wird */
/* nach server_serve: Benachrichtigung "beendet" mit Herkunft des Stopp-Befehls (Weboberfläche, PC-Programm ...) */
void server_quit_message(char *out, size_t n);
#endif
