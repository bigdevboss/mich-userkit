#ifndef MICH64_SYS_WAIT_H
#define MICH64_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 4

// A status word encodes four things in the low byte, the way Linux spells
// it: zero means an exit with the code shifted up, one to 0x7E is a death
// carrying the signal, 0x7F is a stop with the stopping signal shifted up,
// and all ones is a continue. The two job-control shapes take the low byte
// whole, so no exit code can be read as one, but a caller must still test
// WIFSTOPPED and WIFCONTINUED before WIFSIGNALED: the old macro only ever
// excluded the stop shape, and a continue still looks like a death to it.
#define WIFSTOPPED(status) (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status) (((status) >> 8) & 0xFF)
#define WIFCONTINUED(status) ((status) == 0xFFFF)
#define WIFEXITED(status) (((status) & 0xFF) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) ((((status) & 0x7F) > 0) && \
                             (((status) & 0xFF) != 0x7F))
#define WTERMSIG(status) ((status) & 0x7F)

pid_t waitpid(pid_t pid, int *status, int options);

#endif
