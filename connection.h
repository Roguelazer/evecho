#ifndef CONNECTION_H
#define CONNECTION_H

#include <sys/types.h>
#include <event.h>

#define READ_BUF_SIZE 1500

struct connection;
typedef void (*disconnectcb)(struct connection*);

struct connection
{
    struct bufferevent* c_be;
    int c_fd;
    disconnectcb c_dcb;
    char* c_host;
    char* c_srv;
};

struct connection* connection_init(int, disconnectcb);
void connection_free(struct connection*);

void connection_read(struct bufferevent*, void*);
void connection_write(struct bufferevent*, void*);
void connection_error(struct bufferevent*, short, void*);

#endif /* CONNECTION_H */
/* vim: set expandtab ts=4 sw=4 sts=0: */
