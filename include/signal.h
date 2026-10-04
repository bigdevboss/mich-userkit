#ifndef MICH64_SIGNAL_H
#define MICH64_SIGNAL_H

#include <sys/types.h>

#define SIG_ERR ((void (*)(int))-1)

/* The bounded signal surface of the POSIX application profile: a fixed
   set of classic signals, one bit each, no queues, no job control, and no
   realtime extensions. Handlers take the bare signal number, so there is
   no siginfo path. */

typedef int sig_atomic_t;

/* Signal numbers, the POSIX values. Anything outside this set is EINVAL
   at the syscall boundary. */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

/* The kernel reads no sigaction flags beyond the restorer ride, which the
   wrapper supplies on its own. SA_RESTART stays unsupported: interrupted
   calls answer EINTR. */
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_RESTART 0x10000000
#define SA_RESTORER 0x04000000

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* One bit per signal, signals 1 through 31. The helpers keep the set
   opaque the way portable code expects. */
typedef struct { unsigned long bits; } sigset_t;

static inline void sigemptyset(sigset_t *set) {
    set->bits = 0;
}

static inline void sigfillset(sigset_t *set) {
    set->bits = 0x7FFFFFFFUL << 1 | 1UL;
}

static inline void sigaddset(sigset_t *set, int signo) {
    if (signo > 0 && signo <= 31) set->bits |= 1UL << (signo - 1);
}

static inline void sigdelset(sigset_t *set, int signo) {
    if (signo > 0 && signo <= 31) set->bits &= ~(1UL << (signo - 1));
}

static inline int sigismember(const sigset_t *set, int signo) {
    if (signo <= 0 || signo > 31) return 0;
    return (set->bits & (1UL << (signo - 1))) != 0;
}

struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    /* The return trampoline the kernel stacks for the handler. The
       wrapper installs its own when the caller leaves this NULL, which
       every caller in this profile does. */
    void (*sa_restorer)(void);
};

void (*signal(int signo, void (*handler)(int)))(int);
int kill(pid_t pid, int signo);
int raise(int signo);
int sigaction(int signo, const struct sigaction *action,
              struct sigaction *previous);
int sigprocmask(int how, const sigset_t *set, sigset_t *previous);
int sigpending(sigset_t *set);

#endif
