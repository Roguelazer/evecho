/* 
 * A really stupid benchmarker.
 *
 * Reads stdin up to EOF (probably want to redirect something to it).
 *
 * Then writes all of that data to the target host every second.
 *
 * TODO: Experiment with using iovecs and writev(2) instead of epoll(7) and
 * write(2).
 */

#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 201203L
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
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>

#include "sendlib.h"
#include "debugs.h"

#define BUFFER_SIZE 4096

#define IOBUF_SIZE 32768

static struct timespec getaddr_start, getaddr_end, connect_start, connect_end;
static struct timespec stdin_start, stdin_end;
static struct timespec send_start, send_end;

static void print_help(void)
{
    fprintf(stderr, "Usage: timesend -H hostname -p port\n"
            "Reads data from stdin, sends to port\n"
            "\n");
}

int main(int argc, char **argv) {
    char* address = NULL;
    char* service = NULL;
    char opt;
    char buf[BUFFER_SIZE];
    ssize_t bytes_read = 0;
    ssize_t data_size = 0;
    ssize_t data_alloced = BUFFER_SIZE;
    char* data = malloc(BUFFER_SIZE);
    int sock;
    struct iovec* vecs = NULL;
    int n_iovecs = 0;
    int vecs_written = 0;
    struct timespec send_time, stdin_time, connect_time;

    int return_status = 0;
    int rc;
    
    if (data == NULL) {
        fprintf(stderr, "could not malloc %u bytes for initial data\n", BUFFER_SIZE);
        return 1;
    }

    while ((opt = getopt(argc, argv, "hc:H:p:")) != -1) {
        switch(opt) {
            case 'h':
                print_help();
                return_status = 0;
                goto end;
            case 'H':
                address = strdup(optarg);
                break;
            case 'p':
                service = strdup(optarg);
                break;
            default:
                print_help();
                return_status = 1;
                goto end;
        }
    }

    if (address == NULL || service == NULL) {
        print_help();
        return_status = 1;
        goto end;
    }

    clock_gettime(CLOCK_MONOTONIC, &stdin_start);
    /* Start by reading all of the data from stdin */
    while(true) {
        bytes_read = read(STDIN_FILENO, buf, BUFFER_SIZE);
        if (bytes_read < 0)  {
            perror("read");
            return_status = 1;
            goto end;
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

    if (data_size > IOBUF_SIZE * IOV_MAX) {
        fprintf(stderr, "data too large; got %zd bytes, max %d bytes\n", data_size, IOBUF_SIZE * IOV_MAX);
        return_status = 1;
        goto end;
    }

    timespec_subtract(&stdin_time, &stdin_end, &stdin_start);
    printf("took %lu.%09lus to read %zd bytes from stdin\n", stdin_time.tv_sec, stdin_time.tv_nsec, data_size);

    if ((sock = do_connect(address, service, &getaddr_start, &getaddr_end, &connect_start, &connect_end, false)) < 0) {
        perror("connect");
        return_status = 1;
        goto end;
    }

    timespec_subtract(&connect_time, &connect_end, &getaddr_start);
    printf("took %lu.%09lus to connect to %s:%s; now going to send %zd bytes every 1s\n", connect_time.tv_sec, connect_time.tv_nsec, address, service, data_size);

    {
        if (data_size % IOBUF_SIZE == 0) {
            n_iovecs = (data_size / IOBUF_SIZE);
        } else {
            n_iovecs = (data_size / IOBUF_SIZE) + 1;
        }
        int n_iovecs_less_1 = n_iovecs - 1;
        int i;
        vecs = malloc(sizeof(struct iovec) * n_iovecs);
        Dprintf("Going to write %d iovecs\n", n_iovecs);
        for (i=0; i < n_iovecs; ++i) {
            vecs[i].iov_base = data + (i*IOBUF_SIZE);
            if (i < n_iovecs_less_1 ) {
                vecs[i].iov_len = IOBUF_SIZE;
            } else {
                vecs[i].iov_len = data_size - (n_iovecs_less_1 * IOBUF_SIZE);
            }
            Dprintf("vecs[%d].iov_len = %zd\n", i, vecs[i].iov_len);
        }
    }

    while (1) {
        struct timespec sleep_req, sleep_rem;
        vecs_written = 0;

        clock_gettime(CLOCK_MONOTONIC, &send_start);
        while (vecs_written < n_iovecs) {
            ssize_t written;
            Dprintf("going to try and write %d vecs starting at vec %d\n", n_iovecs - vecs_written, vecs_written);
            written = writev(sock, vecs + vecs_written, n_iovecs - vecs_written);
            if (written != data_size) {
                if (written < 0) {
                    return_status = 1;
                    DJperror(end, "writev");
                } else if (written % IOBUF_SIZE != 0) {
                    Dprintf(stderr, "wrote %zd bytes, IOBUF_SIZE=%d bytes. Ick!", written, IOBUF_SIZE);
                    return_status = 1;
                    goto end;
                }
                vecs_written += (written / IOBUF_SIZE);
            } else {
                vecs_written = n_iovecs;
            }
            Dprintf("wrote %zd bytes of %zd bytes this time; wrote %d/%d vecs\n", written, data_size, vecs_written, n_iovecs);
        }
        clock_gettime(CLOCK_MONOTONIC, &send_end);
        timespec_subtract(&send_time, &send_end, &send_start);

        if ((send_time.tv_sec >= 1) && (send_time.tv_nsec != 0)) {
            fprintf(stderr, "took more than 1 second to send %zd bytes!\n", data_size);
        }

        printf("took %lu.%09lus to send %zd bytes in %d vectors of ~%d bytes; \n", send_time.tv_sec, send_time.tv_nsec, data_size, n_iovecs, IOBUF_SIZE);

        sleep_req.tv_sec = 0;
        sleep_req.tv_nsec = (1000000000 - send_time.tv_nsec);

        rc = nanosleep(&sleep_req, &sleep_rem);
        while (rc != 0) {
            if (errno == EINTR) {
                rc = nanosleep(&sleep_rem, &sleep_rem);
            } else {
                perror("nanosleep");
            }
        }
    }

    

end:
    if (address != NULL)
        free(address);
    if (service != NULL)
        free(service);
    if (data != NULL)
        free(data);
    if (vecs != NULL)
        free(vecs);
    return return_status;
}
