// Servidor Metro Autonomo
// - TCP, sockets Berkeley
// - Hilos: aceptador, 1 por cliente, telemetría
// - Telemetría cada 10s, evento de estación cada ~30s (parada 20s), inversión cada 5 estaciones

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>

#define MAX_CLIENTS   64
#define BUFFER_SIZE   2048

typedef struct {
    int sock;
    struct sockaddr_in addr;
} client_t;

static client_t* clients[MAX_CLIENTS];
static pthread_mutex_t clients_mx = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;
static FILE* LOGF = NULL;

// Estado del “metro”
static int speed = 30;        // km/h (demo)
static int battery = 100;     // %
static int station = 0;       // id de estación
static int direction = 1;     // 1 = OUTBOUND, -1 = INBOUND

// ===== Utilidades de log =====
static void log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&log_mx);
    // timestamp simple
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stdout, "[%s] ", ts);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    if (LOGF) {
        fprintf(LOGF, "[%s] ", ts);
        va_list ap2; va_start(ap2, fmt);
        vfprintf(LOGF, fmt, ap2);
        va_end(ap2);
        fprintf(LOGF, "\n");
        fflush(LOGF);
    }
    pthread_mutex_unlock(&log_mx);
    va_end(ap);
}

// ===== Lista de clientes =====
static void add_client(client_t* c) {
    pthread_mutex_lock(&clients_mx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i]) { clients[i] = c; break; }
    }
    pthread_mutex_unlock(&clients_mx);
}

static void remove_client_sock(int sock) {
    pthread_mutex_lock(&clients_mx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->sock == sock) {
            free(clients[i]);
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mx);
}

static void broadcast_line(const char* s) {
    pthread_mutex_lock(&clients_mx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i]) {
            ssize_t n = send(clients[i]->sock, s, strlen(s), MSG_NOSIGNAL);
            (void)n; // si falla, hilo de cliente lo gestionará al siguiente recv
        }
    }
    pthread_mutex_unlock(&clients_mx);
}

// ===== Parsing sencillo =====
static int starts_with(const char* buf, const char* pref) {
    return strncmp(buf, pref, strlen(pref)) == 0;
}

// ===== Hilo por cliente =====
static void* client_thread(void* arg) {
    client_t* cli = (client_t*)arg;
    char ip[64];
    inet_ntop(AF_INET, &cli->addr.sin_addr, ip, sizeof ip);
    int cport = ntohs(cli->addr.sin_port);
    log_line("Cliente conectado %s:%d", ip, cport);

    char buf[BUFFER_SIZE];
    while (1) {
        ssize_t r = recv(cli->sock, buf, sizeof(buf)-1, 0);
        if (r <= 0) {
            log_line("Cliente desconectado %s:%d (r=%zd errno=%d)", ip, cport, r, errno);
            close(cli->sock);
            remove_client_sock(cli->sock);
            return NULL;
        }
        buf[r] = '\0';

        // Normalizamos (quitamos \r\n)
        char* nl = strpbrk(buf, "\r\n");
        if (nl) *nl = '\0';
        log_line("RX %s:%d :: %s", ip, cport, buf);

        // TODO: autenticación de admin / roles (por simplicidad, cualquiera puede enviar CMD)
        if (starts_with(buf, "CMD SPEED_UP")) {
            speed += 5;
            send(cli->sock, "ACK\n", 4, MSG_NOSIGNAL);
        } else if (starts_with(buf, "CMD SLOW_DOWN")) {
            speed -= 5; if (speed < 0) speed = 0;
            send(cli->sock, "ACK\n", 4, MSG_NOSIGNAL);
        } else if (starts_with(buf, "CMD STOPNOW")) {
            speed = 0;
            send(cli->sock, "ACK\n", 4, MSG_NOSIGNAL);
        } else if (starts_with(buf, "CMD STARTNOW")) {
            if (speed == 0) speed = 30;
            send(cli->sock, "ACK\n", 4, MSG_NOSIGNAL);
        } else if (starts_with(buf, "PING")) {
            send(cli->sock, "PONG\n", 5, MSG_NOSIGNAL);
        } else {
            send(cli->sock, "NACK unknown_command\n", 22, MSG_NOSIGNAL);
        }
    }
    return NULL;
}

// ===== Hilo de telemetría =====
static void* telemetry_thread(void* arg) {
    (void)arg;
    int tick = 0;
    while (1) {
        // Cada 10s publicamos TELEMETRY
        char line[256];
        snprintf(line, sizeof line,
                 "TELEMETRY ts=%ld speed=%d battery=%d station=%d direction=%s\n",
                 (long)time(NULL), speed, battery, station,
                 direction == 1 ? "OUTBOUND" : "INBOUND");
        broadcast_line(line);
        log_line("TX :: %s", line);

        // Actualizamos “estado”
        battery -= (speed > 0 ? 1 : 0); if (battery < 10) battery = 100; // demo
        tick++;

        // Distancia acumulada entre estaciones
        static double distance_accum = 0.0;

        // Cada ciclo de 10s calculamos cuánto avanza el metro
        double hours = 10.0 / 3600.0;   // 10 segundos expresados en horas
        distance_accum += speed * hours;

        // ¿Llegó a la siguiente estación?
        if (distance_accum >= 1.0) {   // suponemos 1 km entre estaciones
            distance_accum -= 1.0;

            char ev[128];
            snprintf(ev, sizeof ev, "EVENT STATION_ARRIVAL id=%d\n", station);
            broadcast_line(ev);
            log_line("TX :: %s", ev);

            // Parada de 20s
            sleep(20);

            station++;
            if (station > 0 && station % 5 == 0) {
                direction = -direction;
                broadcast_line("EVENT TURNAROUND\n");
                log_line("TX :: EVENT TURNAROUND");
            }
        }

        sleep(10);
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <port> <LogsFile>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    LOGF = fopen(argv[2], "a");
    if (!LOGF) perror("fopen");

    signal(SIGPIPE, SIG_IGN); // evitar que un send a socket roto mate el proceso

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(srv, (struct sockaddr*)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    log_line("Servidor escuchando en puerto %d ...", port);

    // Hilo de telemetría
    pthread_t ttele;
    pthread_create(&ttele, NULL, telemetry_thread, NULL);
    pthread_detach(ttele);

    // Bucle aceptador
    while (1) {
        struct sockaddr_in caddr; socklen_t clen = sizeof caddr;
        int cli = accept(srv, (struct sockaddr*)&caddr, &clen);
        if (cli < 0) { perror("accept"); continue; }

        client_t* c = (client_t*)malloc(sizeof *c);
        c->sock = cli; c->addr = caddr;
        add_client(c);

        pthread_t th;
        pthread_create(&th, NULL, client_thread, c);
        pthread_detach(th);
    }

    return 0;
}
