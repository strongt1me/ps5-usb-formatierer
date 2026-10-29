/*
 * Starter der App "USB-Formatierer" (PPSA86920, Medien; Dump-Ordner, siehe dump/) als nativer PS5-Titel.
 *
 * Läuft der Formatierer schon (Port 8090), bittet er ihn, die Web-UI im PS5-Browser zu öffnen.
 * Sonst schickt er die eingebettete usb_format.elf an den ELF-Loader des Jailbreaks (Port 9021),
 * wartet, bis der Formatierer antwortet, und öffnet dann die Web-UI. Danach beendet er sich (siehe _start).
 *
 * Gebaut wie ein nativer Titel (app/build_title.sh): Der App-Lader der PS5 verlangt einen echten
 * Prozess-Parameter-Block und eine passende Signatur; ein statisches Programm mit direkten
 * Systemaufrufen (frühere app/eboot.c) startete auf FW 12 nicht (CE-107750-0). Benutzt werden nur
 * Funktionen aus libkernel und libSceSystemService, keine C-Bibliothek: Daher -ffreestanding, kein
 * memset/memcpy. (Die libc.prx legt dump/Program.cs trotzdem in den Dump-Ordner: der App-Lader verlangt sie in jedem Titel.)
 */
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#define PORT_UI     8090
#define PORT_LOADER 9021

/* libkernel / libSceSystemService (Stubs aus target/lib des SDK) */
int close(int fd);
int sceKernelUsleep(unsigned int microseconds);
int sceKernelDebugOutText(int channel, const char *text);
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);

__asm__(".section .rodata\n"
        ".global payload_elf\n"
        "payload_elf:\n"
        ".incbin \"usb_format.elf\"\n"
        ".global payload_elf_end\n"
        "payload_elf_end:\n"
        ".previous\n");
extern const unsigned char payload_elf[], payload_elf_end[];

/* vom Linker-Skript (title.ld); der Lader der PS5 nullt den BSS nicht */
extern char __bss_start[], __bss_end[];

static struct {
    char reserved[45];
    char message[3075];
} notify_request;

static size_t length(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Meldung im Kernel-Log und als Benachrichtigung auf dem Bildschirm */
static void report(const char *msg) {
    volatile char *dst = notify_request.message;
    size_t i = 0;
    for (; msg[i] && i + 1 < sizeof(notify_request.message); i++) dst[i] = msg[i];
    dst[i] = 0;
    sceKernelDebugOutText(0, msg);
    sceKernelDebugOutText(0, "\n");
    sceKernelSendNotificationRequest(0, &notify_request, sizeof(notify_request), 0);
}

static int connect_local(int port) {
    struct sockaddr_in sa;
    volatile unsigned char *p = (volatile unsigned char *)&sa;
    for (size_t i = 0; i < sizeof(sa); i++) p[i] = 0;
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    /* nie endlos warten: nimmt der ELF-Loader an, liest aber nicht (bzw. hängt der Formatierer), endete der Titel
     * sonst nie - ohne Meldung */
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(s, (const struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int send_all(int s, const unsigned char *p, size_t n) {
    while (n) {
        ssize_t w = send(s, p, n, MSG_NOSIGNAL);   /* schließt der Loader vorzeitig: Fehler statt SIGPIPE-Abbruch */
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* POST /api/open: Statuscode der Antwort (200 = Browser geöffnet), 0 = Formatierer läuft nicht, -1 = er hat die
 * Verbindung angenommen, aber nicht lesbar geantwortet */
static int open_ui_once(void) {
    static const char req[] = "POST /api/open HTTP/1.1\r\nHost: 127.0.0.1:8090\r\n"
                              "Content-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
    char buf[256];
    char head[16];
    size_t got = 0;
    int s = connect_local(PORT_UI);
    if (s < 0) return 0;
    send_all(s, (const unsigned char *)req, length(req));
    for (;;) {   /* "HTTP/1.1 200 ..." - die ersten Zeichen merken, den Rest nur abholen */
        ssize_t r = recv(s, buf, sizeof(buf), 0);
        if (r <= 0) break;
        for (ssize_t i = 0; i < r && got < sizeof(head); i++) head[got++] = buf[i];
    }
    close(s);
    if (got < 12 || head[0] != 'H' || head[8] != ' ') return -1;
    int code = 0;
    for (int i = 9; i < 12; i++) {
        if (head[i] < '0' || head[i] > '9') return -1;
        code = code * 10 + (head[i] - '0');
    }
    return code;
}

/* 1 = Browser geöffnet (oder ein Fehler wurde schon gemeldet), 0 = Formatierer läuft nicht */
static int open_ui(void) {
    int code = open_ui_once();
    for (int i = 0; i < 10 && code == 503; i++) {   /* gerade zu viele Verbindungen: kurz warten */
        sceKernelUsleep(300 * 1000);
        code = open_ui_once();
    }
    if (code == 0) return 0;
    if (code != 200)   /* der Formatierer läuft, aber der Browser ging nicht auf - sonst passierte gar nichts */
        report("USB-Formatierer: Der Browser konnte nicht starten - Formatierer erreichbar unter http://<PS5-IP>:8090");
    return 1;
}

static void run(void) {
    if (open_ui()) return;

    int s = connect_local(PORT_LOADER);
    if (s < 0) {
        report("USB-Formatierer: ELF-Loader (Port 9021) nicht erreichbar - ist der Jailbreak geladen?");
        return;
    }
    int rc = send_all(s, payload_elf, (size_t)(payload_elf_end - payload_elf));
    close(s);
    if (rc != 0) {
        report("USB-Formatierer: Senden an den ELF-Loader fehlgeschlagen");
        return;
    }
    for (int i = 0; i < 60; i++) {  /* bis 15 s */
        sceKernelUsleep(250 * 1000);
        if (open_ui()) return;
    }
    report("USB-Formatierer: Der Formatierer antwortet nicht");
}

/* Einsprung des Titels. Beendet wird mit sceSystemServiceLoadExec("exit") (die PS5 räumt die App nach dem
 * gescheiterten ExitSpawn "Exit spawn failed : 0x80aa001a" ab): exit() löst in einem nativen Titel SIGSYS aus,
 * und das Schließen per sceSystemServiceKillApp geht nicht - schon die eigene App-ID über
 * sceSystemServiceGetAppIdOfRunningBigApp lehnt die PS5 für normale Apps ab ("!isSystemProcess ... is not
 * allowed to invoke this API"). */
__attribute__((noreturn, visibility("default"))) void _start(void *process_parameters, void (*teardown)(void)) {
    (void)process_parameters;
    (void)teardown;
    for (volatile char *p = __bss_start; p < __bss_end; p++) *p = 0;
    run();
    /* Erst den Browser fertig starten lassen. Endete der Titel im selben Augenblick, in dem der Browser
     * angefordert wurde, startete dessen Prozess (NKWebProcess) erst mitten im Abbau der App; deren Sandbox
     * /mnt/sandbox/PPSA86920_00x blieb dann manchmal stehen, ShadowMountPlus hielt den Titel für noch aktiv
     * und verweigerte bis zum Neustart jeden Spielstart ("launch mount unavailable ... Device busy").
     * Mit Wartezeit läuft der Browser-Prozess schon vorher (Kernel-Log: 0,6 s nach dem Titelstart). */
    sceKernelUsleep(3000 * 1000);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100 * 1000);
}
