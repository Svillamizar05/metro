// Servidor Metro Autonomo
// - TCP, sockets Berkeley
// - Hilos: aceptador, 1 por cliente, telemetría
// - Telemetría cada 10s, evento de estación cada ~30s (parada 20s), inversión cada 5 estaciones

#include <sys/time.h>  
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

static long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec*1000LL + tv.tv_usec/1000;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms/1000, (ms%1000)*1000000L };
    nanosleep(&ts, NULL);
}


// ===== Hilo de telemetría =====
static void* telemetry_thread(void* arg) {
    (void)arg;

    // Estado auxiliar para el tiempo y la distancia
    long long last_ms          = now_ms();
    long long next_telemetry_ms= last_ms + 10000;     // cada 10 s
    long long stop_until_ms    = 0;                   // fin de parada si >0
    double     distance_accum  = 0.0;                 // km; 1.0 km == 1 estación

    for (;;) {
        long long t = now_ms();
        long long dt_ms = t - last_ms;
        if (dt_ms < 0) dt_ms = 0;  // por si cambia el reloj
        last_ms = t;

        // 1) Integración de distancia SOLO si no estamos en parada
        if (stop_until_ms == 0) {
            double dt_h = (double)dt_ms / 3600000.0;   // ms -> horas
            distance_accum += speed * dt_h;            // km/h * h = km
        }

        // 2) Llegada a estación cuando acumulamos 1.0 km
        if (stop_until_ms == 0 && distance_accum >= 1.0) {
            distance_accum -= 1.0;

            char ev[128];
            snprintf(ev, sizeof ev, "EVENT STATION_ARRIVAL id=%d\n", station);
            broadcast_line(ev);
            log_line("TX :: %s", ev);

            // programar parada de 20 s SIN bloquear
            stop_until_ms = t + 20000;
        }

        // 3) Final de parada (20 s)
        if (stop_until_ms > 0 && t >= stop_until_ms) {
            stop_until_ms = 0;

            // avanzar estación y posible cambio de sentido
            station++;
            if (station > 0 && station % 5 == 0) {
                direction = -direction;
                broadcast_line("EVENT TURNAROUND\n");
                log_line("TX :: EVENT TURNAROUND");
            }
        }

        // 4) Telemetría cada 10 s (independiente del movimiento)
        if (t >= next_telemetry_ms) {
            next_telemetry_ms += 10000; // siguiente slot de 10 s

            // batería “demo”: solo drena si NOS MOVEMOS (no en parada)
            if (stop_until_ms == 0 && speed > 0) {
                battery -= 1;
                if (battery < 10) battery = 100; // recarga demo para no "morir"
            }

            char line[256];
            snprintf(line, sizeof line,
                     "TELEMETRY ts=%ld speed=%d battery=%d station=%d direction=%s\n",
                     (long)time(NULL), speed, battery, station,
                     direction == 1 ? "OUTBOUND" : "INBOUND");
            broadcast_line(line);
            log_line("TX :: %s", line);
        }

        // 5) Pequeña siesta para no quemar CPU (50 ms)
        sleep_ms(50);
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
