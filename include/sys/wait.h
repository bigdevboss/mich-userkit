#ifndef MICH64_SYS_WAIT_H
#define MICH64_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1

/* A status word encodes either a bounded exit code or a signal death:
   signalled statuses carry the termsig in the low seven bits, exactly the
   shape POSIX WIFSIGNALED reads. */
#define WIFEXITED(status) (((status) & 0xFF) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) ((((status) & 0x7F) > 0) && \
                             (((status) & 0xFF) != 0x7F))
#define WTERMSIG(status) ((status) & 0x7F)

pid_t waitpid(pid_t pid, int *status, int options);

#endif
