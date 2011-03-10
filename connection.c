#define _XOPEN_SOURCE 600
/* For some reason, event.h doesn't work with C99 unless _GNU_SOURCE is
 * defined (no u_char in _POSIX_SOURCE?) */
#define _GNU_SOURCE

#include <sys/types.h>
#include <event.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#include "connection.h"
#include "debugs.h"

void connection_write(struct bufferevent* e, void* data)
{
    struct connection* c = (struct connection*)data;
    bufferevent_setcb(e, &connection_read, NULL, e->errorcb, c);
    bufferevent_disable(e, EV_WRITE);
}

void connection_error(struct bufferevent* e, short error, void* data)
{
    (void)e;
    struct connection* c = (struct connection*)data;
#if DEBUG
    if (error & EVBUFFER_READ)
        printf("READ error\n");
    if (error & EVBUFFER_WRITE)
        printf("WRITE error\n");
    if (error & EVBUFFER_EOF)
        printf("EOF recieved\n");
    if (error & EVBUFFER_ERROR)
        printf("Unknown error\n");
    if (error & EVBUFFER_TIMEOUT)
        printf("Timeout\n");
#else
    (void) error;
#endif
    c->c_dcb(c);
}

void connection_read(struct bufferevent* e, void* data)
{
    struct connection* c = (struct connection*)data;
    char buf[READ_BUF_SIZE];
    int amt_read;
    if ((amt_read = bufferevent_read(e, buf, READ_BUF_SIZE)) < 0) {
        perror("bufferevent_read");
        return;
    }
    if (bufferevent_write(e, buf, amt_read)) {
        perror("bufferevent_write");
    }
    bufferevent_enable(e, EV_READ|EV_WRITE);
    bufferevent_setcb(e, &connection_read, &connection_write, e->errorcb, c);
}

struct connection* connection_init(int fd, disconnectcb dcb)
{
    struct connection* c;
    
    if ((c = malloc(sizeof(struct connection))) == NULL)
        return NULL;
    c->c_be = NULL;
    c->c_fd = fd;
    c->c_be = bufferevent_new(c->c_fd, &connection_read, NULL, connection_error, c);
    c->c_dcb = dcb;
    bufferevent_enable(c->c_be, EV_READ);
    return c;
}

void connection_free(struct connection* c)
{
    if (c->c_be != NULL)
        bufferevent_free(c->c_be);
    if (c->c_fd >= 0)
        close(c->c_fd);
    if (c != NULL)
        free(c);
}

/* vim: set expandtab ts=4 sw=4 sts=0: */
