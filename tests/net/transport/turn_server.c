// M5.3b transport tests: p2net_turn, a tiny TURN server for local tests (libjuice's juice_server,
// static credentials). Binds 127.0.0.1 unless told otherwise; runs until SIGINT/SIGTERM.
//   p2net_turn --bind 127.0.0.1 --port 47052 --user u --pass p [--relay-ports 47070-47079]
#define _POSIX_C_SOURCE 200809L
#include <juice/juice.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char** argv) {
    const char* bind = "127.0.0.1";
    const char* user = "u";
    const char* pass = "p";
    int port = 3478, relayBegin = 0, relayEnd = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--bind")) bind = argv[i + 1];
        else if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--user")) user = argv[i + 1];
        else if (!strcmp(argv[i], "--pass")) pass = argv[i + 1];
        else if (!strcmp(argv[i], "--relay-ports")) {
            if (sscanf(argv[i + 1], "%d-%d", &relayBegin, &relayEnd) != 2) relayBegin = relayEnd = 0;
        } else {
            fprintf(stderr, "usage: p2net_turn --bind IP --port P --user U --pass P [--relay-ports A-B]\n");
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "p2net_turn: bad port\n");
        return 2;
    }
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);
    juice_server_credentials_t cred = {user, pass, 16};
    juice_server_config_t sc;
    memset(&sc, 0, sizeof sc);
    sc.credentials = &cred;
    sc.credentials_count = 1;
    sc.max_allocations = 64;
    sc.max_peers = 64;
    sc.bind_address = bind;
    sc.external_address = bind;
    sc.port = (uint16_t)port;
    sc.relay_port_range_begin = (uint16_t)relayBegin;
    sc.relay_port_range_end = (uint16_t)relayEnd;
    sc.realm = "mppm-test";
    juice_server_t* server = juice_server_create(&sc);
    if (!server) {
        fprintf(stderr, "p2net_turn: cannot start on %s:%d\n", bind, port);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "p2net_turn: listening %s:%u user=%s\n", bind, juice_server_get_port(server), user);
    while (!g_stop) {
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }
    juice_server_destroy(server);
    fprintf(stderr, "p2net_turn: stopped\n");
    return 0;
}
