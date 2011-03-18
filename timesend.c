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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <event.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>

#include "debugs.h"

#define BUFFER_SIZE 1024

struct timespec stdin_start, stdin_end;
long num_items = 1;
bool nodelay=false;

struct ts_connection {
    char* data;
    size_t data_size;
    size_t bytes_written;
    size_t bytes_read;
    size_t total_bytes_written;
    size_t total_bytes_read;
    int evt_idx;
    int sock;
    bool done;
    bool read_started;
    bool write_started;
    long num_queries;
    long read_idx;
    long write_idx;
    struct timespec* write_start;
    struct timespec* read_start;
    struct timespec* read_end;
    struct timespec* start;
    struct timespec* end;
    struct timespec* write_end;
    struct timespec* getaddr_start;
    struct timespec* getaddr_end;
    struct timespec* connect_start;
    struct timespec* connect_end;
    struct event* evt;
};

int ts_connection_init(int, char* restrict, size_t, int, struct ts_connection* restrict);
int do_connect(const char* restrict, const char* restrict, struct ts_connection* restrict);

int ts_connection_init(int queries_per_connection, char* restrict data, size_t data_size, int which, struct ts_connection* restrict ds)
{
    ds->start = malloc(sizeof(struct timespec));
    ds->end = malloc(sizeof(struct timespec));
    ds->getaddr_start = malloc(sizeof(struct timespec));
    ds->getaddr_end = malloc(sizeof(struct timespec));
    ds->connect_start = malloc(sizeof(struct timespec));
    ds->connect_end = malloc(sizeof(struct timespec));
    ds->write_start = malloc(sizeof(struct timespec) * queries_per_connection);
    ds->write_end = malloc(sizeof(struct timespec) * queries_per_connection);
    ds->read_start = malloc(sizeof(struct timespec) * queries_per_connection);
    ds->read_end = malloc(sizeof(struct timespec) * queries_per_connection);
    ds->evt = malloc(sizeof(struct event));

    memset(ds->start, 0, sizeof(struct timespec));
    memset(ds->end, 0, sizeof(struct timespec));
    memset(ds->getaddr_start, 0, sizeof(struct timespec));
    memset(ds->getaddr_end, 0, sizeof(struct timespec));
    memset(ds->connect_start, 0, sizeof(struct timespec));
    memset(ds->connect_end, 0, sizeof(struct timespec));
    memset(ds->write_start, 0, sizeof(struct timespec)*queries_per_connection);
    memset(ds->write_end, 0, sizeof(struct timespec)*queries_per_connection);
    memset(ds->write_end, 0, sizeof(struct timespec)*queries_per_connection);
    memset(ds->read_start, 0, sizeof(struct timespec)*queries_per_connection);
    memset(ds->evt, 0, sizeof(struct event));

    ds->data = data;
    ds->bytes_written = 0;
    ds->data_size = data_size;
    ds->evt_idx = which;
    ds->num_queries = queries_per_connection;
    ds->write_idx = 0;
    ds->read_idx = 0;
    ds->read_started = false;
    ds->write_started = false;
    ds->total_bytes_written = 0;
    ds->total_bytes_read = 0;
    return 0;
}

void ts_connection_free(struct ts_connection* restrict ds)
{
    free(ds->start);
    free(ds->end);
    free(ds->getaddr_start);
    free(ds->getaddr_end);
    free(ds->connect_start);
    free(ds->connect_end);
    free(ds->write_start);
    free(ds->write_end);
    free(ds->read_start);
    free(ds->read_end);
    event_del(ds->evt);
    //free(ds->evt);
}

struct ts_connection* ts_connection;

 bool all_done() {
    int i;
    for (i = 0; i < num_items; ++i) {
        if (!ts_connection[i].done) {
            return false;
        }
    }
    return true;
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

void print_help(void)
{
    fprintf(stderr, "Usage: timesend -H hostname -p port\n"
            "Reads data from stdin, sends to port\n"
            "\n"
            "Options\n"
            "  -c FILE      Write csv to file FILE instead of human-readable to stdout\n"
            "  -n NUM       Send NUM streams in parallel (default 1)\n"
            "  -N NUM       Send NUM queries per connection (default 1)\n"
            "\n"
            "CSV Format:\n"
            "total time,lookup time,connect time,writing time,reading time,sent bytes,received bytes\n");
}

int do_connect(const char* restrict host, const char* restrict svc, struct ts_connection* restrict ds)
{
    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* rp;
    int sfd, err;
    long current;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    clock_gettime(CLOCK_MONOTONIC, ds->getaddr_start);
    if ((err = getaddrinfo(host, svc, &hints, &result)) != 0) {
        fprintf(stderr, "getaddr error %s", gai_strerror(err));
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, ds->getaddr_end);
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        int one=1;
        clock_gettime(CLOCK_MONOTONIC, ds->connect_start);
        if ((sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0) {
            return -1;
        }
        if (nodelay) {
            setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        }
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            clock_gettime(CLOCK_MONOTONIC, ds->connect_end);
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

void on_activity(int s, short ev_type, void* data)
{
    struct ts_connection* ds = (struct ts_connection*)data;
    if (ev_type & EV_READ) {
        int read_idx = ds->read_idx;
        char buf[BUFFER_SIZE];
        ssize_t bytes_read;
        while (true) {
            bytes_read = read(s, buf, BUFFER_SIZE);
            if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
            }
            else if (bytes_read == 0 && ds->bytes_read < ds->data_size) {
                fprintf(stderr, "Read %zd bytes, expected to read %zd\n", ds->bytes_read, ds->data_size);
                break;
            }
            ds->total_bytes_read += bytes_read;
            ds->bytes_read += bytes_read;
            if (ds->bytes_read >= ds->data_size) {
                clock_gettime(CLOCK_MONOTONIC, &(ds->read_end[read_idx]));
                ds->bytes_read = 0;
                break;
            }
        }
        Dprintf("read_idx %d done\n", ds->read_idx);
        ds->read_started = false;
        ds->read_idx++;
        if (read_idx >= ds->num_queries - 1) {
            ds->done = true;
            event_del(ds->evt);
            clock_gettime(CLOCK_MONOTONIC, ds->end);
            if (all_done()) {
                event_loopbreak();
            }
        } else {
            event_del(ds->evt);
            event_set(ds->evt, s, EV_WRITE|EV_PERSIST, &on_activity, ds);
            event_add(ds->evt, NULL);
        }
    }
    else if (ev_type & EV_WRITE) {
        ssize_t bytes_written_here;
        int write_idx = ds->write_idx;
        if (!ds->write_started) {
            Dprintf("write_idx %d started\n", write_idx);
            ds->write_started = true;
            clock_gettime(CLOCK_MONOTONIC, &(ds->write_start[write_idx]));
        }
        while (ds->bytes_written < ds->data_size) {
            bytes_written_here = write(s, (ds->data + ds->bytes_written), (ds->data_size - ds->bytes_written));
            if (bytes_written_here < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!ds->read_started) {
                        ds->read_started = true;
                        Dprintf("read_idx %d started\n", ds->read_idx);
                        clock_gettime(CLOCK_MONOTONIC, &(ds->read_start[write_idx]));
                    }
                    event_del(ds->evt);
                    event_set(ds->evt, s, EV_READ|EV_WRITE|EV_PERSIST, &on_activity, ds);
                    event_add(ds->evt, NULL);
                    return;
                }
                perror("write");
                exit(1);
            }
            ds->bytes_written += bytes_written_here;
            ds->total_bytes_written += bytes_written_here;
        }
        Dprintf("write_idx %d done\n", ds->write_idx);
        ds->bytes_written = 0;
        clock_gettime(CLOCK_MONOTONIC, &(ds->write_end[write_idx]));
        if (!ds->read_started) {
            ds->read_started = true;
            Dprintf("read_idx %d started\n", ds->read_idx);
            clock_gettime(CLOCK_MONOTONIC, &(ds->read_start[write_idx]));
        }
        ds->write_idx++;
        ds->write_started = false;
        event_del(ds->evt);
        event_set(ds->evt, s, EV_READ|EV_PERSIST, &on_activity, ds);
        event_add(ds->evt, NULL);
    }
}

int create_csv(char* csv)
{
    int err;
    FILE* f;
    if ((err = creat(csv, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)) < 0)
        return -1;
    if ((f = fopen(csv, "a")) == NULL)
        return -1;
    fprintf(f, "# total, lookup, connect, writing, reading, bytes_sent, bytes_received\n");
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    char* address = NULL;
    char* service = NULL;
    char* data = malloc(BUFFER_SIZE);
    size_t data_size = 0;
    size_t data_alloced = BUFFER_SIZE;
    struct event_base* base;
    char buf[BUFFER_SIZE];
    ssize_t bytes_read;
    char opt;
    int i;
    long num_per_connection = 1;

    char* csv_output = NULL;

    while ((opt = getopt(argc, argv, "hc:H:p:n:N:D")) != -1) {
        switch(opt) {
            case 'h':
                print_help();
                return 0;
            case 'H':
                address = strdup(optarg);
                break;
            case 'p':
                service = strdup(optarg);
                break;
            case 'n':
                num_items = atol(optarg);
                if (num_items < 1) {
                    fprintf(stderr, "-n must be >=1\n");
                    return 1;
                }
                break;
            case 'D':
                nodelay = true;
                break;
            case 'N':
                num_per_connection = atol(optarg);
                if (num_per_connection < 1) {
                    fprintf(stderr, "-N must be >= 1\n");
                    return 1;
                } else if (num_per_connection > 99999) {
                    fprintf(stderr, "-N must be <= 99999\n");
                }
                break;
            case 'c':
                csv_output = strdup(optarg);
                break;
            default:
                print_help();
                return 1;
        }
    }
    ts_connection = malloc(sizeof(struct ts_connection) * num_items);

    if (address == NULL || service == NULL) {
        print_help();
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &stdin_start);
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
    clock_gettime(CLOCK_MONOTONIC, &stdin_end);

    base = event_init();
    
    for (i=0; i<num_items; ++i) {
        struct ts_connection* ds = &(ts_connection[i]);
        if (ts_connection_init(num_per_connection, data, data_size, i, ds) < 0) {
            perror("connection_init");
            return 1;
        }
        clock_gettime(CLOCK_MONOTONIC, ds->start);
        if ((ds->sock = do_connect(address, service, &ts_connection[i])) < 0) {
            perror("connect");
            return 1;
        }
        event_set(ds->evt, ds->sock, EV_WRITE|EV_READ|EV_PERSIST, &on_activity, ds);
        event_add(ds->evt, NULL);
    }

    event_dispatch();

    if (csv_output == NULL) {
        struct timespec delta;
        int i;
        int j;
        timespec_subtract(&delta, &stdin_end, &stdin_start);
        printf("Reading stdin:  %lu.%09lus\n\n", delta.tv_sec, delta.tv_nsec);
        for (i = 0; i < num_items; ++i) {
            struct ts_connection* ds = &(ts_connection[i]);
            timespec_subtract(&delta, ds->end, ds->start);
            printf("Total time:     %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            timespec_subtract(&delta, ds->getaddr_end, ds->getaddr_start);
            printf("  Host lookup:  %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            timespec_subtract(&delta, ds->connect_end, ds->connect_start);
            printf("  Connect:      %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            for (j = 0; j < num_per_connection; ++j) {
                timespec_subtract(&delta, &(ds->write_end[j]), &(ds->write_start[j]));
                printf("  (query % 5i) Writing:      %lu.%09lus\n", j, delta.tv_sec, delta.tv_nsec);
                timespec_subtract(&delta, &(ds->read_end[j]), &(ds->read_start[j]));
                printf("  (query % 5i) Reading:      %lu.%09lus\n", j, delta.tv_sec, delta.tv_nsec);
            }
            printf("  Bytes written: %zu\n", ds->total_bytes_written);
            printf("  Bytes read:    %zu\n", ds->total_bytes_read);
            printf("\n");
        }
    } else {
        FILE* f;
        int i, j;
        struct timespec total,lookup,connect,writing,reading;
        if (access(csv_output, F_OK) != 0) {
            if (create_csv(csv_output) < 0) {
                fprintf(stderr, "failure creating %s\n", csv_output);
                goto end;
            }
        }
        if ((f = fopen(csv_output, "a")) == NULL) {
            fprintf(stderr, "failure opening %s\n", csv_output);
            goto end;
        }
        for (i = 0; i < num_items; ++i) {
            struct ts_connection* ds = &(ts_connection[i]);
            for (j = 0; j < num_per_connection; ++j) {
                timespec_subtract(&total,ds->end,ds->start);
                timespec_subtract(&lookup,ds->getaddr_end,ds->getaddr_start);
                timespec_subtract(&connect,ds->connect_end,ds->connect_start);
                timespec_subtract(&writing,&(ds->write_end[j]),&(ds->write_start[j]));
                timespec_subtract(&reading,&(ds->read_end[j]),&(ds->read_start[j]));
                fprintf(f, "%lu.%09lu,%lu.%09lu,%lu.%09lu,%lu.%09lu,%lu.%09lu,%zd,%zd\n", total.tv_sec, total.tv_nsec,lookup.tv_sec,lookup.tv_nsec,connect.tv_sec,connect.tv_nsec,writing.tv_sec,writing.tv_nsec,reading.tv_sec,reading.tv_nsec,ds->total_bytes_written,ds->total_bytes_read);
            }
        }
        fclose(f);
    }
    for (i=0;i<num_items;++i) {
        Dprintf("Freeing %d\n", i);
        ts_connection_free(&(ts_connection[i]));
    }
end:
    free(address);
    free(service);
    free(ts_connection);
    free(data);
    event_base_free(base);
    if (csv_output != NULL)
        free(csv_output);
    return 0;
}
/* vim: set expandtab ts=4 sw=4 sts=0: */
