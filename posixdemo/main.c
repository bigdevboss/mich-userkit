#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <mich/syscall.h>

// First compatibility-claim fixture (profile step 6): a real static POSIX
// application launched by init64 through fork/execve. The parent role
// exercises the v0 core API surface; the child role is reached by the
// application exec'ing itself with its own argv/envp. Contract: entry table
// empty, cwd "/", parent passes {"posixdemo","demo"}, {"POSIXDEMO=stage6"}.

static int string_equals(const char *left, const char *right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static int wait_for_go(int sync_fd) {
    for (unsigned long attempt = 0; attempt < 100000u; attempt++) {
        char flag = 0;
        if (lseek(sync_fd, 0, 0) != 0) return -1;
        if (read(sync_fd, &flag, 1) != 1) return -1;
        if (flag == 'g') return 0;
        mich_yield();
    }
    return -1;
}

static int io_demo(void) {
    static const char payload[] = "mich static posix demo";
    char first[12];
    char second[12];
    int fd = open("/posixdemo-io", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd != 0) return -1;
    if (write(fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    // dup shares the open-file description, so the alias continues at the
    // shared offset instead of restarting the file.
    int alias = dup(fd);
    if (alias != 1) return -1;
    if (lseek(fd, 0, 0) != 0) return -1;
    if (read(fd, first, sizeof(payload) / 2) != (ssize_t)(sizeof(payload) / 2))
        return -1;
    if (read(alias, second, sizeof(payload) / 2) !=
        (ssize_t)(sizeof(payload) / 2))
        return -1;
    first[sizeof(payload) / 2] = 0;
    second[sizeof(payload) / 2] = 0;
    struct stat info;
    if (!string_equals(first, "mich static") ||
        !string_equals(second, " posix demo"))
        return -1;
    if (fstat(alias, &info) || !S_ISREG(info.st_mode) ||
        (info.st_mode & 0777u) != 0600u ||
        info.st_size != (off_t)(sizeof(payload) - 1))
        return -1;
    // access mirrors the permission picture the descriptors already
    // proved: 0600 opens read and write for the owner and keeps the
    // execute question closed.
    if (access("/posixdemo-io", R_OK | W_OK)) return -1;
    errno = 0;
    if (access("/posixdemo-io", X_OK) != -1 || errno != EACCES) return -1;
    errno = 0;
    if (access("/posixdemo-missing", F_OK) != -1 || errno != ENOENT)
        return -1;



    // The positioned calls address an offset without touching the shared
    // cursor, and a truncate extension reads back as zeroes.
    if (pwrite(fd, "ZZ", 2, 8) != 2) return -1;
    if (lseek(alias, 0, 1) != (off_t)(sizeof(payload) - 1)) return -1;
    if (pread(fd, first, 2, 8) != 2 || first[0] != 'Z' || first[1] != 'Z')
        return -1;
    if (lseek(alias, 0, 1) != (off_t)(sizeof(payload) - 1)) return -1;
    if (ftruncate(fd, 32)) return -1;
    if (fstat(fd, &info) || info.st_size != 32) return -1;
    if (pread(fd, first, 1, 31) != 1 || first[0]) return -1;
    if (ftruncate(fd, (off_t)(sizeof(payload) - 1))) return -1;
    // A durability call rides the format staging path and has to leave
    // the file readable through the plain cursor afterwards.
    if (fsync(fd) || fdatasync(alias)) return -1;
    if (pread(fd, first, 2, 8) != 2 || first[0] != 'Z' || first[1] != 'Z')
        return -1;
    if (close(alias) || close(fd)) return -1;
    return unlink("/posixdemo-io") ? -1 : 0;
}

static int link_demo(void) {
    static const char payload[] = "mich link demo";
    char buffer[16];
    int fd = open("/posixdemo-link", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (write(fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    if (close(fd)) return -1;
    if (link("/posixdemo-link", "/posixdemo-twin")) return -1;
    errno = 0;
    if (link("/posixdemo-link", "/posixdemo-twin") != -1 || errno != EEXIST)
        return -1;
    struct stat info;
    if (stat("/posixdemo-twin", &info) || info.st_nlink != 2) return -1;
    if (unlink("/posixdemo-link")) return -1;
    if (stat("/posixdemo-twin", &info) || info.st_nlink != 1) return -1;
    // The surviving name still reads the bytes both names shared.
    int twin = open("/posixdemo-twin", O_RDONLY);
    if (twin < 0) return -1;
    if (read(twin, buffer, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    buffer[sizeof(payload) - 1] = 0;
    if (close(twin)) return -1;
    if (!string_equals(buffer, payload)) return -1;
    return unlink("/posixdemo-twin") ? -1 : 0;
}

static int rename_demo(void) {
    static const char payload[] = "mich rename demo";
    char buffer[24];
    struct stat info;
    if (mkdir("/posixdemo-move", 0700)) return -1;
    int fd = open("/posixdemo-rename", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (write(fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    if (close(fd)) return -1;
    // Moving the name across directories keeps the bytes and drops the
    // old name; renaming a name onto itself changes nothing.
    if (rename("/posixdemo-rename", "/posixdemo-move/landed")) return -1;
    if (rename("/posixdemo-move/landed", "/posixdemo-move/landed")) return -1;
    errno = 0;
    if (stat("/posixdemo-rename", &info) != -1 || errno != ENOENT)
        return -1;
    if (stat("/posixdemo-move/landed", &info) ||
        info.st_nlink != 1 || info.st_size != (off_t)(sizeof(payload) - 1))
        return -1;
    // A same-kind target leaves quietly and the moved name takes its
    // place.
    fd = open("/posixdemo-victim", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (write(fd, "victim", 6) != 6) return -1;
    if (close(fd)) return -1;
    if (rename("/posixdemo-move/landed", "/posixdemo-victim")) return -1;
    errno = 0;
    if (stat("/posixdemo-move/landed", &info) != -1 || errno != ENOENT)
        return -1;
    fd = open("/posixdemo-victim", O_RDONLY);
    if (fd < 0) return -1;
    if (read(fd, buffer, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    buffer[sizeof(payload) - 1] = 0;
    if (close(fd)) return -1;
    if (!string_equals(buffer, payload)) return -1;
    // The replace matrix and the cross-filesystem refusal carry their
    // own errno values.
    errno = 0;
    if (rename("/posixdemo-victim", "/posixdemo-move") != -1 ||
        errno != EISDIR)
        return -1;
    errno = 0;
    if (rename("/posixdemo-move", "/posixdemo-victim") != -1 ||
        errno != ENOTDIR)
        return -1;
    // ENOTEMPTY belongs to the replaced target: a full source directory
    // onto an empty one is a legal move.
    if (mkdir("/posixdemo-move/inner", 0700)) return -1;
    if (mkdir("/posixdemo-full", 0700)) return -1;
    fd = open("/posixdemo-full/occupant", O_WRONLY | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (close(fd)) return -1;
    errno = 0;
    if (rename("/posixdemo-move", "/posixdemo-full") != -1 ||
        errno != ENOTEMPTY)
        return -1;
    errno = 0;
    if (rename("/posixdemo-victim", "/boot/posixdemo-cross") != -1 ||
        errno != EXDEV)
        return -1;
    if (unlink("/posixdemo-full/occupant") ||
        rmdir("/posixdemo-move/inner") || rmdir("/posixdemo-move") ||
        rmdir("/posixdemo-full") || unlink("/posixdemo-victim"))
        return -1;
    return 0;
}

static int symlink_demo(void) {
    static const char payload[] = "mich symlink demo";
    char buffer[24];
    char target[32];
    struct stat info;
    int fd = open("/posixdemo-real", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (write(fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    if (close(fd)) return -1;
    if (symlink("/posixdemo-real", "/posixdemo-link")) return -1;
    errno = 0;
    if (symlink("/posixdemo-real", "/posixdemo-link") != -1 ||
        errno != EEXIST)
        return -1;
    // stat follows the link, lstat does not, and readlink carries the
    // target string without a terminator.
    if (stat("/posixdemo-link", &info) || !S_ISREG(info.st_mode) ||
        info.st_size != (off_t)(sizeof(payload) - 1))
        return -1;
    if (lstat("/posixdemo-link", &info) || !S_ISLNK(info.st_mode) ||
        info.st_size != (off_t)15)
        return -1;
    int got = readlink("/posixdemo-link", target, sizeof(target));
    if (got != 15) return -1;
    if (target[0] != '/' || target[1] != 'p' || target[14] != 'l')
        return -1;
    // Opening the link reaches the file behind it.
    fd = open("/posixdemo-link", O_RDONLY);
    if (fd < 0) return -1;
    if (read(fd, buffer, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    buffer[sizeof(payload) - 1] = 0;
    if (close(fd)) return -1;
    if (!string_equals(buffer, payload)) return -1;
    // A dangling link reads back but resolves to nothing, and two links
    // pointing at each other never resolve at all.
    if (symlink("/posixdemo-missing", "/posixdemo-dangling")) return -1;
    errno = 0;
    if (stat("/posixdemo-dangling", &info) != -1 || errno != ENOENT)
        return -1;
    got = readlink("/posixdemo-dangling", target, sizeof(target));
    if (got != 18) return -1;
    if (symlink("/posixdemo-loop-b", "/posixdemo-loop-a")) return -1;
    if (symlink("/posixdemo-loop-a", "/posixdemo-loop-b")) return -1;
    errno = 0;
    if (stat("/posixdemo-loop-a", &info) != -1 || errno != ELOOP)
        return -1;
    if (unlink("/posixdemo-link") || unlink("/posixdemo-dangling") ||
        unlink("/posixdemo-loop-a") || unlink("/posixdemo-loop-b") ||
        unlink("/posixdemo-real"))
        return -1;
    return 0;
}

static int times_demo(void) {
    struct stat info;
    struct timespec chosen[2];
    int fd = open("/posixdemo-times", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    // Chosen pairs land whole: seconds and nanoseconds both survive the
    // round trip through the path call.
    chosen[0].tv_sec = 1000000000;
    chosen[0].tv_nsec = 123456789;
    chosen[1].tv_sec = 1000000001;
    chosen[1].tv_nsec = 987654321;
    if (utimensat(AT_FDCWD, "/posixdemo-times", chosen, 0)) return -1;
    if (stat("/posixdemo-times", &info) ||
        info.st_atime != 1000000000 || info.st_atime_nsec != 123456789 ||
        info.st_mtime != 1000000001 || info.st_mtime_nsec != 987654321)
        return -1;
    // OMIT leaves one half standing while the other moves.
    chosen[0].tv_nsec = UTIME_OMIT;
    chosen[1].tv_sec = 2000000000;
    chosen[1].tv_nsec = 1;
    if (utimensat(AT_FDCWD, "/posixdemo-times", chosen, 0)) return -1;
    if (stat("/posixdemo-times", &info) ||
        info.st_atime != 1000000000 || info.st_atime_nsec != 123456789 ||
        info.st_mtime != 2000000000 || info.st_mtime_nsec != 1)
        return -1;
    // A NULL pair is the classic utime request, and the descriptor twin
    // carries it without a path at all. The wall clock answers with real
    // seconds, so NOW lands anywhere at or past the last chosen pair.
    if (futimens(fd, 0)) return -1;
    if (fstat(fd, &info) || info.st_mtime < 1000000001 ||
        info.st_mtime_nsec > 999999999)
        return -1;
    // A nanosecond half outside [0, 999999999] is EINVAL, and so is a
    // dirfd or flag the wrapper does not carry.
    errno = 0;
    chosen[0].tv_sec = 0;
    chosen[0].tv_nsec = 1000000000;
    chosen[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, "/posixdemo-times", chosen, 0) != -1 ||
        errno != EINVAL)
        return -1;
    errno = 0;
    if (utimensat(3, "/posixdemo-times", 0, 0) != -1 || errno != EINVAL)
        return -1;
    if (close(fd)) return -1;
    return unlink("/posixdemo-times");
}

static int realpath_demo(void) {
    char path[PATH_MAX];
    char long_path[320];
    if (mkdir("/posixdemo-canonical", 0755)) return -1;
    int fd = open("/posixdemo-canonical/target.txt", O_WRONLY | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (close(fd)) return -1;
    // Repeated slashes, "." and ".." fold into the canonical form, and the
    // root is its own parent.
    if (!realpath("/posixdemo-canonical//./../posixdemo-canonical/./target.txt",
                  path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (!realpath("/posixdemo-canonical/..", path) ||
        !string_equals(path, "/"))
        return -1;
    // A relative path starts from the working directory.
    if (chdir("/posixdemo-canonical")) return -1;
    if (!realpath("target.txt", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (!realpath("../posixdemo-canonical/target.txt", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (chdir("/")) return -1;
    // Links expand: an absolute target restarts the walk at the root, a
    // relative one continues from the link's directory.
    if (symlink("/posixdemo-canonical/target.txt", "/posixdemo-canonical/absolute"))
        return -1;
    if (!realpath("/posixdemo-canonical/absolute", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (symlink("target.txt", "/posixdemo-canonical/relative")) return -1;
    if (!realpath("/posixdemo-canonical/relative", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    // A NULL buffer makes the call allocate the answer itself.
    char *heap = realpath("/posixdemo-canonical/target.txt", 0);
    if (!heap || !string_equals(heap, "/posixdemo-canonical/target.txt"))
        return -1;
    free(heap);
    // The error surface: a missing component, a non-directory in the
    // middle, a link loop, an empty path, and a result that cannot fit.
    errno = 0;
    if (realpath("/posixdemo-canonical/missing", path) || errno != ENOENT)
        return -1;
    errno = 0;
    if (realpath("/posixdemo-canonical/target.txt/inside", path) ||
        errno != ENOTDIR)
        return -1;
    if (symlink("loop-b", "/posixdemo-canonical/loop-a")) return -1;
    if (symlink("loop-a", "/posixdemo-canonical/loop-b")) return -1;
    errno = 0;
    if (realpath("/posixdemo-canonical/loop-a", path) || errno != ELOOP)
        return -1;
    errno = 0;
    if (realpath("", path) || errno != ENOENT) return -1;
    long_path[0] = '/';
    for (int index = 1; index < 300; index++) long_path[index] = 'x';
    long_path[300] = 0;
    errno = 0;
    if (realpath(long_path, path) || errno != ENAMETOOLONG) return -1;
    if (unlink("/posixdemo-canonical/absolute") ||
        unlink("/posixdemo-canonical/relative") ||
        unlink("/posixdemo-canonical/loop-a") ||
        unlink("/posixdemo-canonical/loop-b") ||
        unlink("/posixdemo-canonical/target.txt"))
        return -1;
    return rmdir("/posixdemo-canonical");
}

static int cwd_demo(void) {
    char buffer[32];
    if (mkdir("/posixdemo-dir", 0700)) return -1;
    if (chdir("/posixdemo-dir")) return -1;
    if (!getcwd(buffer, sizeof(buffer)) ||
        !string_equals(buffer, "/posixdemo-dir"))
        return -1;
    int fd = open("relative.txt", O_WRONLY | O_CREAT, 0600);
    if (fd != 0) return -1;
    if (write(fd, "relative", 8) != 8) return -1;
    if (close(fd)) return -1;
    if (chdir("/")) return -1;
    struct stat info;
    if (stat("/posixdemo-dir/relative.txt", &info) || !S_ISREG(info.st_mode))
        return -1;
    if (unlink("/posixdemo-dir/relative.txt")) return -1;
    return rmdir("/posixdemo-dir") ? -1 : 0;
}

static int errno_demo(void) {
    char buffer[4];
    int status = -1;
    errno = 0;
    if (open("/posixdemo-absent", O_RDONLY) != -1 || errno != ENOENT)
        return -1;
    errno = 0;
    if (open("/", O_WRONLY) != -1 || errno != EISDIR) return -1;
    // Directories open read-only so they can be listed; drain the root and
    // expect exactly the boot and dev directories the system mounts.
    int root_fd = open("/", O_RDONLY);
    if (root_fd < 0) return -1;
    struct dirent listing;
    int seen_boot = 0;
    int seen_dev = 0;
    int seen_other = 0;
    for (;;) {
        int listed = getdents(root_fd, &listing, sizeof(listing));
        if (listed < 0) return -1;
        if (!listed) break;
        int offset = 0;
        while (offset < listed) {
            struct dirent *record =
                (struct dirent *)(void *)((unsigned char *)&listing + offset);
            if (record->d_reclen < 26 || (record->d_reclen & 7)) return -1;
            if (record->d_name[0] == 'b' && record->d_name[1] == 'o' &&
                record->d_name[2] == 'o' && record->d_name[3] == 't' &&
                !record->d_name[4] && record->d_type == DT_DIR) {
                seen_boot++;
            } else if (record->d_name[0] == 'd' && record->d_name[1] == 'e' &&
                       record->d_name[2] == 'v' && !record->d_name[3] &&
                       record->d_type == DT_DIR) {
                seen_dev++;
            } else {
                seen_other++;
            }
            offset += record->d_reclen;
        }
    }
    if (close(root_fd)) return -1;
    if (!seen_boot || !seen_dev || seen_other) return -1;
    // Ownership and mode edits: chmod locks and unlocks a file, chown hands
    // it to another owner and takes it back, and the umask shapes a create.
    int perms_fd = open("/posixdemo-perms", O_RDWR | O_CREAT, 0600);
    if (perms_fd < 0) return -1;
    if (fchmod(perms_fd, 0)) return -1;
    errno = 0;
    if (open("/posixdemo-perms", O_RDONLY) != -1 || errno != EACCES)
        return -1;
    if (chmod("/posixdemo-perms", 0400)) return -1;
    if (chown("/posixdemo-perms", 1, 1)) return -1;
    errno = 0;
    if (chmod("/posixdemo-perms", 0600) != -1 || errno != EPERM) return -1;
    if (chown("/posixdemo-perms", 0, 0)) return -1;
    if (chmod("/posixdemo-perms", 0600)) return -1;
    if (close(perms_fd)) return -1;
    if (unlink("/posixdemo-perms")) return -1;
    if (umask(0) != 022) return -1;
    int raw_fd = open("/posixdemo-raw", O_RDWR | O_CREAT, 0666);
    if (raw_fd < 0) return -1;
    struct stat raw_info;
    if (fstat(raw_fd, &raw_info) || (raw_info.st_mode & 0777u) != 0666u)
        return -1;
    if (close(raw_fd) || unlink("/posixdemo-raw")) return -1;
    umask(022);
    errno = 0;
    if (read(9999, buffer, sizeof(buffer)) != -1 || errno != EBADF)
        return -1;
    errno = 0;
    if (waitpid(9999, &status, 0) != -1 || errno != ECHILD) return -1;
    return 0;
}

static int string_demo(void) {
    char scratch[12];
    if (memset(scratch, 'a', sizeof(scratch)) != scratch) return -1;
    if (memcmp(scratch, "aaaaaaaaaaaa", sizeof(scratch))) return -1;
    memcpy(scratch, "0123456789A", sizeof(scratch));
    if (memcmp(scratch, "0123456789A", sizeof(scratch))) return -1;
    // Overlapping ranges: memmove must behave as if copied through a
    // temporary buffer, unlike a plain forward memcpy.
    memmove(scratch + 1, scratch, 5);
    if (memcmp(scratch, "0012346789A", 11)) return -1;
    if (strlen("posixdemo") != 9) return -1;
    if (strcmp("abc", "abc") || strcmp("abc", "abd") >= 0 ||
        strcmp("b", "a") <= 0)
        return -1;
    if (strncmp("michabc", "michxyz", 4) ||
        !strncmp("michabc", "michxyz", 5))
        return -1;
    const char *hit = strchr("aXbXc", 'X');
    if (!hit || hit[0] != 'X' || hit[1] != 'b') return -1;
    if (strchr("abc", 'Z')) return -1;
    const char *last = strrchr("aXbXc", 'X');
    if (!last || last[1] != 'c') return -1;
    return 0;
}

static int stdio_demo(void) {
    char buffer[48];
    if (snprintf(buffer, sizeof(buffer), "mich %d", 42) != 7) return -1;
    if (strcmp(buffer, "mich 42")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%08X|%-5d|%x", 48879, -7, 48879) != 19)
        return -1;
    if (strcmp(buffer, "0000BEEF|-7   |beef")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%s/%c", "boot", 'p') != 6) return -1;
    if (strcmp(buffer, "boot/p")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%p", (void *)0x1000) != 6) return -1;
    if (strcmp(buffer, "0x1000")) return -1;
    // POSIX truncation contract: the full length is returned while the
    // buffer stays bounded and nul-terminated.
    if (snprintf(buffer, 4, "%s", "abcdefg") != 7) return -1;
    if (strcmp(buffer, "abc")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%lu%%", 1000UL) != 5) return -1;
    if (strcmp(buffer, "1000%")) return -1;
    // printf itself must stream through the bounded serial flush path.
    if (printf("libc printf alive: %d %s 0x%x\n", 42, "mich", 48879) != 34)
        return -1;
    return 0;
}

static int heap_demo(void) {
    unsigned long base =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (!base) return -1;
    // Neighbor coalescing while the arena is still empty: two freed
    // adjacent blocks must serve one larger request without growth.
    unsigned char *first = malloc(64);
    unsigned char *second = malloc(64);
    unsigned char *third = malloc(64);
    if (!first || !second || !third) return -1;
    if (second - first != third - second) return -1;
    free(second);
    free(first);
    unsigned char *merged = malloc(120);
    if (!merged || merged != first) return -1;
    free(third);
    free(merged);
    // A large request crosses page boundaries through the break syscall.
    unsigned char *data = malloc(70000);
    if (!data) return -1;
    for (int index = 0; index < 70000; index++) data[index] = (unsigned char)index;
    for (int index = 0; index < 70000; index++)
        if (data[index] != (unsigned char)index) return -1;
    unsigned long grown =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (grown <= base) return -1;
    // Freed space is reused instead of growing the arena again.
    free(data);
    unsigned char *reused = malloc(70000);
    if (!reused) return -1;
    if ((unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0) != grown) return -1;
    // A double free must not merge the same block twice.
    free(reused);
    free(reused);
    unsigned char *alive = malloc(32);
    if (!alive) return -1;
    alive[0] = 1;
    if (alive[0] != 1) return -1;
    unsigned char *zeroed = calloc(16, 16);
    if (!zeroed) return -1;
    for (int index = 0; index < 256; index++)
        if (zeroed[index]) return -1;
    // realloc preserves the old contents across a move.
    unsigned char *small = malloc(16);
    if (!small) return -1;
    for (int index = 0; index < 16; index++) small[index] = (unsigned char)(index + 1);
    unsigned char *expanded = realloc(small, 200);
    if (!expanded) return -1;
    for (int index = 0; index < 16; index++)
        if (expanded[index] != (unsigned char)(index + 1)) return -1;
    free(expanded);
    free(zeroed);
    free(alive);
    // The heap window is bounded, so an oversized request reports ENOMEM.
    errno = 0;
    if (malloc(32 * 1024 * 1024)) return -1;
    if (errno != ENOMEM) return -1;
    // The break is grow-only: requesting the base back changes nothing.
    if ((unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, base) != grown) return -1;
    return 0;
}

static int file_demo(void) {
    char text[64];
    unsigned char block[256];
    unsigned char mirror[256];
    for (int index = 0; index < 256; index++)
        block[index] = (unsigned char)(index * 7);
    FILE *file = fopen("/libc-file", "w");
    if (!file) return -1;
    if (fputs("mich libc file stream\n", file) == EOF) return -1;
    if (fprintf(file, "%s=%d hex=%04X\n", "value", 42, 48879) < 0) return -1;
    if (fputc('Z', file) == EOF) return -1;
    // 700 pattern bytes force the 512-byte buffer through a mid-stream
    // flush while the file stays under the 4 KiB VFS cap.
    if (fwrite(block, 1, 256, file) != 256) return -1;
    if (fwrite(block, 1, 256, file) != 256) return -1;
    if (fwrite(block, 1, 188, file) != 188) return -1;
    // ftell must account for bytes still staged in the buffer.
    if (ftell(file) != 741) return -1;
    if (fclose(file)) return -1;
    file = fopen("/libc-file", "r");
    if (!file) return -1;
    if (ftell(file) != 0) return -1;
    if (!fgets(text, sizeof(text), file)) return -1;
    if (!string_equals(text, "mich libc file stream\n")) return -1;
    if (!fgets(text, sizeof(text), file)) return -1;
    if (!string_equals(text, "value=42 hex=BEEF\n")) return -1;
    if (fseek(file, 40, SEEK_SET)) return -1;
    if (fgetc(file) != 'Z') return -1;
    if (ftell(file) != 41) return -1;
    rewind(file);
    if (ftell(file) != 0) return -1;
    if (fseek(file, -700, SEEK_END)) return -1;
    if (ftell(file) != 41) return -1;
    for (int pass = 0; pass < 2; pass++) {
        if (fread(mirror, 1, 256, file) != 256) return -1;
        for (int index = 0; index < 256; index++)
            if (mirror[index] != block[index]) return -1;
    }
    if (fread(mirror, 1, 188, file) != 188) return -1;
    for (int index = 0; index < 188; index++)
        if (mirror[index] != block[index]) return -1;
    // The sticky EOF flag rises on the short read, not before it.
    if (fread(mirror, 1, 256, file) != 0) return -1;
    if (!feof(file)) return -1;
    if (fgetc(file) != EOF) return -1;
    if (fclose(file)) return -1;
    file = fopen("/libc-file", "a");
    if (!file) return -1;
    if (fputs("tail", file) == EOF) return -1;
    if (fclose(file)) return -1;
    file = fopen("/libc-file", "r");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END)) return -1;
    if (ftell(file) != 745) return -1;
    if (fseek(file, -4, SEEK_END)) return -1;
    if (!fgets(text, sizeof(text), file)) return -1;
    if (!string_equals(text, "tail")) return -1;
    // Writing into a read stream fails and sets the error flag without
    // disturbing the pending reads.
    if (fwrite("x", 1, 1, file) != 0) return -1;
    if (!ferror(file)) return -1;
    clearerr(file);
    if (ferror(file)) return -1;
    if (fclose(file)) return -1;
    return unlink("/libc-file") ? -1 : 0;
}

static int entropy_demo(void) {
    unsigned char first[256];
    unsigned char second[256];
    if (getrandom(first, sizeof(first), 0) != (ssize_t)sizeof(first)) return -1;
    // A stuck or absent generator must not look like all-zero or all-one
    // output, and the bit balance must sit near one half.
    unsigned int set_bits = 0;
    unsigned int same_zero = 1;
    unsigned int same_one = 1;
    for (int index = 0; index < 256; index++) {
        if (first[index]) same_zero = 0;
        if (first[index] != 0xFF) same_one = 0;
        for (int bit = 0; bit < 8; bit++)
            if (first[index] & (1u << bit)) set_bits++;
    }
    if (same_zero || same_one) return -1;
    if (set_bits < 896 || set_bits > 1152) return -1;
    if (getrandom(second, sizeof(second), 0) != (ssize_t)sizeof(second)) return -1;
    if (!memcmp(first, second, sizeof(first))) return -1;
    // /dev/urandom rides the same DRBG through the file facade.
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    unsigned char third[256];
    if (read(fd, third, sizeof(third)) != (ssize_t)sizeof(third)) return -1;
    if (read(fd, first, 64) != 64) return -1;
    if (!memcmp(third, second, sizeof(third))) return -1;
    if (close(fd)) return -1;
    // Reserved flags and zero length are rejected with EINVAL.
    errno = 0;
    if (getrandom(second, 16, 1) != -1 || errno != EINVAL) return -1;
    errno = 0;
    if (getrandom(second, 0, 0) != -1 || errno != EINVAL) return -1;
    return 0;
}

static int time_demo(void) {
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution) ||
        resolution.tv_sec || resolution.tv_nsec != 10000000L)
        return -1;
    if (clock_getres(CLOCK_REALTIME, &resolution) ||
        resolution.tv_sec || resolution.tv_nsec != 10000000L)
        return -1;
    struct timespec before;
    if (clock_gettime(CLOCK_MONOTONIC, &before)) return -1;
    struct timespec interval;
    interval.tv_sec = 0;
    interval.tv_nsec = 20000000L;
    struct timespec remaining;
    if (nanosleep(&interval, &remaining) || remaining.tv_sec ||
        remaining.tv_nsec)
        return -1;
    struct timespec after;
    if (clock_gettime(CLOCK_MONOTONIC, &after)) return -1;
    // The kernel rounds up to whole ticks, so the slept span is never
    // shorter than the requested twenty milliseconds.
    long slept = (long)(after.tv_sec - before.tv_sec) * 1000000000L +
        (after.tv_nsec - before.tv_nsec);
    if (slept < 20000000L) return -1;
    // Monotonic never runs backwards across the sleep.
    if (after.tv_sec < before.tv_sec) return -1;
    struct timespec wall;
    if (clock_gettime(CLOCK_REALTIME, &wall) || wall.tv_sec < 1790240000LL ||
        wall.tv_nsec < 0 || wall.tv_nsec > 999999999L)
        return -1;
    // time and gettimeofday draw on the same wall clock, so the two
    // readings stay inside a one second window.
    time_t stamp = time(0);
    struct timeval day;
    if (gettimeofday(&day, 0) || day.tv_sec < stamp - 1 ||
        day.tv_sec > stamp + 1 || day.tv_usec < 0 || day.tv_usec > 999999L)
        return -1;
    if (gettimeofday(&day, (void *)1) != -1 || errno != EINVAL) return -1;
    errno = 0;
    if (clock_gettime(7, &before) != -1 || errno != EINVAL) return -1;
    errno = 0;
    if (clock_getres(7, &before) != -1 || errno != EINVAL) return -1;
    struct timespec bogus = { -1, 0 };
    errno = 0;
    if (nanosleep(&bogus, 0) != -1 || errno != EINVAL) return -1;
    bogus.tv_sec = 0;
    bogus.tv_nsec = -1;
    errno = 0;
    if (nanosleep(&bogus, 0) != -1 || errno != EINVAL) return -1;
    bogus.tv_nsec = 1000000000L;
    errno = 0;
    if (nanosleep(&bogus, 0) != -1 || errno != EINVAL) return -1;
    // A zero interval is a scheduling point, not a park.
    bogus.tv_nsec = 0;
    if (nanosleep(&bogus, 0)) return -1;
    return 0;
}

static volatile sig_atomic_t signal_note;

static void note_signal(int signo) {
    signal_note = signo;
}

static void exit_from_handler(int signo) {
    signal_note = signo;
    _exit(77);
}

static int signal_demo(void) {
    struct sigaction action;
    struct sigaction previous;

    // A caught signal posted to the caller itself is delivered on the
    // syscall exit, and the restorer path resumes the interrupted code.
    sigemptyset(&action.sa_mask);
    action.sa_handler = note_signal;
    action.sa_flags = 0;
    action.sa_restorer = 0;
    if (sigaction(SIGUSR1, &action, &previous)) return -1;
    signal_note = 0;
    if (raise(SIGUSR1)) return -1;
    if (signal_note != SIGUSR1) return -1;
    if (previous.sa_handler != SIG_DFL) return -1;

    // A blocked signal parks instead of delivering, sigpending shows it,
    // and opening the mask delivers on the unblock exit.
    sigset_t blocked;
    sigset_t pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &blocked, 0)) return -1;
    signal_note = 0;
    if (raise(SIGUSR1)) return -1;
    if (signal_note) return -1;
    if (sigpending(&pending) || !sigismember(&pending, SIGUSR1)) return -1;
    if (sigprocmask(SIG_UNBLOCK, &blocked, &pending)) return -1;
    // The delivery rides the unblock exit, and the answer carries the
    // mask as it was before: USR1 was part of it, so membership here is
    // the expected picture, not a leftover pending bit.
    if (signal_note != SIGUSR1) return -1;
    if (!sigismember(&pending, SIGUSR1)) return -1;

    // A user loop with no syscalls still reaches its handler through the
    // tick delivery: the child waits the parent into the loop first.
    signal_note = 0;
    int child = fork();
    if (child < 0) return -1;
    if (!child) {
        struct timespec pause;
        pause.tv_sec = 0;
        pause.tv_nsec = 100000000L;
        nanosleep(&pause, 0);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    volatile unsigned long spin = 0;
    while (!signal_note) spin++;
    if (signal_note != SIGUSR1) return -1;
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status))
        return -1;

    // An interrupted nanosleep answers EINTR with the time it still
    // owed, rounded to whole ticks.
    action.sa_handler = note_signal;
    if (sigaction(SIGUSR2, &action, 0)) return -1;
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        struct timespec pause;
        pause.tv_sec = 0;
        pause.tv_nsec = 100000000L;
        nanosleep(&pause, 0);
        kill(getppid(), SIGUSR2);
        _exit(0);
    }
    struct timespec interval;
    interval.tv_sec = 2;
    interval.tv_nsec = 0;
    struct timespec remaining;
    errno = 0;
    if (nanosleep(&interval, &remaining) != -1 || errno != EINTR)
        return -1;
    if (remaining.tv_sec < 1 || remaining.tv_sec > 1 ||
        remaining.tv_nsec > 999999999L) return -1;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status))
        return -1;

    // A hardware fault reaches its handler with the faulting frame: the
    // POSIX return repeats the instruction, so the handler leaves through
    // _exit rather than resuming into the same fault.
    action.sa_handler = exit_from_handler;
    if (sigaction(SIGSEGV, &action, 0)) return -1;
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        *(volatile unsigned long *)0 = 1;
        _exit(1);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 77)
        return -1;
    // The handler ran in the child's own address space after the fork, so
    // the parent cannot observe its signal_note write: the 77 exit byte is
    // the proof, only exit_from_handler produces it.

    // SIGKILL on a parked child surfaces as WIFSIGNALED with the real
    // termsig, not a folded exit byte.
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        struct timespec long_sleep;
        long_sleep.tv_sec = 5;
        long_sleep.tv_nsec = 0;
        nanosleep(&long_sleep, 0);
        _exit(0);
    }
    struct timespec settle;
    settle.tv_sec = 0;
    settle.tv_nsec = 100000000L;
    nanosleep(&settle, 0);
    if (kill(child, SIGKILL)) return -1;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGKILL)
        return -1;

    // An ignored SIGCHLD is the auto-reap contract: the child leaves no
    // zombie and waitpid answers ECHILD.
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGCHLD, &action, 0)) return -1;
    child = fork();
    if (child < 0) return -1;
    if (!child) _exit(50);
    nanosleep(&settle, 0);
    errno = 0;
    if (waitpid(child, &status, 0) != -1 || errno != ECHILD) return -1;
    action.sa_handler = SIG_DFL;
    if (sigaction(SIGCHLD, &action, 0)) return -1;

    // Rejections: an unknown signal, an untouchable one, a non positive
    // pid, and a pid no task answers for.
    errno = 0;
    if (sigaction(0, &action, 0) != -1 || errno != EINVAL) return -1;
    errno = 0;
    if (sigaction(32, &action, 0) != -1 || errno != EINVAL) return -1;
    errno = 0;
    if (sigaction(SIGKILL, &action, 0) != -1 || errno != EINVAL)
        return -1;
    errno = 0;
    if (kill(-1, SIGTERM) != -1 || errno != EINVAL) return -1;
    errno = 0;
    if (kill(9999, SIGTERM) != -1 || errno != ESRCH) return -1;
    errno = 0;
    if (sigprocmask(99, &blocked, 0) != -1 || errno != EINVAL) return -1;
    // Signal zero is the existence probe and reports success quietly.
    if (raise(0)) return -1;
    return 0;
}

static volatile sig_atomic_t socket_note;

static void note_socket_signal(int signo) {
    socket_note = signo;
}

static void fill_loopback_address(struct sockaddr_in *address,
                                  unsigned short port) {
    for (unsigned int index = 0; index < sizeof(*address); index++)
        ((unsigned char *)address)[index] = 0;
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static int socket_demo(void) {
    static unsigned char block[512];
    static unsigned char mirror[512];
    for (unsigned int index = 0; index < sizeof(block); index++)
        block[index] = (unsigned char)(index * 11u + 5u);

    // A same process round trip over loopback: both ends bind, the
    // sender's datagram lands in the receiver's queue, and the source
    // address names the sender.
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver < 0 || sender < 0) return -1;
    struct sockaddr_in receiver_address;
    struct sockaddr_in sender_address;
    fill_loopback_address(&receiver_address, 31050);
    fill_loopback_address(&sender_address, 31051);
    if (bind(receiver, (struct sockaddr *)&receiver_address,
             sizeof(receiver_address))) return -1;
    if (bind(sender, (struct sockaddr *)&sender_address,
             sizeof(sender_address))) return -1;

    struct sockaddr_in named;
    socklen_t named_length = sizeof(named);
    if (getsockname(receiver, (struct sockaddr *)&named, &named_length))
        return -1;
    if (named_length != sizeof(named) || named.sin_family != AF_INET ||
        ntohs(named.sin_port) != 31050 ||
        ntohl(named.sin_addr.s_addr) != INADDR_LOOPBACK) return -1;

    // The option rows the profile answers: the type and the domain read
    // back, and port reuse is stack policy that accepts the setting.
    int option_value = 0;
    socklen_t option_length = sizeof(option_value);
    if (getsockopt(receiver, SOL_SOCKET, SO_TYPE, &option_value,
                   &option_length)) return -1;
    if (option_value != (int)SOCK_DGRAM ||
        option_length != sizeof(option_value)) return -1;
    option_length = sizeof(option_value);
    if (getsockopt(receiver, SOL_SOCKET, SO_DOMAIN, &option_value,
                   &option_length)) return -1;
    if (option_value != (int)AF_INET) return -1;
    if (setsockopt(receiver, SOL_SOCKET, SO_REUSEADDR, &option_value,
                   sizeof(option_value))) return -1;

    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    if (sendto(sender, block, 96, 0, (struct sockaddr *)&receiver_address,
               sizeof(receiver_address)) != 96) return -1;
    if (recvfrom(receiver, mirror, sizeof(mirror), 0,
                 (struct sockaddr *)&source, &source_length) != 96)
        return -1;
    for (unsigned int index = 0; index < 96u; index++)
        if (mirror[index] != block[index]) return -1;
    if (source_length != sizeof(source) || source.sin_family != AF_INET ||
        ntohs(source.sin_port) != 31051 ||
        ntohl(source.sin_addr.s_addr) != INADDR_LOOPBACK) return -1;

    // The message calls scatter and gather the same trip: two vectors
    // out, two vectors back, the name riding the header.
    struct iovec out_vectors[2];
    out_vectors[0].iov_base = block;
    out_vectors[0].iov_len = 40;
    out_vectors[1].iov_base = block + 40;
    out_vectors[1].iov_len = 56;
    struct msghdr out_message;
    for (unsigned int index = 0; index < sizeof(out_message); index++)
        ((unsigned char *)&out_message)[index] = 0;
    out_message.msg_name = &receiver_address;
    out_message.msg_namelen = sizeof(receiver_address);
    out_message.msg_iov = out_vectors;
    out_message.msg_iovlen = 2;
    if (sendmsg(sender, &out_message, 0) != 96) return -1;
    struct iovec in_vectors[2];
    in_vectors[0].iov_base = mirror;
    in_vectors[0].iov_len = 48;
    in_vectors[1].iov_base = mirror + 48;
    in_vectors[1].iov_len = 48;
    struct msghdr in_message;
    for (unsigned int index = 0; index < sizeof(in_message); index++)
        ((unsigned char *)&in_message)[index] = 0;
    in_message.msg_name = &source;
    in_message.msg_namelen = sizeof(source);
    in_message.msg_iov = in_vectors;
    in_message.msg_iovlen = 2;
    if (recvmsg(receiver, &in_message, 0) != 96) return -1;
    for (unsigned int index = 0; index < 96u; index++)
        if (mirror[index] != block[index]) return -1;
    if (in_message.msg_namelen != sizeof(source) ||
        ntohs(source.sin_port) != 31051) return -1;

    // A datagram socket carries no peer name to answer.
    errno = 0;
    if (getpeername(receiver, (struct sockaddr *)&source, &source_length) !=
            -1 || errno != ENOTCONN) return -1;
    if (close(sender)) return -1;

    // Across fork the child binds its own end while the parent parks on
    // the empty queue; the child's sendto is the wake.
    pid_t child = fork();
    if (child < 0) return -1;
    if (!child) {
        struct sockaddr_in fresh_address;
        fill_loopback_address(&fresh_address, 31052);
        int fresh = socket(AF_INET, SOCK_DGRAM, 0);
        if (fresh < 0 || bind(fresh, (struct sockaddr *)&fresh_address,
                              sizeof(fresh_address))) _exit(68);
        if (sendto(fresh, block, 128, 0,
                   (struct sockaddr *)&receiver_address,
                   sizeof(receiver_address)) != 128) _exit(68);
        _exit(0);
    }
    if (recvfrom(receiver, mirror, sizeof(mirror), 0,
                 (struct sockaddr *)&source, &source_length) != 128)
        return -1;
    for (unsigned int index = 0; index < 128u; index++)
        if (mirror[index] != block[index]) return -1;
    if (source_length != sizeof(source) ||
        ntohs(source.sin_port) != 31052) return -1;
    int status = 0;
    if (waitpid(child, &status, 0) != child || status != 0) return -1;

    // A signal breaks the parked receive with EINTR; the handler note
    // proves the resumed context ran it first.
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_handler = note_socket_signal;
    action.sa_flags = 0;
    action.sa_restorer = 0;
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        sigemptyset(&action.sa_mask);
        action.sa_handler = note_socket_signal;
        action.sa_flags = 0;
        action.sa_restorer = 0;
        if (sigaction(SIGUSR1, &action, 0)) _exit(68);
        socket_note = 0;
        errno = 0;
        if (recvfrom(receiver, mirror, sizeof(mirror), 0,
                     (struct sockaddr *)&source, &source_length) != -1 ||
            errno != EINTR) _exit(68);
        if (socket_note != SIGUSR1) _exit(68);
        _exit(67);
    }
    struct timespec settle;
    settle.tv_sec = 0;
    settle.tv_nsec = 100 * 1000 * 1000;
    struct timespec left;
    nanosleep(&settle, &left);
    if (kill(child, SIGUSR1)) return -1;
    if (waitpid(child, &status, 0) != child) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 67) return -1;
    if (close(receiver)) return -1;
    return 0;
}

static int process_demo(void) {
    static const char payload[] = "posixdemo-child-payload";
    char *const child_argv[] = { "/posixdemo", "child", 0 };
    char *const child_envp[] = { "POSIXDEMO=child", 0 };
    int status = -1;
    // Child contract: 0 sync flag, 1 payload, 2 CLOEXEC victim.
    int sync_fd = open("/posixdemo-sync", O_RDWR | O_CREAT, 0600);
    int data_fd = open("/posixdemo-data", O_RDWR | O_CREAT, 0600);
    int gone_fd = open("/posixdemo-gone", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (sync_fd != 0 || data_fd != 1 || gone_fd != 2) return -1;
    if (write(sync_fd, "w", 1) != 1 ||
        write(data_fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    int child = fork();
    if (child < 0) return -1;
    if (!child) {
        execve("/boot/posixdemo", child_argv, child_envp);
        _exit(9);
    }
    // The child parks on the sync flag, so the WNOHANG poll cannot race.
    if (waitpid(child, &status, WNOHANG) != 0) return -1;
    if (lseek(sync_fd, 0, 0) != 0 || write(sync_fd, "g", 1) != 1) return -1;
    if (waitpid(child, &status, 0) != child) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) return -1;
    if (close(sync_fd) || close(data_fd) || close(gone_fd)) return -1;
    if (unlink("/posixdemo-sync") || unlink("/posixdemo-data") ||
        unlink("/posixdemo-gone"))
        return -1;
    return 0;
}

static volatile sig_atomic_t pledge_note;

static void note_pledge_abrt(int signo) {
    pledge_note = signo;
}

// The child of the pledge demo runs after execve inside the exec
// promises: "stdio" and nothing else, under the veil the parent left.
// Its one act is a fork, which the narrowed set forbids: the denial is
// loud, the default SIGABRT disposition takes the child down, and the
// parent reads the termsig through waitpid. If the exec promises had
// not applied, the inherited set still carried "proc" and the fork
// would have succeeded.
static int sandbox_role(void) {
    struct stat st;
    if (stat("/boot/posixdemo", &st)) return 70;
    errno = 0;
    if (stat("/pledge-out.txt", &st) != -1 || errno != ENOENT) return 71;
    pid_t pid = fork();
    // Reaching either return means the gate let the fork through.
    if (pid >= 0) return 72;
    (void)pid;
    return 73;
}

static int pledge_demo(void) {
    struct stat st;

    // Fixtures exist before the veil: a sandbox tree and a file that
    // must disappear once the veil drops.
    if (mkdir("/pledge-sandbox", 0777) && errno != EEXIST) return -1;
    int fd = open("/pledge-sandbox/box.txt", O_WRONLY | O_CREAT | O_TRUNC,
                  0666);
    if (fd < 0) return -1;
    if (write(fd, "sandbox", 8) != 8) { close(fd); return -1; }
    close(fd);
    fd = open("/pledge-out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    close(fd);

    // The veil: the sandbox opens for reading, writing, and creation,
    // /boot for reading and executing, then the table locks.
    if (unveil("/pledge-sandbox", "rwc")) return -1;
    if (unveil("/boot", "rx")) return -1;
    if (unveil(0, 0)) return -1;
    errno = 0;
    if (unveil("/pledge-sandbox", "r") != -1 || errno != EPERM) return -1;

    // Promises with the error promise first: denials stay quiet.
    if (pledge("stdio rpath wpath cpath exec proc error", 0)) return -1;
    if (pledge("stdio rpath cpath exec proc error", 0)) return -1;
    errno = 0;
    if (pledge("stdio rpath wpath", 0) != -1 || errno != EPERM) return -1;

    // The veil answers before anything else: an existing file outside
    // every rule is ENOENT, a rule without the permission asked for is
    // EACCES.
    errno = 0;
    if (stat("/pledge-out.txt", &st) != -1 || errno != ENOENT) return -1;
    if (stat("/pledge-sandbox/box.txt", &st)) return -1;
    errno = 0;
    if (execve("/pledge-sandbox/box.txt", 0, 0) != -1 || errno != EACCES)
        return -1;

    // The error promise turns a promise denial into a quiet ENOSYS.
    errno = 0;
    if (chmod("/boot/posixdemo", 0644) != -1 || errno != ENOSYS) return -1;

    // Inode-sticky directories: the rule remembers the directory it was
    // written on, so a removed and re-created directory of the same
    // name falls out of the veil even though "cpath" allows the very
    // operations that swap it.
    if (unlink("/pledge-sandbox/box.txt")) return -1;
    if (rmdir("/pledge-sandbox")) return -1;
    errno = 0;
    if (mkdir("/pledge-sandbox", 0777) != -1 || errno != ENOENT) return -1;
    errno = 0;
    if (stat("/pledge-sandbox", &st) != -1 || errno != ENOENT) return -1;

    // Narrowing the error promise away makes the next denial loud: a
    // caught SIGABRT rides the delivery path block D built.
    if (pledge("stdio rpath cpath exec proc", 0)) return -1;
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_handler = note_pledge_abrt;
    action.sa_flags = 0;
    action.sa_restorer = 0;
    if (sigaction(SIGABRT, &action, 0)) return -1;
    pledge_note = 0;
    errno = 0;
    if (chmod("/boot/posixdemo", 0644) != -1 || errno != ENOSYS) return -1;
    if (pledge_note != SIGABRT) return -1;

    // Exec promises: the child inherits the sandbox and the pending
    // replacement set, and after its execve runs under "stdio" alone.
    // The forbidden fork kills it with the SIGABRT termsig the parent
    // reads through waitpid.
    if (pledge(0, "stdio")) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *const sandbox_argv[] = { "posixdemo", "sandbox", 0 };
        char *const sandbox_envp[] = { "POSIXDEMO=sandbox", 0 };
        execve("/boot/posixdemo", sandbox_argv, sandbox_envp);
        _exit(79);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) return -1;
    return 0;
}

static volatile sig_atomic_t pipe_note;

static void note_pipe_signal(int signo) {
    pipe_note = signo;
}

static int pipe_demo(void) {
    static unsigned char block[4096];
    static unsigned char mirror[4096];
    for (unsigned int index = 0; index < sizeof(block); index++)
        block[index] = (unsigned char)(index * 7u + 3u);
    int descriptors[2];

    // A same process round trip, then a write that wraps the ring tail:
    // 3600 bytes leave the tail there and the next 1024 span the end.
    if (pipe(descriptors)) return -1;
    if (write(descriptors[1], block, 3600) != 3600) return -1;
    if (read(descriptors[0], mirror, 3600) != 3600) return -1;
    for (unsigned int index = 0; index < 3600u; index++)
        if (mirror[index] != block[index]) return -1;
    if (write(descriptors[1], block, 1024) != 1024) return -1;
    if (read(descriptors[0], mirror, 1024) != 1024) return -1;
    for (unsigned int index = 0; index < 1024u; index++)
        if (mirror[index] != block[index]) return -1;
    if (close(descriptors[0]) || close(descriptors[1])) return -1;

    // Across fork the descriptors share the ends, so the child's write
    // wakes the parent parked on the empty ring.
    if (pipe(descriptors)) return -1;
    pid_t child = fork();
    if (child < 0) return -1;
    if (!child) {
        if (write(descriptors[1], block, 96) != 96) _exit(80);
        _exit(0);
    }
    if (read(descriptors[0], mirror, 96) != 96) return -1;
    for (unsigned int index = 0; index < 96u; index++)
        if (mirror[index] != block[index]) return -1;
    int status = 0;
    if (waitpid(child, &status, 0) != child || status != 0) return -1;
    // End of file: the reaped child held the other write end, so the
    // parent's close is the last one and the drained ring answers 0.
    if (close(descriptors[1])) return -1;
    if (read(descriptors[0], mirror, 96) != 0) return -1;
    if (close(descriptors[0])) return -1;

    // A write with no read end left answers EPIPE and raises SIGPIPE;
    // catching it turns the death into a note.
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_handler = note_pipe_signal;
    action.sa_flags = 0;
    action.sa_restorer = 0;
    if (sigaction(SIGPIPE, &action, 0)) return -1;
    if (pipe(descriptors)) return -1;
    if (close(descriptors[0])) return -1;
    pipe_note = 0;
    errno = 0;
    if (write(descriptors[1], block, 64) != -1 || errno != EPIPE)
        return -1;
    if (pipe_note != SIGPIPE) return -1;
    if (close(descriptors[1])) return -1;

    // A signal breaks the parked read with EINTR; the resumed context
    // is the one the handler returns to.
    if (sigaction(SIGUSR1, &action, 0)) return -1;
    if (pipe(descriptors)) return -1;
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        sigemptyset(&action.sa_mask);
        action.sa_handler = note_pipe_signal;
        action.sa_flags = 0;
        action.sa_restorer = 0;
        if (sigaction(SIGUSR1, &action, 0)) _exit(81);
        pipe_note = 0;
        errno = 0;
        if (read(descriptors[0], mirror, 128) != -1 || errno != EINTR)
            _exit(82);
        if (pipe_note != SIGUSR1) _exit(82);
        _exit(65);
    }
    struct timespec settle;
    settle.tv_sec = 0;
    settle.tv_nsec = 100 * 1000 * 1000;
    struct timespec left;
    nanosleep(&settle, &left);
    if (kill(child, SIGUSR1)) return -1;
    if (waitpid(child, &status, 0) != child) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 65) return -1;
    if (close(descriptors[0]) || close(descriptors[1])) return -1;

    // Nine 512 byte writes overflow the 4096 ring, so the child parks on
    // the last one and the parent's drain completes it.
    if (pipe(descriptors)) return -1;
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        for (unsigned int round = 0; round < 9; round++)
            if (write(descriptors[1], block, 512) != 512) _exit(83);
        _exit(0);
    }
    nanosleep(&settle, &left);
    unsigned int drained = 0;
    while (drained < 9u * 512u) {
        long got = read(descriptors[0], mirror, 512);
        if (got <= 0) return -1;
        for (unsigned int index = 0; index < (unsigned int)got; index++)
            if (mirror[index] != block[(drained + index) % 512u])
                return -1;
        drained += (unsigned int)got;
    }
    if (waitpid(child, &status, 0) != child || status != 0) return -1;
    if (close(descriptors[0]) || close(descriptors[1])) return -1;
    return 0;
}

static int poll_demo(void) {
    static unsigned char block[128];
    static unsigned char mirror[128];
    for (unsigned int index = 0; index < sizeof(block); index++)
        block[index] = (unsigned char)(index * 13u + 7u);
    struct timespec settle;
    settle.tv_sec = 0;
    settle.tv_nsec = 50 * 1000 * 1000;
    int descriptors[2];
    struct pollfd list[2];
    int status = 0;

    // A null list is the POSIX sleep spelling, and the timer under it is
    // the one the park uses: the elapsed reading has to reach the request.
    struct timespec before;
    struct timespec after;
    if (clock_gettime(CLOCK_MONOTONIC, &before)) return -1;
    if (poll(0, 0, 30) != 0) return -1;
    if (clock_gettime(CLOCK_MONOTONIC, &after)) return -1;
    long long elapsed = (after.tv_sec - before.tv_sec) * 1000 +
        (after.tv_nsec - before.tv_nsec) / 1000000;
    if (elapsed < 20) return -1;
    if (poll(0, 0, 0) != 0) return -1;

    // An empty pipe with a live writer answers nothing, and the zero
    // timeout must return without parking for it.
    if (pipe(descriptors)) return -1;
    list[0].fd = descriptors[0];
    list[0].events = POLLIN;
    list[0].revents = 0;
    if (poll(list, 1, 0) != 0 || list[0].revents != 0) return -1;
    // The writer is a child parked on a sleep, so the parent is already
    // parked in poll when the bytes land: the answer has to come from the
    // pipe wake path, not from the readiness scan the call starts with.
    pid_t child = fork();
    if (child < 0) return -1;
    if (!child) {
        nanosleep(&settle, 0);
        if (write(descriptors[1], block, 64) != 64) _exit(70);
        _exit(0);
    }
    if (poll(list, 1, 1000) != 1 || list[0].revents != POLLIN) return -1;
    if (read(descriptors[0], mirror, 64) != 64) return -1;
    for (unsigned int index = 0; index < 64u; index++)
        if (mirror[index] != block[index]) return -1;
    if (waitpid(child, &status, 0) != child || status != 0) return -1;

    // The same descriptor reports the timeout as zero ready, the last
    // writer leaving as a hangup, and its own close as NVAL.
    if (poll(list, 1, 100) != 0 || list[0].revents != 0) return -1;
    if (close(descriptors[1])) return -1;
    if (poll(list, 1, 100) != 1 || list[0].revents != POLLHUP) return -1;
    if (close(descriptors[0])) return -1;
    if (poll(list, 1, 0) != 1 || list[0].revents != POLLNVAL) return -1;
    // A negative descriptor is the ignore slot, not an answer.
    list[0].fd = -1;
    list[0].events = POLLIN;
    list[0].revents = 0;
    if (poll(list, 1, 0) != 0 || list[0].revents != 0) return -1;

    // select rides poll underneath: one readable pipe, and the sets come
    // back scoped to the descriptors that were asked about.
    if (pipe(descriptors)) return -1;
    if (write(descriptors[1], block, 32) != 32) return -1;
    fd_set reads;
    fd_set writes;
    FD_ZERO(&reads);
    FD_ZERO(&writes);
    FD_SET(descriptors[0], &reads);
    struct timeval none;
    none.tv_sec = 0;
    none.tv_usec = 0;
    if (select(descriptors[0] + 1, &reads, &writes, 0, &none) != 1) return -1;
    if (!FD_ISSET(descriptors[0], &reads)) return -1;
    if (FD_ISSET(descriptors[1], &reads) || FD_ISSET(descriptors[0], &writes))
        return -1;
    if (read(descriptors[0], mirror, 32) != 32) return -1;
    if (close(descriptors[0]) || close(descriptors[1])) return -1;

    // A datagram socket: an idle one answers nothing on the read side
    // while a bound peer is always ready to write, the queued datagram is
    // what makes it readable, and the park wakes on the child's send.
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver < 0 || sender < 0) return -1;
    struct sockaddr_in receiver_address;
    struct sockaddr_in sender_address;
    fill_loopback_address(&receiver_address, 31060);
    fill_loopback_address(&sender_address, 31061);
    if (bind(receiver, (struct sockaddr *)&receiver_address,
             sizeof(receiver_address))) return -1;
    if (bind(sender, (struct sockaddr *)&sender_address,
             sizeof(sender_address))) return -1;
    list[0].fd = receiver;
    list[0].events = POLLIN;
    list[0].revents = 0;
    list[1].fd = sender;
    list[1].events = POLLOUT;
    list[1].revents = 0;
    if (poll(list, 2, 0) != 1 || list[0].revents != 0 ||
        list[1].revents != POLLOUT) return -1;
    child = fork();
    if (child < 0) return -1;
    if (!child) {
        nanosleep(&settle, 0);
        if (sendto(sender, block, 48, 0,
                   (struct sockaddr *)&receiver_address,
                   sizeof(receiver_address)) != 48) _exit(71);
        _exit(0);
    }
    list[0].revents = 0;
    list[1].revents = 0;
    // Only the receiver is on the list here: the sender is ready to write
    // for its whole life, and a poll that watches it answers at once
    // instead of parking for the datagram.
    if (poll(list, 1, 1000) != 1 || list[0].revents != POLLIN) return -1;
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    if (recvfrom(receiver, mirror, sizeof(mirror), 0,
                 (struct sockaddr *)&source, &source_length) != 48)
        return -1;
    for (unsigned int index = 0; index < 48u; index++)
        if (mirror[index] != block[index]) return -1;
    if (waitpid(child, &status, 0) != child || status != 0) return -1;
    list[0].revents = 0;
    if (poll(list, 1, 0) != 0 || list[0].revents != 0) return -1;
    if (close(receiver) || close(sender)) return -1;
    return 0;
}

static int child_role(void) {
    static const char payload[] = "posixdemo-child-payload";
    char buffer[32];
    char cwd[32];
    if (wait_for_go(0)) return 70;
    if (!getcwd(cwd, sizeof(cwd)) || !string_equals(cwd, "/")) return 71;
    if (getpid() == getppid()) return 72;
    if (lseek(1, 0, 0) != 0 ||
        read(1, buffer, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1))
        return 73;
    buffer[sizeof(payload) - 1] = 0;
    if (!string_equals(buffer, payload)) return 74;
    errno = 0;
    if (fcntl(2, F_GETFD) != -1 || errno != EBADF) return 75;
    return 42;
}

// The console is a devfs node over the serial line: opening it yields the
// descriptor the termios family answers on, a write to it leaves through
// the line discipline's output map, and a request the tty does not carry
// is refused rather than answered with a zero.
static int tty_demo(void) {
    static const char marker[] = "Mich x86_64: POSIX tty console pass\n";
    int console = open("/dev/console", O_RDWR);
    if (console < 0) return -1;
    struct termios settings;
    if (tcgetattr(console, &settings)) return -1;
    if (!(settings.c_lflag & ICANON) || !(settings.c_lflag & ECHO) ||
        !(settings.c_lflag & ISIG))
        return -1;
    if (!(settings.c_iflag & ICRNL) || !(settings.c_oflag & ONLCR))
        return -1;
    if (settings.c_cc[VINTR] != 3 || settings.c_cc[VEOF] != 4 ||
        settings.c_cc[VERASE] != 0x7F)
        return -1;
    // Clearing echo and reading it back proves the change landed on the
    // one line the console holds; the original comes back afterwards.
    struct termios quiet = settings;
    quiet.c_lflag &= ~(tcflag_t)ECHO;
    if (tcsetattr(console, TCSANOW, &quiet)) return -1;
    struct termios probe;
    if (tcgetattr(console, &probe)) return -1;
    if (probe.c_lflag & ECHO) return -1;
    if (tcsetattr(console, TCSANOW, &settings)) return -1;
    if (tcgetattr(console, &probe)) return -1;
    if (!(probe.c_lflag & ECHO)) return -1;

    struct winsize size;
    if (ioctl(console, TIOCGWINSZ, &size)) return -1;
    if (size.ws_row != 25 || size.ws_col != 80) return -1;
    size.ws_row = 40;
    size.ws_col = 100;
    if (ioctl(console, TIOCSWINSZ, &size)) return -1;
    if (ioctl(console, TIOCGWINSZ, &size)) return -1;
    if (size.ws_row != 40 || size.ws_col != 100) return -1;

    // Nothing has been typed into this console, so a read parks rather
    // than answering zero bytes. The readiness row says the same thing,
    // and the child below proves the park by still running after the
    // pause, which a read that answered would not be.
    struct pollfd watch;
    watch.fd = console;
    watch.events = POLLIN | POLLOUT;
    watch.revents = 0;
    if (poll(&watch, 1, 0) != 1) return -1;
    if (!(watch.revents & POLLOUT) || (watch.revents & POLLIN)) return -1;

    char typed[8];
    int typed_child = fork();
    if (typed_child < 0) return -1;
    if (typed_child == 0) {
        // A parked read only comes back on input, a signal, or death, and
        // this profile has no way to type into the console yet.
        if (read(console, typed, sizeof(typed)) < 0) _exit(41);
        _exit(42);
    }
    struct timespec typed_pause;
    typed_pause.tv_sec = 0;
    typed_pause.tv_nsec = 50000000;
    if (nanosleep(&typed_pause, 0)) return -1;
    int typed_status = 0;
    if (waitpid(typed_child, &typed_status, WNOHANG) != 0) return -1;
    if (kill(typed_child, SIGKILL)) return -1;
    if (waitpid(typed_child, &typed_status, 0) != typed_child) return -1;
    if (!WIFSIGNALED(typed_status) || WTERMSIG(typed_status) != SIGKILL)
        return -1;

    // The foreground rules. The demo's own group takes the line, so a
    // child that put itself in a group of its own is background: its read
    // stops it with SIGTTIN, an ignored SIGTTIN fails the read with EIO
    // rather than parking forever, and a write stops it with SIGTTOU once
    // the line carries TOSTOP.
    if (setpgid(0, 0)) return -1;
    pid_t demo_group = getpgrp();
    if (demo_group <= 0) return -1;
    if (tcsetpgrp(console, demo_group)) return -1;
    if (tcgetpgrp(console) != demo_group) return -1;

    int background = fork();
    if (background < 0) return -1;
    if (background == 0) {
        if (setpgid(0, 0)) _exit(51);
        char byte[1];
        errno = 0;
        if (read(console, byte, sizeof(byte)) != -1 || errno != EINTR)
            _exit(52);
        _exit(53);
    }
    int background_status = 0;
    if (waitpid(background, &background_status, WUNTRACED) != background)
        return -1;
    if (!WIFSTOPPED(background_status) ||
        WSTOPSIG(background_status) != SIGTTIN)
        return -1;
    if (kill(background, SIGCONT)) return -1;
    if (waitpid(background, &background_status, 0) != background) return -1;
    if (!WIFEXITED(background_status) ||
        WEXITSTATUS(background_status) != 53)
        return -1;

    int muting = fork();
    if (muting < 0) return -1;
    if (muting == 0) {
        if (setpgid(0, 0)) _exit(54);
        struct sigaction ignore;
        sigemptyset(&ignore.sa_mask);
        ignore.sa_handler = SIG_IGN;
        ignore.sa_flags = 0;
        ignore.sa_restorer = 0;
        if (sigaction(SIGTTIN, &ignore, 0)) _exit(55);
        char byte[1];
        errno = 0;
        if (read(console, byte, sizeof(byte)) != -1 || errno != EIO)
            _exit(56);
        _exit(57);
    }
    if (waitpid(muting, &background_status, 0) != muting) return -1;
    if (!WIFEXITED(background_status) ||
        WEXITSTATUS(background_status) != 57)
        return -1;

    struct termios loud;
    if (tcgetattr(console, &loud)) return -1;
    loud.c_lflag |= (tcflag_t)TOSTOP;
    if (tcsetattr(console, TCSANOW, &loud)) return -1;
    int writer = fork();
    if (writer < 0) return -1;
    if (writer == 0) {
        if (setpgid(0, 0)) _exit(58);
        // A stopped write resumes with zero bytes moved, so the child
        // checks for exactly that: the gate ran and nothing left.
        if (write(console, "z", 1) != 0) _exit(59);
        _exit(60);
    }
    if (waitpid(writer, &background_status, WUNTRACED) != writer) return -1;
    if (!WIFSTOPPED(background_status) ||
        WSTOPSIG(background_status) != SIGTTOU)
        return -1;
    if (kill(writer, SIGCONT)) return -1;
    if (waitpid(writer, &background_status, 0) != writer) return -1;
    if (!WIFEXITED(background_status) ||
        WEXITSTATUS(background_status) != 60)
        return -1;
    loud.c_lflag &= ~(tcflag_t)TOSTOP;
    if (tcsetattr(console, TCSANOW, &loud)) return -1;

    // /dev/null: reads answer end of file and writes are swallowed.
    int null = open("/dev/null", O_RDWR);
    if (null < 0) return -1;
    if (read(null, typed, sizeof(typed)) != 0) return -1;
    if (write(null, "gone", 4) != 4) return -1;
    if (close(null)) return -1;

    // A descriptor that is not a tty answers ENOTTY, and so does a request
    // the tty does not carry.
    int descriptors[2];
    if (pipe(descriptors)) return -1;
    errno = 0;
    if (ioctl(descriptors[0], TCGETS, &settings) != -1 || errno != ENOTTY)
        return -1;
    errno = 0;
    if (ioctl(console, 0x9999, &settings) != -1 || errno != ENOTTY) return -1;
    if (close(descriptors[0]) || close(descriptors[1])) return -1;

    // The marker itself is the write path: it leaves through the line
    // discipline and reaches the serial console the runner reads.
    if (write(console, marker, sizeof(marker) - 1) !=
        (ssize_t)(sizeof(marker) - 1))
        return -1;
    if (close(console)) return -1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && string_equals(argv[1], "sandbox")) {
        if (!environ || !string_equals(environ[0], "POSIXDEMO=sandbox") ||
            environ[1])
            return 78;
        return sandbox_role();
    }
    if (argc == 2 && string_equals(argv[1], "child")) {
        if (!environ || !string_equals(environ[0], "POSIXDEMO=child") ||
            environ[1])
            return 76;
        return child_role();
    }
    if (argc != 2 || !string_equals(argv[0], "/boot/posixdemo") ||
        !string_equals(argv[1], "demo") || !environ ||
        !string_equals(environ[0], "POSIXDEMO=stage6") || environ[1])
        return 80;
    char cwd[32];
    if (!getcwd(cwd, sizeof(cwd)) || !string_equals(cwd, "/")) return 81;
    mich_write("Mich x86_64: POSIX application alive\n");
    if (io_demo()) return 82;
    mich_write("Mich x86_64: POSIX application IO pass\n");
    if (cwd_demo()) return 83;
    mich_write("Mich x86_64: POSIX application cwd pass\n");
    if (errno_demo()) return 84;
    mich_write("Mich x86_64: POSIX application errno pass\n");
    if (string_demo()) return 86;
    mich_write("Mich x86_64: POSIX libc string pass\n");
    if (stdio_demo()) return 87;
    mich_write("Mich x86_64: POSIX libc stdio pass\n");
    if (heap_demo()) return 88;
    mich_write("Mich x86_64: POSIX libc heap pass\n");
    if (file_demo()) return 89;
    mich_write("Mich x86_64: POSIX libc file pass\n");
    if (entropy_demo()) return 90;
    mich_write("Mich x86_64: POSIX entropy pass\n");
    if (link_demo()) return 91;
    mich_write("Mich x86_64: POSIX application link pass\n");
    if (rename_demo()) return 92;
    mich_write("Mich x86_64: POSIX application rename pass\n");
    if (symlink_demo()) return 93;
    mich_write("Mich x86_64: POSIX application symlink pass\n");
    if (times_demo()) return 94;
    mich_write("Mich x86_64: POSIX application times pass\n");
    if (realpath_demo()) return 95;
    mich_write("Mich x86_64: POSIX libc realpath pass\n");
    if (time_demo()) return 96;
    mich_write("Mich x86_64: POSIX clocks pass\n");
    if (signal_demo()) return 97;
    mich_write("Mich x86_64: POSIX signals pass\n");
    if (socket_demo()) return 66;
    mich_write("Mich x86_64: POSIX sockets pass\n");
    if (poll_demo()) return 100;
    mich_write("Mich x86_64: POSIX poll pass\n");
    if (tty_demo()) return 101;
    if (process_demo()) return 85;
    mich_write("Mich x86_64: POSIX application process pass\n");
    if (pledge_demo()) return 98;
    mich_write("Mich x86_64: POSIX pledge and unveil pass\n");
    if (pipe_demo()) return 99;
    mich_write("Mich x86_64: POSIX pipes pass\n");
    return 0;
}

