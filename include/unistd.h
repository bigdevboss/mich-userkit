#ifndef MICH64_UNISTD_H
#define MICH64_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

extern char **environ;

ssize_t read(int fd, void *buffer, size_t length);
ssize_t write(int fd, const void *buffer, size_t length);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int dup(int fd);
int dup2(int fd, int replacement);
int chdir(const char *path);
char *getcwd(char *buffer, size_t size);
int truncate(const char *path, off_t size);
int ftruncate(int fd, off_t length);
ssize_t pread(int fd, void *buffer, size_t length, off_t offset);
ssize_t pwrite(int fd, const void *buffer, size_t length, off_t offset);
int fsync(int fd);
int fdatasync(int fd);
int chown(const char *path, uid_t owner, gid_t group);
int link(const char *old_path, const char *new_path);
int rename(const char *old_path, const char *new_path);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buffer, size_t size);

// access modes
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// access reports the permission picture at the instant of the call. The
// answer can go stale before the caller acts on it, because nothing stops
// another process from chmod'ing or unlinking the path in between, so the
// only safe use is reporting an error, never preflighting an operation.
int access(const char *path, int mode);

pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
void _exit(int code) __attribute__((noreturn));
pid_t waitpid(pid_t pid, int *status, int options);
pid_t getpid(void);
pid_t getppid(void);
// Park the calling task for whole seconds. Nothing interrupts the park yet,
// so the answer is always zero unslept time; the shape is what signals will
// change.
unsigned int sleep(unsigned int seconds);

// Narrow the syscall surface. The first call fixes the ceiling, later
// calls may only remove promises, and a call outside the set dies with
// SIGABRT unless the "error" promise turned denials into ENOSYS. Either
// string may be NULL to leave that half unchanged; an empty string is a
// real empty set. execpromises replaces the set after the next execve.
// Mirrors OpenBSD pledge(2) over this profile's canon-9 vocabulary.
int pledge(const char *promises, const char *execpromises);

// Drop a veil over the namespace: after the first call only the unveiled
// paths stay visible. Permissions are the letters "rwx c" in one string.
// unveil(NULL, NULL) locks the table forever, and a pledge without the
// "unveil" promise closes the window the same way. Mirrors OpenBSD
// unveil(2).
int unveil(const char *path, const char *permissions);

// Two descriptors over one kernel ring: descriptors[0] reads and
// descriptors[1] writes. A read on an empty ring parks until a write or
// the last write end closes (which answers 0), and a write onto a full
// ring parks until a read drains room. Writes of at most 4096 bytes are
// atomic; a write with no read end left answers EPIPE and raises
// SIGPIPE.
int pipe(int descriptors[2]);

// Process groups. The profile has a single session, so a group is created
// by naming the leader's pid; setpgid(0, 0) puts the caller in a group of
// its own, which is what a shell does before handing over the terminal.
pid_t getpgrp(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t tcgetpgrp(int descriptor);
int tcsetpgrp(int descriptor, pid_t pgid);


#endif
