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
#include <getopt.h>

#define BUFFER_SIZE 1024

struct timespec stdin_start, stdin_end;
struct event* remote_events;
ssize_t total_read_bytes;
long num_items = 1;

struct data_status {
    char* data;
    size_t data_size;
    size_t bytes_written;
    int evt_idx;
    bool read_started;
    bool done;
    bool write_started;
    struct timespec write_start;
    struct timespec read_start;
    struct timespec start;
    struct timespec end;
    struct timespec write_end;
    struct timespec getaddr_start;
    struct timespec getaddr_end;
    struct timespec connect_start;
    struct timespec connect_end;
};
struct data_status* data_status;

bool all_done() {
    int i;
    for (i = 0; i < num_items; ++i) {
        if (!data_status[i].done) {
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
            "  -n NUM       Send NUM streams in parallel\n"
            "\n"
            "CSV Format:\n"
            "total time,lookup time,connect time,writing time,reading time,sent bytes,received bytes\n");
}

int do_connect(const char* restrict host, const char* restrict svc, struct data_status* restrict ds)
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

    clock_gettime(CLOCK_MONOTONIC, &ds->getaddr_start);
    if ((err = getaddrinfo(host, svc, &hints, &result)) != 0) {
        fprintf(stderr, "getaddr error %s", gai_strerror(err));
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &ds->getaddr_end);
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        clock_gettime(CLOCK_MONOTONIC, &ds->connect_start);
        if ((sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0) {
            return -1;
        }
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            clock_gettime(CLOCK_MONOTONIC, &ds->connect_end);
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
    struct data_status* ds = (struct data_status*)data;
    int i = ds->evt_idx;
    if (ev_type & EV_READ) {
        if (!ds->read_started) {
            ds->read_started = true;
            clock_gettime(CLOCK_MONOTONIC, &ds->read_start);
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
        clock_gettime(CLOCK_MONOTONIC, &ds->end);
        ds->done = true;
        if (all_done()) {
            event_loopbreak();
        }
    }
    else if (ev_type & EV_WRITE) {
        ssize_t bytes_written_here;
        if (!ds->write_started) {
            ds->write_started = true;
            clock_gettime(CLOCK_MONOTONIC, &ds->write_start);
        }
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
        clock_gettime(CLOCK_MONOTONIC, &ds->write_end);
        event_del(&remote_events[i]);
        event_set(&remote_events[i], s, EV_READ|EV_PERSIST, &on_activity, ds);
        event_add(&remote_events[i], NULL);
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
    int sock;
    ssize_t bytes_read;
    char opt;
    int i;

    char* csv_output = NULL;

    while ((opt = getopt(argc, argv, "hc:H:p:n:")) != -1) {
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
            case 'c':
                csv_output = strdup(optarg);
                break;
            default:
                print_help();
                return 1;
        }
    }
    remote_events = malloc(sizeof(struct event) * num_items);
    data_status = malloc(sizeof(struct data_status) * num_items);

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

    for (i=0; i < num_items; ++i) {
        memset(&data_status[i], 0, sizeof(struct data_status));
        clock_gettime(CLOCK_MONOTONIC, &(&data_status[i])->start);
        if ((sock = do_connect(address, service, &data_status[i])) < 0) {
            perror("connect");
            return 1;
        }
        (&data_status[i])->data = data;
        (&data_status[i])->bytes_written = 0;
        (&data_status[i])->data_size = data_size;
        (&data_status[i])->evt_idx = i;
        memset((&remote_events[i]), 0, sizeof(struct event));
        event_set((&remote_events[i]), sock, EV_WRITE|EV_READ|EV_PERSIST, &on_activity, (&data_status[i]));
        event_add((&remote_events[i]), NULL);
    }

    event_dispatch();

    if (csv_output == NULL) {
        struct timespec delta;
        int i;
        timespec_subtract(&delta, &stdin_end, &stdin_start);
        printf("Reading stdin:  %lu.%09lus\n\n", delta.tv_sec, delta.tv_nsec);
        for (i = 0; i < num_items; ++i) {
            struct data_status* ds = &(data_status[i]);
            timespec_subtract(&delta, &ds->end, &ds->start);
            printf("Total time:     %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            timespec_subtract(&delta, &ds->getaddr_end, &ds->getaddr_start);
            printf("  Host lookup:  %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            timespec_subtract(&delta, &ds->connect_end, &ds->connect_start);
            printf("  Connect:      %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            timespec_subtract(&delta, &ds->write_end, &ds->start);
            printf("  Writing:      %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            timespec_subtract(&delta, &ds->end, &ds->read_start);
            printf("  Reading:      %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
            printf("Sent %zd bytes, recieved %zd bytes\n", ds->bytes_written, total_read_bytes);
            printf("\n");
        }
    } else {
        FILE* f;
        int i;
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
            struct data_status* ds = &(data_status[i]);
            timespec_subtract(&total,&ds->end,&ds->start);
            timespec_subtract(&lookup,&ds->getaddr_end,&ds->getaddr_start);
            timespec_subtract(&connect,&ds->connect_end,&ds->connect_start);
            timespec_subtract(&writing,&ds->write_end,&ds->write_start);
            timespec_subtract(&reading,&ds->end,&ds->read_start);
            fprintf(f, "%lu.%09lu,%lu.%09lu,%lu.%09lu,%lu.%09lu,%lu.%09lu,%zd,%zd\n", total.tv_sec, total.tv_nsec,lookup.tv_sec,lookup.tv_nsec,connect.tv_sec,connect.tv_nsec,writing.tv_sec,writing.tv_nsec,reading.tv_sec,reading.tv_nsec,data_status->bytes_written,total_read_bytes);
        }
        fclose(f);
    }
end:
    free(address);
    free(service);
    free(data_status);
    free(data);
    event_base_free(base);
    free(remote_events);
    if (csv_output != NULL)
        free(csv_output);
    return 0;
}
/* vim: set expandtab ts=4 sw=4 sts=0: */
