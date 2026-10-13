#ifndef SERVER_H
#define SERVER_H

#ifndef SERVER_PORT
#define SERVER_PORT 8090
#endif

int  server_listen(int port);   /* Socket oder -1, wenn der Port belegt ist */
void server_serve(int sock);    /* blockiert, bis /api/quit aufgerufen wird */
#endif
