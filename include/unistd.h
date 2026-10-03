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
int chown(const char *path, uid_t owner, gid_t group);
int link(const char *old_path, const char *new_path);
int rename(const char *old_path, const char *new_path);

pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
void _exit(int code) __attribute__((noreturn));
pid_t waitpid(pid_t pid, int *status, int options);
pid_t getpid(void);
pid_t getppid(void);

#endif
