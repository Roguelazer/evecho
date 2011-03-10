#define _XOPEN_SOURCE 600
/* For some reason, event.h doesn't work with C99 unless _GNU_SOURCE is
 * defined (no u_char in _POSIX_SOURCE?) */
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <event.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <netdb.h>

#include "connection.h"
#include "debugs.h"

void connection_write(struct bufferevent* e, void* data)
{
#if DEBUG
    struct connection* c = (struct connection*)data;
    Dprintf("Write on fd %d\n", c->c_fd);
#else
    (void) data;
#endif
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
    Dprintf("0x%zd bytes left in output buffer\n", c->c_be->output->buffer-c->c_be->output->orig_buffer);
    c->c_dcb(c);
}

void connection_read(struct bufferevent* e, void* data)
{
    char buf[READ_BUF_SIZE];
#if DEBUG
    struct connection* c = (struct connection*)data;
    Dprintf("Read on fd %d\n", c->c_fd);
#else
    (void) data;
#endif
    while (1) {
        ssize_t amt_read = 0;
        if ((amt_read = bufferevent_read(e, buf, READ_BUF_SIZE)) < 0) {
            perror("bufferevent_read");
            return;
        } else if (amt_read == 0)
            break;
        Dprintf("Read %zd bytes\n", amt_read);
        if (bufferevent_write(e, buf, amt_read)) {
            perror("bufferevent_write");
        }
    }
    bufferevent_enable(e, EV_READ|EV_WRITE);
}

bool fill_name(struct connection* c)
{
    struct sockaddr sin;
    char* host = malloc(48);
    char* srv = malloc(6);
    socklen_t sin_len = sizeof(struct sockaddr);
    if (getsockname(c->c_fd, &sin, &sin_len) < 0) {
        Dperror("fill_name/getsockname");
        free(host);
        free(srv);
        return false;
    }
    if (getnameinfo(&sin, sin_len, host, 48, srv, 6, NI_NUMERICHOST|NI_NUMERICSERV) != 0) {
        Dperror("getnameinfo");
        free(host);
        free(srv);
        return false;
    }
    c->c_host = host;
    c->c_srv = srv;
    return true;
}

struct connection* connection_init(int fd, disconnectcb dcb)
{
    struct connection* c;
    
    if ((c = malloc(sizeof(struct connection))) == NULL)
        return NULL;
    c->c_be = NULL;
    c->c_fd = fd;
    c->c_host = NULL;
    c->c_srv = NULL;
    if (!fill_name(c)) {
        c->c_host = malloc(1);
        c->c_srv = malloc(1);
        *c->c_host = '\0';
        *c->c_srv = '\0';
    }
    c->c_be = bufferevent_new(c->c_fd, &connection_read, &connection_write, &connection_error, c);
    /*bufferevent_setwatermark(c->c_be, EV_READ, 100, 8000);*/
    bufferevent_setwatermark(c->c_be, EV_WRITE, 0, 0);
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
    if (c->c_host != NULL)
        free(c->c_host);
    if (c->c_srv != NULL)
        free(c->c_srv);
    if (c != NULL)
        free(c);
}

/* vim: set expandtab ts=4 sw=4 sts=0: */
