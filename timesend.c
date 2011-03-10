/* 
 * A really stupid benchmarker.
 *
 * Reads stdin up to EOF (probably want to redirect something to it).
 *
 * Then writes all of that to the given host and port and waits to recieve
 * it all back.
 */

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <event.h>
#include <fcntl.h>
#include <errno.h>

#define BUFFER_SIZE 1024

struct timespec read_start, start, end, write_end, getaddr_start, getaddr_end, connect_start, connect_end, delta;
struct event* remote_event;
ssize_t total_read_bytes;
bool read_started = false;

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

void print_help(void)
{
    fprintf(stderr, "Usage: timesend hostname port\n"
            "Reads data from stdin, sends to port\n");
}

int do_connect(const char* restrict host, const char* restrict svc)
{
    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* rp;
    int sfd;
    long current;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    clock_gettime(CLOCK_MONOTONIC, &getaddr_start);
    if (getaddrinfo(host, svc, &hints, &result) != 0)
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &getaddr_end);
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        clock_gettime(CLOCK_MONOTONIC, &connect_start);
        if ((sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0)
            return -1;
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            clock_gettime(CLOCK_MONOTONIC, &connect_end);
            break;
        }
        close(sfd);
    }
    if (rp == NULL) {
        fprintf(stderr, "No address succeeded\n");
        return -1;
    }
    freeaddrinfo(result);
    if ((current = fcntl(sfd, F_GETFL)) < 0) {
        close(sfd);
        return -1;
    }
    if (fcntl(sfd, F_SETFL, current|O_NONBLOCK) < 0) {
        close(sfd);
        return -1;
    }
    return sfd;
}

struct data_status {
    char* data;
    size_t data_size;
    size_t bytes_written;
};

void on_activity(int s, short ev_type, void* data)
{
    struct data_status* ds = (struct data_status*)data;
    if (ev_type & EV_READ) {
        if (!read_started) {
            read_started = true;
            clock_gettime(CLOCK_MONOTONIC, &read_start);
        }
        char buf[BUFFER_SIZE];
        ssize_t bytes_read;
        while (true) {
            bytes_read = read(s, buf, BUFFER_SIZE);
            if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                perror("read");
                exit(1);
            }
            else if (bytes_read == 0) {
                break;
            }
            total_read_bytes += bytes_read;
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        event_loopbreak();
    }
    else if (ev_type & EV_WRITE) {
        ssize_t bytes_written_here;
        while (ds->bytes_written < ds->data_size) {
            bytes_written_here = write(s, (ds->data + ds->bytes_written), (ds->data_size - ds->bytes_written));
            if (bytes_written_here < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                perror("write");
                exit(1);
            }
            ds->bytes_written += bytes_written_here;
        }
        shutdown(s, SHUT_WR);
        clock_gettime(CLOCK_MONOTONIC, &write_end);
        event_del(remote_event);
        event_set(remote_event, s, EV_READ|EV_PERSIST, &on_activity, ds);
        event_add(remote_event, NULL);
    }
}


int main(int argc, char** argv) {
    char* address;
    char* service;
    char* data = malloc(BUFFER_SIZE);
    size_t data_size = 0;
    size_t data_alloced = BUFFER_SIZE;
    struct event_base* base;
    char buf[BUFFER_SIZE];
    int sock;
    ssize_t bytes_read;
    struct data_status* data_status = malloc(sizeof(struct data_status));

    remote_event = malloc(sizeof(struct event));

    if (argc != 3) {
        print_help();
        exit(2);
    }
    address = strdup(argv[1]);
    service = strdup(argv[2]);


    /* Start by reading all of the data from stdin */
    while(true) {
        bytes_read = read(STDIN_FILENO, buf, BUFFER_SIZE);
        if (bytes_read < 0)  {
            perror("read");
            return 1;
        }
        else if (bytes_read == 0) {
            break;
        }
        else {
            if (data_size + bytes_read > data_alloced) {
                data_alloced *= 2;
                data = realloc(data, data_alloced);
            }
            memcpy(data + data_size, buf, bytes_read);
            data_size += bytes_read;
        }
    }

    data_status->data = data;
    data_status->bytes_written = 0;
    data_status->data_size = data_size;

    base = event_init();
    if ((sock = do_connect(address, service)) < 0) {
        perror("connect");
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    event_set(remote_event, sock, EV_WRITE|EV_READ|EV_PERSIST, &on_activity, data_status);
    event_add(remote_event, NULL);

    event_dispatch();

    timespec_subtract(&delta, &end, &start);
    printf("Total time:     %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
    timespec_subtract(&delta, &getaddr_end, &getaddr_start);
    printf("  Host lookup:  %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
    timespec_subtract(&delta, &connect_end, &connect_start);
    printf("  Connect:      %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
    timespec_subtract(&delta, &write_end, &start);
    printf("  Writing:      %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
    timespec_subtract(&delta, &end, &read_start);
    printf("  Reading:      %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
    printf("Sent %zd bytes, recieved %zd bytes\n", data_status->bytes_written, total_read_bytes);

    free(address);
    free(service);
    free(remote_event);
    free(data_status);
    free(data);
    event_base_free(base);
    return 0;
}
/* vim: set expandtab ts=4 sw=4 sts=0: */