#ifndef SENDLIB_H
#define SENDLIB_H

#include <sys/types.h>
#include <unistd.h>
#include <time.h>

void timespec_subtract(struct timespec* restrict res, struct timespec* lhs, struct timespec* rhs);
int do_connect(const char* restrict host, const char* restrict svc, struct timespec* getaddr_start, struct timespec* getaddr_end, struct timespec* connect_start, struct timespec* connect_end, bool set_nonblock);

#endif /* SENDLIB_H */
