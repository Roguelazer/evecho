#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include "sendlib.h"

void timespec_subtract(struct timespec* restrict res, struct timespec* lhs, struct timespec* rhs)
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

int do_connect(const char* restrict host, const char* restrict svc, struct timespec* getaddr_start, struct timespec* getaddr_end, struct timespec* connect_start, struct timespec* connect_end, bool set_nonblock)
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

    clock_gettime(CLOCK_MONOTONIC, getaddr_start);
    if ((err = getaddrinfo(host, svc, &hints, &result)) != 0) {
        fprintf(stderr, "getaddr error %s", gai_strerror(err));
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, getaddr_end);
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        clock_gettime(CLOCK_MONOTONIC, connect_start);
        if ((sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0) {
            return -1;
        }
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            clock_gettime(CLOCK_MONOTONIC, connect_end);
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
    if (set_nonblock) {
	    if (fcntl(sfd, F_SETFL, current|O_NONBLOCK) < 0) {
		close(sfd);
		return -1;
	    }
    }
    return sfd;
}
