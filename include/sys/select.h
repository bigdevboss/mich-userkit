#ifndef MICH64_SYS_SELECT_H
#define MICH64_SYS_SELECT_H

#include <sys/types.h>
#include <time.h>

// The set is one word, sized to the descriptor table: bit n stands for
// descriptor n, and a descriptor past it cannot be open in the first
// place. The kernel waits on the poll list this shim builds, so select
// inherits poll's bounds and its EINTR answer.
#define FD_SETSIZE 32

typedef struct {
    unsigned long bits[(FD_SETSIZE + 63u) / 64u];
} fd_set;

#define FD_ZERO(set) \
    do { \
        for (unsigned int fd_index = 0; \
             fd_index < (FD_SETSIZE + 63u) / 64u; fd_index++) \
            (set)->bits[fd_index] = 0; \
    } while (0)
#define FD_SET(fd, set) \
    do { \
        (set)->bits[(unsigned int)(fd) / 64u] |= \
            1ul << ((unsigned int)(fd) % 64u); \
    } while (0)
#define FD_CLR(fd, set) \
    do { \
        (set)->bits[(unsigned int)(fd) / 64u] &= \
            ~(1ul << ((unsigned int)(fd) % 64u)); \
    } while (0)
#define FD_ISSET(fd, set) \
    (((set)->bits[(unsigned int)(fd) / 64u] >> \
      ((unsigned int)(fd) % 64u)) & 1ul)

// The exception set has no kernel meaning here: an empty one is ignored
// and a non-empty one answers EINVAL, since nothing in this profile
// reports out-of-band data.
int select(int count, fd_set *read_set, fd_set *write_set,
           fd_set *except_set, struct timeval *timeout);

#endif
