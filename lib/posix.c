#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <mich/syscall.h>
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
    /* The dispatcher rejects a request whose last path byte is not zero,
       so the whole tail past the string must be cleared, not just the
       terminator: leftover stack bytes would read as a non canonical path. */
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
    /* The system call keeps the POSIX contract: the buffer gets the raw
       bytes, no terminator, and a smaller caller buffer truncates while
       the return keeps the count actually placed. */
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
    if (result_int(request_call(POSIX_SYSCALL_NANOSLEEP, &request))) return -1;
    /* Nothing interrupts the park yet, so the kernel always reports a zero
       remainder; the copy keeps the shape signals will need. */
    if (remaining) {
        remaining->tv_sec = (time_t)request.remaining_sec;
        remaining->tv_nsec = (long)request.remaining_nsec;
    }
    return 0;
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
    /* The profile carries no timezone table, so the second pointer stays
       NULL rather than silently reading garbage. */
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
