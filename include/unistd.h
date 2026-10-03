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

pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
void _exit(int code) __attribute__((noreturn));
pid_t waitpid(pid_t pid, int *status, int options);
pid_t getpid(void);
pid_t getppid(void);

#endif
