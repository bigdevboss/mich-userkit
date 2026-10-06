#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <mich/syscall.h>
#include <poll.h>
#include <sys/select.h>
#include <termios.h>
#include <posix_abi.h>

int errno;

static int copy_path(char destination[VFS_PATH_MAX], const char *source) {
    if (!source) return -EINVAL;
    u32 length = 0;
    while (length + 1 < VFS_PATH_MAX && source[length]) {
        destination[length] = source[length];
        length++;
    }
    if (source[length]) return -ENAMETOOLONG;
    // The dispatcher rejects a request whose last path byte is not zero,
    // so the whole tail past the string must be cleared, not just the
    // terminator: leftover stack bytes would read as a non canonical path.
    for (u32 index = length; index < VFS_PATH_MAX; index++)
        destination[index] = 0;
    return 0;
}

static long request_call(u32 number, void *request) {
    return mich_syscall1(number, (unsigned long)request);
}

static int result_int(long result) {
    if (result >= 0) return (int)result;
    errno = (int)-result;
    return -1;
}

int open(const char *path, int flags, ...) {
    struct posix_open_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    request.flags = (u32)flags;
    if (flags & O_CREAT) {
        __builtin_va_list arguments;
        __builtin_va_start(arguments, flags);
        request.mode = __builtin_va_arg(arguments, unsigned int);
        __builtin_va_end(arguments);
    }
    return result_int(request_call(POSIX_SYSCALL_OPEN, &request));
}

int close(int fd) {
    struct posix_fd_request request = { fd, 0 };
    return result_int(request_call(POSIX_SYSCALL_CLOSE, &request));
}

int pipe(int descriptors[2]) {
    struct posix_pipe_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    long result = request_call(POSIX_SYSCALL_PIPE, &request);
    if (result < 0) return result_int(result);
    descriptors[0] = request.descriptors[0];
    descriptors[1] = request.descriptors[1];
    return 0;
}

// The kernel takes the whole list in one copied request, so the bound is
// the descriptor table and not a staging size; a longer list is refused
// rather than cut.
int poll(struct pollfd *descriptors, nfds_t count, int timeout_ms) {
    if (!descriptors && count) {
        errno = EINVAL;
        return -1;
    }
    // An empty list is the POSIX way of spelling a sleep, and the kernel
    // request never carries fewer than one descriptor: answer it here.
    if (!count) {
        if (timeout_ms < 0) {
            errno = EINVAL;
            return -1;
        }
        struct timespec interval;
        interval.tv_sec = (time_t)(timeout_ms / 1000);
        interval.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        return nanosleep(&interval, 0);
    }
    if (count > POSIX_POLL_FD_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct posix_poll_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    request.count = count;
    for (u32 index = 0; index < count; index++) {
        request.fds[index].descriptor = descriptors[index].fd;
        request.fds[index].events = (u16)descriptors[index].events;
    }
    long result = mich_syscall2(POSIX_SYSCALL_POLL, (unsigned long)&request,
                                (unsigned long)timeout_ms);
    if (result < 0) return result_int(result);
    for (u32 index = 0; index < count; index++)
        descriptors[index].revents = (short)request.fds[index].revents;
    return (int)result;
}

// select is a shim over poll: the sets become a poll list, and the answer
// lands back in the sets. The kernel reports only what was asked for plus
// the error bits, so any answer on a watched descriptor is that direction
// being ready, which is what a caller tests for anyway.
int select(int count, fd_set *read_set, fd_set *write_set,
           fd_set *except_set, struct timeval *timeout) {
    if (count < 0 || count > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    if (except_set) {
        for (unsigned int fd = 0; fd < FD_SETSIZE; fd++)
            if (FD_ISSET(fd, except_set)) {
                errno = EINVAL;
                return -1;
            }
    }
    for (unsigned int fd = (unsigned int)count; fd < FD_SETSIZE; fd++) {
        if (read_set) FD_CLR(fd, read_set);
        if (write_set) FD_CLR(fd, write_set);
    }
    struct pollfd list[FD_SETSIZE];
    unsigned int used = 0;
    for (unsigned int fd = 0; fd < (unsigned int)count; fd++) {
        short events = 0;
        if (read_set && FD_ISSET(fd, read_set)) events |= POLLIN;
        if (write_set && FD_ISSET(fd, write_set)) events |= POLLOUT;
        if (!events) continue;
        list[used].fd = (int)fd;
        list[used].events = events;
        list[used].revents = 0;
        used++;
    }
    int timeout_ms = -1;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0) {
            errno = EINVAL;
            return -1;
        }
        // Rounded up, so a select never returns before its timeout the way
        // nanosleep never wakes early.
        timeout_ms = (int)(timeout->tv_sec * 1000 +
                           (timeout->tv_usec + 999) / 1000);
    }
    int result = poll(list, used, timeout_ms);
    if (read_set) FD_ZERO(read_set);
    if (write_set) FD_ZERO(write_set);
    if (except_set) FD_ZERO(except_set);
    if (result <= 0) return result;
    for (unsigned int index = 0; index < used; index++) {
        if (!list[index].revents) continue;
        if (read_set && (list[index].events & POLLIN))
            FD_SET(list[index].fd, read_set);
        if (write_set && (list[index].events & POLLOUT))
            FD_SET(list[index].fd, write_set);
    }
    return result;
}

int ioctl(int descriptor, unsigned long request, void *argument) {
    long result = mich_syscall3(POSIX_SYSCALL_IOCTL,
                                (unsigned long)descriptor, request,
                                (unsigned long)argument);
    return result_int(result);
}

int tcgetattr(int descriptor, struct termios *termios) {
    if (!termios) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(descriptor, TCGETS, termios);
}

int tcsetattr(int descriptor, int actions, const struct termios *termios) {
    if (!termios || actions < 0 || actions > TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(descriptor, TCSETS, (void *)termios);
}

static ssize_t io_call(u32 number, int fd, void *buffer, size_t length) {
    if (!buffer && length) {
        errno = EINVAL;
        return -1;
    }
    size_t done = 0;
    while (done < length) {
        u32 chunk = length - done > POSIX_IO_MAX ? POSIX_IO_MAX :
            (u32)(length - done);
        struct posix_io_request request;
        request.descriptor = fd;
        request.length = chunk;
        request.transferred = 0;
        if (number == POSIX_SYSCALL_WRITE)
            for (u32 index = 0; index < chunk; index++)
                request.data[index] = ((const u8 *)buffer)[done + index];
        long result = request_call(number, &request);
        if (result < 0) {
            if (done) return (ssize_t)done;
            errno = (int)-result;
            return -1;
        }
        if (number == POSIX_SYSCALL_READ)
            for (u32 index = 0; index < (u32)result; index++)
                ((u8 *)buffer)[done + index] = request.data[index];
        done += (u32)result;
        if ((u32)result < chunk) break;
    }
    return (ssize_t)done;
}

ssize_t read(int fd, void *buffer, size_t length) {
    return io_call(POSIX_SYSCALL_READ, fd, buffer, length);
}

ssize_t write(int fd, const void *buffer, size_t length) {
    return io_call(POSIX_SYSCALL_WRITE, fd, (void *)buffer, length);
}

// The positioned calls share the plain io chunking but pin every chunk to
// an explicit offset, so the descriptor cursor never moves.
static ssize_t pio_call(u32 number, int fd, void *buffer, size_t length,
                        off_t offset) {
    if (!buffer && length) {
        errno = EINVAL;
        return -1;
    }
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    size_t done = 0;
    while (done < length) {
        u32 chunk = length - done > POSIX_IO_MAX ? POSIX_IO_MAX :
            (u32)(length - done);
        struct posix_pio_request request;
        request.descriptor = fd;
        request.reserved = 0;
        request.offset = offset + (off_t)done;
        request.length = chunk;
        if (number == POSIX_SYSCALL_PWRITE)
            for (u32 index = 0; index < chunk; index++)
                request.data[index] = ((const u8 *)buffer)[done + index];
        long result = request_call(number, &request);
        if (result < 0) {
            if (done) return (ssize_t)done;
            errno = (int)-result;
            return -1;
        }
        if (number == POSIX_SYSCALL_PREAD)
            for (u32 index = 0; index < (u32)result; index++)
                ((u8 *)buffer)[done + index] = request.data[index];
        done += (u32)result;
        if ((u32)result < chunk) break;
    }
    return (ssize_t)done;
}

ssize_t pread(int fd, void *buffer, size_t length, off_t offset) {
    return pio_call(POSIX_SYSCALL_PREAD, fd, buffer, length, offset);
}

ssize_t pwrite(int fd, const void *buffer, size_t length, off_t offset) {
    return pio_call(POSIX_SYSCALL_PWRITE, fd, (void *)buffer, length, offset);
}

off_t lseek(int fd, off_t offset, int whence) {
    struct posix_seek_request request;
    request.descriptor = fd;
    request.whence = (u32)whence;
    request.offset = offset;
    request.position = 0;
    request.reserved = 0;
    long result = request_call(POSIX_SYSCALL_LSEEK, &request);
    if (result < 0) {
        errno = (int)-result;
        return (off_t)-1;
    }
    return (off_t)result;
}

int dup(int fd) {
    struct posix_fd_request request = { fd, 0 };
    return result_int(request_call(POSIX_SYSCALL_DUP, &request));
}

int dup2(int fd, int replacement) {
    struct posix_dup2_request request = { fd, replacement };
    return result_int(request_call(POSIX_SYSCALL_DUP2, &request));
}

int fcntl(int fd, int command, ...) {
    struct posix_fcntl_request request;
    request.descriptor = fd;
    request.command = (u32)command;
    request.argument = 0;
    request.reserved = 0;
    if (command == F_SETFD) {
        __builtin_va_list arguments;
        __builtin_va_start(arguments, command);
        request.argument = __builtin_va_arg(arguments, unsigned int);
        __builtin_va_end(arguments);
    }
    return result_int(request_call(POSIX_SYSCALL_FCNTL, &request));
}

static void copy_stat(struct stat *destination,
                      const struct posix_stat_record *source) {
    destination->st_mode = source->st_mode;
    destination->st_size = source->st_size;
    destination->st_nlink = source->st_nlink;
    destination->st_uid = source->st_uid;
    destination->st_gid = source->st_gid;
    destination->st_atime = source->st_atime;
    destination->st_mtime = source->st_mtime;
    destination->st_ctime = source->st_ctime;
    destination->st_atime_nsec = source->st_atime_nsec;
    destination->st_mtime_nsec = source->st_mtime_nsec;
}

int stat(const char *path, struct stat *buffer) {
    if (!buffer) {
        errno = EINVAL;
        return -1;
    }
    struct posix_stat_path_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    long result = request_call(POSIX_SYSCALL_STAT, &request);
    if (result < 0) return result_int(result);
    copy_stat(buffer, &request.stat);
    return 0;
}

int fstat(int fd, struct stat *buffer) {
    if (!buffer) {
        errno = EINVAL;
        return -1;
    }
    struct posix_fstat_request request;
    request.descriptor = fd;
    request.reserved = 0;
    request.stat.st_mode = 0;
    request.stat.st_size = 0;
    request.stat.st_nlink = 0;
    request.stat.st_uid = 0;
    request.stat.st_gid = 0;
    request.stat.st_atime = 0;
    request.stat.st_mtime = 0;
    request.stat.st_ctime = 0;
    long result = request_call(POSIX_SYSCALL_FSTAT, &request);
    if (result < 0) return result_int(result);
    copy_stat(buffer, &request.stat);
    return 0;
}

int getdents(int fd, struct dirent *buffer, unsigned int length) {
    if (!buffer || !length || length > POSIX_IO_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct posix_getdents_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    request.descriptor = fd;
    request.length = length;
    long result = request_call(POSIX_SYSCALL_GETDENTS, &request);
    if (result < 0) return result_int(result);
    for (u32 index = 0; index < (u32)result; index++)
        ((u8 *)buffer)[index] = request.data[index];
    return (int)result;
}

int chmod(const char *path, mode_t mode) {
    struct posix_chmod_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    request.mode = (u32)mode;
    return result_int(request_call(POSIX_SYSCALL_CHMOD, &request));
}

int fchmod(int fd, mode_t mode) {
    struct posix_fchmod_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    request.descriptor = fd;
    request.mode = (u32)mode;
    return result_int(request_call(POSIX_SYSCALL_FCHMOD, &request));
}

int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags) {
    if (dirfd != AT_FDCWD || flags) {
        errno = EINVAL;
        return -1;
    }
    struct posix_utimensat_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    // A NULL times pair is the classic utime request: stamp both halves
    // with the clock. tv_sec rides along ignored when NOW or OMIT speaks.
    if (times) {
        request.atime_sec = times[0].tv_sec;
        request.atime_nsec = times[0].tv_nsec;
        request.mtime_sec = times[1].tv_sec;
        request.mtime_nsec = times[1].tv_nsec;
    } else {
        request.atime_nsec = POSIX_UTIME_NOW;
        request.mtime_nsec = POSIX_UTIME_NOW;
    }
    return result_int(request_call(POSIX_SYSCALL_UTIMENSAT, &request));
}

int futimens(int fd, const struct timespec times[2]) {
    struct posix_futimens_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    request.descriptor = fd;
    if (times) {
        request.atime_sec = times[0].tv_sec;
        request.atime_nsec = times[0].tv_nsec;
        request.mtime_sec = times[1].tv_sec;
        request.mtime_nsec = times[1].tv_nsec;
    } else {
        request.atime_nsec = POSIX_UTIME_NOW;
        request.mtime_nsec = POSIX_UTIME_NOW;
    }
    return result_int(request_call(POSIX_SYSCALL_FUTIMENS, &request));
}

int chown(const char *path, uid_t owner, gid_t group) {
    struct posix_chown_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    request.uid = (i32)owner;
    request.gid = (i32)group;
    return result_int(request_call(POSIX_SYSCALL_CHOWN, &request));
}

int link(const char *old_path, const char *new_path) {
    struct posix_link_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.old_path, old_path);
    if (copied) return result_int(copied);
    copied = copy_path(request.new_path, new_path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_LINK, &request));
}

int rename(const char *old_path, const char *new_path) {
    struct posix_rename_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.old_path, old_path);
    if (copied) return result_int(copied);
    copied = copy_path(request.new_path, new_path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_RENAME, &request));
}

int symlink(const char *target, const char *path) {
    struct posix_symlink_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.target, target);
    if (copied) return result_int(copied);
    copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_SYMLINK, &request));
}

ssize_t readlink(const char *path, char *buffer, size_t size) {
    if (!buffer || !size) {
        errno = EINVAL;
        return -1;
    }
    struct posix_readlink_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    long result = request_call(POSIX_SYSCALL_READLINK, &request);
    if (result < 0) return result_int(result);
    // The system call keeps the POSIX contract: the buffer gets the raw
    // bytes, no terminator, and a smaller caller buffer truncates while
    // the return keeps the count actually placed.
    size_t count = (size_t)result;
    if (count > size) count = size;
    for (size_t index = 0; index < count; index++)
        buffer[index] = request.data[index];
    return (ssize_t)count;
}

int lstat(const char *path, struct stat *buffer) {
    if (!buffer) {
        errno = EINVAL;
        return -1;
    }
    struct posix_stat_path_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    long result = request_call(POSIX_SYSCALL_LSTAT, &request);
    if (result < 0) return result_int(result);
    copy_stat(buffer, &request.stat);
    return 0;
}

mode_t umask(mode_t mask) {
    struct posix_umask_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    request.mask = (u32)mask;
    return (mode_t)request_call(POSIX_SYSCALL_UMASK, &request);
}

int mkdir(const char *path, mode_t mode) {
    struct posix_mode_path_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    request.mode = mode;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_MKDIR, &request));
}

static int path_call(u32 number, const char *path) {
    struct posix_path_request request;
    for (u32 index = 0; index < sizeof(request); index++) request.path[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(number, &request));
}

int rmdir(const char *path) {
    return path_call(POSIX_SYSCALL_RMDIR, path);
}

int unlink(const char *path) {
    return path_call(POSIX_SYSCALL_UNLINK, path);
}

int chdir(const char *path) {
    return path_call(POSIX_SYSCALL_CHDIR, path);
}

char *getcwd(char *buffer, size_t size) {
    if (!buffer || !size) {
        errno = EINVAL;
        return 0;
    }
    struct posix_getcwd_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    request.capacity = size > VFS_PATH_MAX ? VFS_PATH_MAX : (u32)size;
    long result = request_call(POSIX_SYSCALL_GETCWD, &request);
    if (result < 0) {
        errno = (int)-result;
        return 0;
    }
    for (u32 index = 0; index <= request.length; index++) buffer[index] = request.path[index];
    return buffer;
}

ssize_t getrandom(void *buffer, size_t length, unsigned int flags) {
    long result = mich_syscall3(POSIX_SYSCALL_GETRANDOM,
                                (unsigned long)buffer, (unsigned long)length,
                                (unsigned long)flags);
    if (result >= 0) return (ssize_t)result;
    errno = (int)-result;
    return -1;
}

int truncate(const char *path, off_t size) {
    if (size < 0 || (u64)size > VFS_FILE_SIZE_MAX) {
        errno = size < 0 ? EINVAL : EFBIG;
        return -1;
    }
    struct posix_truncate_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    request.size = (u32)size;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_TRUNCATE, &request));
}

int ftruncate(int fd, off_t length) {
    struct posix_ftruncate_request request;
    request.descriptor = fd;
    request.reserved = 0;
    request.length = length;
    return result_int(request_call(POSIX_SYSCALL_FTRUNCATE, &request));
}

int fsync(int fd) {
    struct posix_fd_request request = { fd, 0 };
    return result_int(request_call(POSIX_SYSCALL_FSYNC, &request));
}

int fdatasync(int fd) {
    struct posix_fd_request request = { fd, 0 };
    return result_int(request_call(POSIX_SYSCALL_FDATASYNC, &request));
}

int access(const char *path, int mode) {
    if (mode & ~(R_OK | W_OK | X_OK)) {
        errno = EINVAL;
        return -1;
    }
    struct posix_access_request request;
    request.mode = (u32)mode;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_ACCESS, &request));
}

static int clock_call(u32 number, clockid_t clock, struct timespec *out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    struct posix_clock_request request = { (u32)clock, 0, 0, 0 };
    if (result_int(request_call(number, &request))) return -1;
    out->tv_sec = (time_t)request.sec;
    out->tv_nsec = (long)request.nsec;
    return 0;
}

int clock_gettime(clockid_t clock, struct timespec *out) {
    return clock_call(POSIX_SYSCALL_CLOCK_GETTIME, clock, out);
}

int clock_getres(clockid_t clock, struct timespec *out) {
    return clock_call(POSIX_SYSCALL_CLOCK_GETRES, clock, out);
}

int nanosleep(const struct timespec *requested, struct timespec *remaining) {
    if (!requested) {
        errno = EINVAL;
        return -1;
    }
    struct posix_nanosleep_request request = {
        requested->tv_sec, requested->tv_nsec, 0, 0
    };
    int failed = result_int(request_call(POSIX_SYSCALL_NANOSLEEP, &request));
    // The park is interruptible now (a signal breaks it with EINTR), and
    // POSIX requires the remainder report exactly then: copy it through
    // on the failure path too, not only on the sleep-out success path.
    if (remaining) {
        remaining->tv_sec = (time_t)request.remaining_sec;
        remaining->tv_nsec = (long)request.remaining_nsec;
    }
    return failed ? -1 : 0;
}

time_t time(time_t *out) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) return (time_t)-1;
    if (out) *out = now.tv_sec;
    return now.tv_sec;
}

unsigned int sleep(unsigned int seconds) {
    struct timespec interval;
    interval.tv_sec = (time_t)seconds;
    interval.tv_nsec = 0;
    nanosleep(&interval, 0);
    return 0;
}

int gettimeofday(struct timeval *out, void *timezone) {
    // The profile carries no timezone table, so the second pointer stays
    // NULL rather than silently reading garbage.
    if (!out || timezone) {
        errno = EINVAL;
        return -1;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) return -1;
    out->tv_sec = now.tv_sec;
    out->tv_usec = now.tv_nsec / 1000;
    return 0;
}

int kill(pid_t pid, int signo) {
    long result = mich_syscall2(POSIX_SYSCALL_KILL, (unsigned long)pid,
                                (unsigned long)(u32)signo);
    return result_int((int)result);
}

int raise(int signo) {
    return kill(getpid(), signo);
}

static int sigaction_call(struct posix_sigaction_request *request) {
    return result_int(request_call(POSIX_SYSCALL_SIGACTION, request));
}

static int copy_promise(char destination[POSIX_PLEDGE_PROMISE_MAX],
                       const char *source) {
    u32 length = 0;
    while (length + 1 < POSIX_PLEDGE_PROMISE_MAX && source[length]) {
        destination[length] = source[length];
        length++;
    }
    if (source[length]) return -EINVAL;
    for (u32 index = length; index < POSIX_PLEDGE_PROMISE_MAX; index++)
        destination[index] = 0;
    return 0;
}

int pledge(const char *promises, const char *execpromises) {
    struct posix_pledge_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    // A NULL half means "leave it alone" and an empty string a real
    // empty set; the flags carry that distinction across the copy.
    if (promises) {
        int copied = copy_promise(request.promises, promises);
        if (copied) return result_int(copied);
        request.flags |= POSIX_PLEDGE_HAS_PROMISES;
    }
    if (execpromises) {
        int copied = copy_promise(request.execpromises, execpromises);
        if (copied) return result_int(copied);
        request.flags |= POSIX_PLEDGE_HAS_EXEC_PROMISES;
    }
    return result_int(request_call(POSIX_SYSCALL_PLEDGE, &request));
}

int unveil(const char *path, const char *permissions) {
    struct posix_unveil_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    if (!path && !permissions) {
        request.flags = POSIX_UNVEIL_LOCK;
    } else if (!path || !permissions) {
        errno = EINVAL;
        return -1;
    } else {
        int copied = copy_path(request.path, path);
        if (copied) return result_int(copied);
        u32 length = 0;
        while (length + 1 < 8 && permissions[length]) {
            request.permissions[length] = permissions[length];
            length++;
        }
        if (permissions[length]) {
            errno = EINVAL;
            return -1;
        }
    }
    return result_int(request_call(POSIX_SYSCALL_UNVEIL, &request));
}

int sigaction(int signo, const struct sigaction *action,
              struct sigaction *previous) {
    struct posix_sigaction_request request;
    for (u32 index = 0; index < sizeof(request); index++)
        ((u8 *)&request)[index] = 0;
    request.signo = signo;
    if (action) {
        request.flags = POSIX_SA_APPLY;
        request.handler = (uptr_t)action->sa_handler;
        request.mask = action->sa_mask.bits;
        if (action->sa_handler != SIG_DFL &&
            action->sa_handler != SIG_IGN) {
            // The kernel has no trampoline of its own, so a caught
            // handler always rides with the libc restorer unless the
            // caller brought one.
            request.restorer = action->sa_restorer ?
                (uptr_t)action->sa_restorer : (uptr_t)&mich_sigreturn;
        }
    }
    if (sigaction_call(&request)) return -1;
    if (previous) {
        previous->sa_handler = (void (*)(int))request.previous_handler;
        previous->sa_mask.bits = request.previous_mask;
        previous->sa_flags = (int)request.previous_flags;
        previous->sa_restorer = (void (*)(void))request.previous_restorer;
    }
    return 0;
}

void (*signal(int signo, void (*handler)(int)))(int) {
    struct sigaction action;
    struct sigaction previous;
    sigemptyset(&action.sa_mask);
    action.sa_handler = handler;
    action.sa_flags = 0;
    action.sa_restorer = 0;
    if (sigaction(signo, &action, &previous)) return SIG_ERR;
    return previous.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *previous) {
    // POSIX reads the previous mask even when the new one is missing,
    // which the request shape answers with a no-op setmask of zero.
    struct posix_sigprocmask_request request;
    request.how = set ? (u32)how : POSIX_SIG_SETMASK;
    request.reserved = 0;
    request.mask = set ? set->bits : 0;
    request.previous = 0;
    if (result_int(request_call(POSIX_SYSCALL_SIGPROCMASK, &request)))
        return -1;
    if (previous) previous->bits = request.previous;
    return 0;
}

int sigpending(sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    struct posix_sigpending_request request;
    request.pending = 0;
    if (result_int(request_call(POSIX_SYSCALL_SIGPENDING, &request)))
        return -1;
    set->bits = request.pending;
    return 0;
}

// The socket calls stage their payload and addresses the way the io
// pair does: one request the kernel copies in and answers with a copy
// back carrying the moved bytes and the filled address. Flags carry no
// v0 meaning, so a nonzero flag word answers EOPNOTSUPP before the
// kernel is asked, and one datagram rides whole in a single request.

static int check_socket_flags(int flags) {
    if (!flags) return 0;
    errno = EOPNOTSUPP;
    return -1;
}

static int copy_address_in(struct posix_sockaddr_in *destination,
                           const struct sockaddr *source,
                           socklen_t length) {
    if (!source || length < (socklen_t)sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    for (u32 index = 0; index < sizeof(*destination); index++)
        ((u8 *)destination)[index] = ((const u8 *)source)[index];
    return 0;
}

static void zero_request(void *request, u32 size) {
    for (u32 index = 0; index < size; index++) ((u8 *)request)[index] = 0;
}

int socket(int domain, int type, int protocol) {
    struct posix_socket_request request;
    zero_request(&request, sizeof(request));
    request.domain = (u32)domain;
    request.type = (u32)type;
    request.protocol = (u32)protocol;
    return result_int(request_call(POSIX_SYSCALL_SOCKET, &request));
}

int bind(int fd, const struct sockaddr *address, socklen_t length) {
    struct posix_socket_address_request request;
    zero_request(&request, sizeof(request));
    if (copy_address_in(&request.address, address, length)) return -1;
    request.descriptor = fd;
    request.length = sizeof(request.address);
    return result_int(request_call(POSIX_SYSCALL_BIND, &request));
}

int connect(int fd, const struct sockaddr *address, socklen_t length) {
    struct posix_socket_address_request request;
    zero_request(&request, sizeof(request));
    if (copy_address_in(&request.address, address, length)) return -1;
    request.descriptor = fd;
    request.length = sizeof(request.address);
    return result_int(request_call(POSIX_SYSCALL_CONNECT, &request));
}

int listen(int fd, int backlog) {
    struct posix_socket_listen_request request;
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    request.backlog = (u32)backlog;
    return result_int(request_call(POSIX_SYSCALL_LISTEN, &request));
}

// The v0 accept carries no peer address back; a caller that asks for
// one learns nothing, because the accepted socket keeps no name either.
int accept(int fd, struct sockaddr *address, socklen_t *length) {
    (void)address;
    (void)length;
    struct posix_socket_accept_request request;
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    return result_int(request_call(POSIX_SYSCALL_ACCEPT, &request));
}

// A stream send chunks at the staging bound the way the file io pair
// does; a parked chunk answers through the request copy the waker
// wrote, so the transferred word is read back after every call.
ssize_t send(int fd, const void *buffer, size_t length, int flags) {
    if (check_socket_flags(flags)) return -1;
    if (!buffer && length) {
        errno = EINVAL;
        return -1;
    }
    size_t done = 0;
    while (done < length) {
        u32 chunk = length - done > POSIX_IO_MAX ? POSIX_IO_MAX :
            (u32)(length - done);
        struct posix_socket_io_request request;
        zero_request(&request, sizeof(request));
        request.descriptor = fd;
        request.length = chunk;
        for (u32 index = 0; index < chunk; index++)
            request.data[index] = ((const u8 *)buffer)[done + index];
        long result = request_call(POSIX_SYSCALL_SEND, &request);
        if (result < 0) {
            if (done) return (ssize_t)done;
            return result_int(result);
        }
        done += request.transferred;
        if (request.transferred < chunk) break;
    }
    return (ssize_t)done;
}

ssize_t recv(int fd, void *buffer, size_t length, int flags) {
    if (check_socket_flags(flags)) return -1;
    if (!buffer && length) {
        errno = EINVAL;
        return -1;
    }
    if (length > POSIX_IO_MAX) length = POSIX_IO_MAX;
    struct posix_socket_io_request request;
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    request.length = (u32)length;
    long result = request_call(POSIX_SYSCALL_RECV, &request);
    if (result < 0) return result_int(result);
    for (u32 index = 0; index < request.transferred; index++)
        ((u8 *)buffer)[index] = request.data[index];
    return (ssize_t)request.transferred;
}

// One datagram rides whole: the staging bound is the datagram bound, so
// a larger ask is EMSGSIZE rather than a cut.
ssize_t sendto(int fd, const void *buffer, size_t length, int flags,
               const struct sockaddr *destination, socklen_t destlen) {
    if (check_socket_flags(flags)) return -1;
    if (!buffer && length) {
        errno = EINVAL;
        return -1;
    }
    if (length > POSIX_IO_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    struct posix_socket_io_request request;
    zero_request(&request, sizeof(request));
    if (destination &&
        copy_address_in(&request.address, destination, destlen))
        return -1;
    request.descriptor = fd;
    request.address_length = destination ? sizeof(request.address) : 0;
    request.length = (u32)length;
    for (u32 index = 0; index < length; index++)
        request.data[index] = ((const u8 *)buffer)[index];
    long result = request_call(POSIX_SYSCALL_SENDTO, &request);
    if (result < 0) return result_int(result);
    return (ssize_t)request.transferred;
}

ssize_t recvfrom(int fd, void *buffer, size_t length, int flags,
                 struct sockaddr *source, socklen_t *sourcelen) {
    if (check_socket_flags(flags)) return -1;
    if (!buffer && length) {
        errno = EINVAL;
        return -1;
    }
    if (length > POSIX_IO_MAX) length = POSIX_IO_MAX;
    struct posix_socket_io_request request;
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    request.address_length = sizeof(request.address);
    request.length = (u32)length;
    long result = request_call(POSIX_SYSCALL_RECVFROM, &request);
    if (result < 0) return result_int(result);
    for (u32 index = 0; index < request.transferred; index++)
        ((u8 *)buffer)[index] = request.data[index];
    if (source && sourcelen) {
        for (u32 index = 0; index < sizeof(request.address) &&
             index < *sourcelen; index++)
            ((u8 *)source)[index] = ((u8 *)&request.address)[index];
        *sourcelen = request.address_length ?
            (socklen_t)sizeof(struct sockaddr_in) : 0;
    }
    return (ssize_t)request.transferred;
}

// The message calls translate the POSIX header into the kernel one:
// the vectors stay caller memory the kernel chases per segment, while
// the name travels inline. Control data has no v0 meaning.
static int copy_message_in(struct posix_msghdr *request,
                           const struct msghdr *message) {
    if (!message->msg_iov || message->msg_iovlen < 1 ||
        message->msg_iovlen > (int)POSIX_MSG_IOV_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (message->msg_control && message->msg_controllen) {
        errno = EOPNOTSUPP;
        return -1;
    }
    request->iov_count = (u32)message->msg_iovlen;
    for (u32 index = 0; index < request->iov_count; index++) {
        if (!message->msg_iov[index].iov_base &&
            message->msg_iov[index].iov_len) {
            errno = EINVAL;
            return -1;
        }
        request->iov[index].base = (uptr_t)message->msg_iov[index].iov_base;
        request->iov[index].length = (u32)message->msg_iov[index].iov_len;
        request->iov[index].reserved = 0;
    }
    if (message->msg_name) {
        if (copy_address_in(&request->address,
                            (const struct sockaddr *)message->msg_name,
                            message->msg_namelen))
            return -1;
        request->address_length = sizeof(request->address);
    }
    return 0;
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    if (check_socket_flags(flags)) return -1;
    if (!message) {
        errno = EINVAL;
        return -1;
    }
    struct posix_msghdr request;
    zero_request(&request, sizeof(request));
    if (copy_message_in(&request, message)) return -1;
    request.descriptor = fd;
    return result_int(request_call(POSIX_SYSCALL_SENDMSG, &request));
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    if (check_socket_flags(flags)) return -1;
    if (!message) {
        errno = EINVAL;
        return -1;
    }
    struct posix_msghdr request;
    zero_request(&request, sizeof(request));
    if (copy_message_in(&request, message)) return -1;
    request.descriptor = fd;
    long result = request_call(POSIX_SYSCALL_RECVMSG, &request);
    if (result < 0) return result_int(result);
    // The kernel scattered the payload straight into the caller's
    // vectors; the name and the flags ride back in the request copy.
    if (message->msg_name && message->msg_namelen) {
        for (u32 index = 0; index < sizeof(request.address) &&
             index < message->msg_namelen; index++)
            ((u8 *)message->msg_name)[index] =
                ((u8 *)&request.address)[index];
        message->msg_namelen = request.address_length ?
            (socklen_t)sizeof(struct sockaddr_in) : 0;
    }
    message->msg_flags = (int)request.flags;
    return (ssize_t)result;
}

int shutdown(int fd, int how) {
    struct posix_socket_shutdown_request request;
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    request.how = (u32)how;
    return result_int(request_call(POSIX_SYSCALL_SHUTDOWN, &request));
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *length) {
    if (!length) {
        errno = EINVAL;
        return -1;
    }
    struct posix_sockopt_request request;
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    request.level = (u32)level;
    request.name = (u32)name;
    if (*length > (socklen_t)sizeof(request.value)) {
        errno = EINVAL;
        return -1;
    }
    request.length = *length;
    long result = request_call(POSIX_SYSCALL_GETSOCKOPT, &request);
    if (result < 0) return result_int(result);
    if (value)
        for (u32 index = 0; index < request.length; index++)
            ((u8 *)value)[index] = request.value[index];
    *length = (socklen_t)request.length;
    return 0;
}

int setsockopt(int fd, int level, int name, const void *value,
               socklen_t length) {
    struct posix_sockopt_request request;
    if (!value || !length || length > (socklen_t)sizeof(request.value)) {
        errno = EINVAL;
        return -1;
    }
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    request.level = (u32)level;
    request.name = (u32)name;
    request.length = (u32)length;
    for (u32 index = 0; index < length; index++)
        request.value[index] = ((const u8 *)value)[index];
    return result_int(request_call(POSIX_SYSCALL_SETSOCKOPT, &request));
}

static int name_call(u32 number, int fd, struct sockaddr *address,
                     socklen_t *length) {
    if (!address || !length ||
        *length < (socklen_t)sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    struct posix_socket_address_request request;
    zero_request(&request, sizeof(request));
    request.descriptor = fd;
    request.length = sizeof(request.address);
    long result = request_call(number, &request);
    if (result < 0) return result_int(result);
    for (u32 index = 0; index < sizeof(request.address); index++)
        ((u8 *)address)[index] = ((u8 *)&request.address)[index];
    *length = (socklen_t)request.length;
    return 0;
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    return name_call(POSIX_SYSCALL_GETSOCKNAME, fd, address, length);
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
    return name_call(POSIX_SYSCALL_GETPEERNAME, fd, address, length);
}

// The byte order pair: the machine is little endian, so the network
// order the wire and the kernel addresses carry is the swap of these.
in_port_t htons(in_port_t value) {
    return (in_port_t)(((value & 0xFFu) << 8) | ((value >> 8) & 0xFFu));
}

in_port_t ntohs(in_port_t value) {
    return htons(value);
}

in_addr_t htonl(in_addr_t value) {
    return ((value & 0xFFu) << 24) | ((value & 0xFF00u) << 8) |
        ((value >> 8) & 0xFF00u) | ((value >> 24) & 0xFFu);
}

in_addr_t ntohl(in_addr_t value) {
    return htonl(value);
}
