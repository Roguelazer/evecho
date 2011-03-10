#define _XOPEN_SOURCE 600
/* For some reason, event.h doesn't work with C99 unless _GNU_SOURCE is
 * defined (no u_char in _POSIX_SOURCE?) */
#define _GNU_SOURCE

#include <sys/types.h>
#include <event-config.h>
#include <event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <evutil.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include "debugs.h"
#include "connection.h"

#define MAX_CONNS 128

#if DEBUG
static bool verbose = true;
#else
static bool verbose = false;
#endif

void print_help(void)
{
    fprintf(stderr,
            "evecho [opts]\n"
            "\n"
            "Options:\n"
            "    -p port        Listen on port <port>\n"
            "    -b address     Bind to address <address>\n"
            "    -v             Be more verbose\n"
            "    -h             Show this help\n");
}

static void timespec_subtract(struct timespec* restrict res, struct timespec* lhs, struct timespec* rhs)
{
    res->tv_sec = lhs->tv_sec - rhs->tv_sec;
    if (rhs->tv_nsec > lhs->tv_nsec) {
        res->tv_sec-=1;
        res->tv_nsec = rhs->tv_nsec - lhs->tv_nsec;
    }
    else {
        res->tv_nsec = lhs->tv_nsec - rhs->tv_nsec;
    }
}

int make_nonblocking(int fd)
{
    long current;
    if ((current = fcntl(fd, F_GETFL)) < 0)
        return -1;
    if (fcntl(fd, F_SETFL, current|O_NONBLOCK) < 0)
        return -1;
    return 0;
}

int setup_listener(const char* restrict address, const char* restrict svc)
{
    int fd, err;
    struct addrinfo *ai, *rp;
    struct addrinfo hints;
    int reuseaddr = 1;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if ((err = getaddrinfo(address, svc, NULL, &ai)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }
    for (rp = ai; rp != NULL; rp=rp->ai_next) {
        if ((fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0)
            continue;
        if (make_nonblocking(fd)) {
            perror("make_nonblocking");
            close(fd);
            return -1;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
    }
    if (rp == NULL) {
        fprintf(stderr, "Could not bind to %s:%s\n", address, svc);
        return -1;
#if DEBUG
    } else {
#else
    } else if ((strlen(svc) == 1) && (svc[0] == '0')) {
#endif
        struct sockaddr sin;
        socklen_t sin_len = sizeof(struct sockaddr);
        char host[48];
        char srv[6];
        if (getsockname(fd, &sin, &sin_len) < 0) {
            perror("getsockname");
            close(fd);
            return -1;
        }
        if (getnameinfo(&sin, sin_len, host, 48, srv, 6, NI_NUMERICHOST|NI_NUMERICSERV) < 0) {
            perror("getnameinfo");
            close(fd);
            return -1;
        }
        fprintf(stderr, "Bound on %s:%s\n", host, srv);
    }
    if (listen(fd, MAX_CONNS) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    freeaddrinfo(ai);
    return fd;
}

void on_disconnect(struct connection* restrict c)
{
    bufferevent_disable(c->c_be, EV_READ|EV_WRITE);
    if (verbose) {
        struct timespec delta;
        timespec_subtract(&delta, &c->c_end_time, &c->c_start_time);
        printf("Cleaning up connection to %s:%s\n", c->c_host, c->c_srv);
        printf("\tConnection sent %zd bytes\n", c->c_bytes_read);
        printf("\tConnection active %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
    }
    connection_free(c);
}

void on_connect(int fd, short evtype, void* data)
{
    (void) data;
    (void) evtype;
    int rfd;
    struct sockaddr sin;
    socklen_t len = sizeof(struct sockaddr);
    struct connection* c;

    if ((rfd = accept(fd, &sin, &len)) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        else {
            Dperror("accept");
            return;
        }
    }
    if (make_nonblocking(rfd) != 0) {
        Dperror("make_nonblocking");
        close(rfd);
        return;
    }
    if ((c = connection_init(rfd, &on_disconnect)) == NULL) {
        Dperror("connection_init");
        close(rfd);
        return;
    }
    if (verbose)
        printf("Connected to %s:%s on fd %d\n", c->c_host, c->c_srv, rfd);
    return;
}

int main(int argc, char **argv)
{
    int opt;
    int listen_socket;
    struct event *listen_event = malloc(sizeof(struct event));
    struct event_base* base;
    char* address = "0.0.0.0";
    char* service = "0";
    while ((opt = getopt(argc, argv, "hb:p:v")) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                return 0;
            case 'p':
                service = strdup(optarg);
                break;
            case 'b':
                address = strdup(optarg);
                break;
            case 'v':
                verbose = true;
                break;
            default:
                print_help();
                return 1;
        }
    }
    connection_init_globals(verbose);
    signal(SIGPIPE, SIG_IGN);
    base = event_init();
    if ((listen_socket = setup_listener(address, service)) < 0)
        return 1;
    event_set(listen_event, listen_socket, EV_READ|EV_PERSIST, &on_connect, NULL);
    if (event_add(listen_event, NULL) != 0) {
        perror("event_add");
        return 1;
    }
    if (event_base_dispatch(base) < 0) {
        perror("event_dispatch");
        return 1;
    }
    Dprintf("finished event_base_dispatch\n");
    free(listen_event);
    event_base_free(base);
    return 0;
}

/* vim: set expandtab ts=4 sw=4 sts=0: */
