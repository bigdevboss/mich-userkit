#ifndef MICH64_POLL_H
#define MICH64_POLL_H

#include <sys/types.h>

typedef unsigned int nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN 0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

// The kernel scans a poll list in one copied request, so the list is
// bounded by the descriptor table itself; a longer one answers EINVAL
// rather than a half-polled answer. A descriptor below zero is the POSIX
// ignore slot, and one that names no open descriptor comes back POLLNVAL.
// A timeout of zero returns at once, a negative one waits forever, and a
// signal that arrives mid wait answers EINTR.
int poll(struct pollfd *descriptors, nfds_t count, int timeout_ms);

#endif
