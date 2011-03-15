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

struct timespec write_start, read_start, start, end, write_end, getaddr_start, getaddr_end, connect_start, connect_end, stdin_start, stdin_end;
struct timespec read_start, start, write_start, end, write_end, getaddr_start, getaddr_end, connect_start, connect_end;
struct event* remote_event;
ssize_t total_read_bytes;
bool read_started = false, write_started = false;

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
            "\n"
            "CSV Format:\n"
            "total time,lookup time,connect time,writing time,reading time,sent bytes,received bytes\n");
}

int do_connect(const char* restrict host, const char* restrict svc)
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

    clock_gettime(CLOCK_MONOTONIC, &getaddr_start);
    if ((err = getaddrinfo(host, svc, &hints, &result)) != 0) {
        fprintf(stderr, "getaddr error %s", gai_strerror(err));
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &getaddr_end);
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        clock_gettime(CLOCK_MONOTONIC, &connect_start);
        if ((sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0) {
            return -1;
        }
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
        if (!write_started) {
            write_started = true;
            clock_gettime(CLOCK_MONOTONIC, &write_start);
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
        clock_gettime(CLOCK_MONOTONIC, &write_end);
        event_del(remote_event);
        event_set(remote_event, s, EV_READ|EV_PERSIST, &on_activity, ds);
        event_add(remote_event, NULL);
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
    struct data_status* data_status = malloc(sizeof(struct data_status));

    char* csv_output = NULL;

    while ((opt = getopt(argc, argv, "hc:H:p:")) != -1) {
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
            case 'c':
                csv_output = strdup(optarg);
                break;
            default:
                print_help();
                return 1;
        }
    }
    remote_event = malloc(sizeof(struct event));

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

    data_status->data = data;
    data_status->bytes_written = 0;
    data_status->data_size = data_size;

    base = event_init();
    clock_gettime(CLOCK_MONOTONIC, &start);
    if ((sock = do_connect(address, service)) < 0) {
        perror("connect");
        return 1;
    }
    event_set(remote_event, sock, EV_WRITE|EV_READ|EV_PERSIST, &on_activity, data_status);
    event_add(remote_event, NULL);

    event_dispatch();

    if (csv_output == NULL) {
        struct timespec delta;
        timespec_subtract(&delta, &stdin_end, &stdin_start);
        printf("Reading stdin:  %lu.%09lus\n", delta.tv_sec, delta.tv_nsec);
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
    } else {
        FILE* f;
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
        timespec_subtract(&total, &end, &start);
        timespec_subtract(&lookup,&getaddr_end,&getaddr_start);
        timespec_subtract(&connect,&connect_end,&connect_start);
        timespec_subtract(&writing,&write_end,&write_start);
        timespec_subtract(&reading,&end,&read_start);
        fprintf(f, "%lu.%09lu,%lu.%09lu,%lu.%09lu,%lu.%09lu,%lu.%09lu,%zd,%zd\n", total.tv_sec, total.tv_nsec,lookup.tv_sec,lookup.tv_nsec,connect.tv_sec,connect.tv_nsec,writing.tv_sec,writing.tv_nsec,reading.tv_sec,reading.tv_nsec,data_status->bytes_written,total_read_bytes);
        fclose(f);
    }
end:
    free(address);
    free(service);
    free(remote_event);
    free(data_status);
    free(data);
    event_base_free(base);
    if (csv_output != NULL)
        free(csv_output);
    return 0;
}
/* vim: set expandtab ts=4 sw=4 sts=0: */
