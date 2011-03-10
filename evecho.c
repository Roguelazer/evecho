#include <event-config.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <evutil.h>
#include <event.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>

struct connection
{
    struct bufferevent* c_be;
    int c_fd;
};

void print_help(void)
{
    fprintf(stderr,
            "evecho [opts]\n"
            "\n"
            "Options:\n"
            "    -p port        Listen on port <port>\n"
            "    -b address     Bind to address <address>\n"
            "    -h             Show this help\n");
}

int setup_listener(const char* address, const char* svc)
{
    int fd, err;
    struct addrinfo *ai, *rp;
    struct addrinfo hints;
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
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
    }
    if (rp == NULL) {
        fprintf(stderr, "Could not bind to %s:%s\n", address, svc);
        return -1;
    } else {
        struct sockaddr* addr = rp->ai_addr;
        size_t addrlen = rp->ai_addrlen;
        char host[48];
        char srv[6];
        if (getnameinfo(rp->ai_addr, rp->ai_addrlen, host, 48, srv, 6, NI_NUMERICHOST|NI_NUMERICSERV) < 0) {
            perror("getnameinfo");
            close(fd);
            return -1;
        }
        fprintf(stderr, "Bound on %s:%s\n", host, srv);
    }
    freeaddrinfo(ai);
    return fd;
}

void on_connect(int fd, short evtype, void* data)
{
    return;
}

int main(int argc, char **argv)
{
    int opt;
    int listen_socket;
    struct event listen_event;
    char* address = "0.0.0.0";
    char* service = "0";
	while ((opt = getopt(argc, argv, "hb:p:")) != -1) {
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
			default:
				print_help();
				return 1;
        }
    }
    event_init();
    if ((listen_socket = setup_listener(address, service)) < 0)
        return 1;
    event_set(&listen_event, listen_socket, EV_READ|EV_WRITE, &on_connect, NULL);
    event_add(&listen_event, NULL);
    if (event_dispatch() < 0) {
        perror("event_dispatch");
        return 1;
    }
    return 0;
}
