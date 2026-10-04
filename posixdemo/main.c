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
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <mich/syscall.h>

/* First compatibility-claim fixture (profile step 6): a real static POSIX
   application launched by init64 through fork/execve. The parent role
   exercises the v0 core API surface; the child role is reached by the
   application exec'ing itself with its own argv/envp. Contract: entry table
   empty, cwd "/", parent passes {"posixdemo","demo"}, {"POSIXDEMO=stage6"}. */

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
    /* dup shares the open-file description, so the alias continues at the
       shared offset instead of restarting the file. */
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
    /* access mirrors the permission picture the descriptors already
       proved: 0600 opens read and write for the owner and keeps the
       execute question closed. */
    if (access("/posixdemo-io", R_OK | W_OK)) return -1;
    errno = 0;
    if (access("/posixdemo-io", X_OK) != -1 || errno != EACCES) return -1;
    errno = 0;
    if (access("/posixdemo-missing", F_OK) != -1 || errno != ENOENT)
        return -1;



    /* The positioned calls address an offset without touching the shared
       cursor, and a truncate extension reads back as zeroes. */
    if (pwrite(fd, "ZZ", 2, 8) != 2) return -1;
    if (lseek(alias, 0, 1) != (off_t)(sizeof(payload) - 1)) return -1;
    if (pread(fd, first, 2, 8) != 2 || first[0] != 'Z' || first[1] != 'Z')
        return -1;
    if (lseek(alias, 0, 1) != (off_t)(sizeof(payload) - 1)) return -1;
    if (ftruncate(fd, 32)) return -1;
    if (fstat(fd, &info) || info.st_size != 32) return -1;
    if (pread(fd, first, 1, 31) != 1 || first[0]) return -1;
    if (ftruncate(fd, (off_t)(sizeof(payload) - 1))) return -1;
    /* A durability call rides the format staging path and has to leave
       the file readable through the plain cursor afterwards. */
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
    /* The surviving name still reads the bytes both names shared. */
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
    /* Moving the name across directories keeps the bytes and drops the
       old name; renaming a name onto itself changes nothing. */
    if (rename("/posixdemo-rename", "/posixdemo-move/landed")) return -1;
    if (rename("/posixdemo-move/landed", "/posixdemo-move/landed")) return -1;
    errno = 0;
    if (stat("/posixdemo-rename", &info) != -1 || errno != ENOENT)
        return -1;
    if (stat("/posixdemo-move/landed", &info) ||
        info.st_nlink != 1 || info.st_size != (off_t)(sizeof(payload) - 1))
        return -1;
    /* A same-kind target leaves quietly and the moved name takes its
       place. */
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
    /* The replace matrix and the cross-filesystem refusal carry their
       own errno values. */
    errno = 0;
    if (rename("/posixdemo-victim", "/posixdemo-move") != -1 ||
        errno != EISDIR)
        return -1;
    errno = 0;
    if (rename("/posixdemo-move", "/posixdemo-victim") != -1 ||
        errno != ENOTDIR)
        return -1;
    /* ENOTEMPTY belongs to the replaced target: a full source directory
       onto an empty one is a legal move. */
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
    /* stat follows the link, lstat does not, and readlink carries the
       target string without a terminator. */
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
    /* Opening the link reaches the file behind it. */
    fd = open("/posixdemo-link", O_RDONLY);
    if (fd < 0) return -1;
    if (read(fd, buffer, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    buffer[sizeof(payload) - 1] = 0;
    if (close(fd)) return -1;
    if (!string_equals(buffer, payload)) return -1;
    /* A dangling link reads back but resolves to nothing, and two links
       pointing at each other never resolve at all. */
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
    /* Chosen pairs land whole: seconds and nanoseconds both survive the
       round trip through the path call. */
    chosen[0].tv_sec = 1000000000;
    chosen[0].tv_nsec = 123456789;
    chosen[1].tv_sec = 1000000001;
    chosen[1].tv_nsec = 987654321;
    if (utimensat(AT_FDCWD, "/posixdemo-times", chosen, 0)) return -1;
    if (stat("/posixdemo-times", &info) ||
        info.st_atime != 1000000000 || info.st_atime_nsec != 123456789 ||
        info.st_mtime != 1000000001 || info.st_mtime_nsec != 987654321)
        return -1;
    /* OMIT leaves one half standing while the other moves. */
    chosen[0].tv_nsec = UTIME_OMIT;
    chosen[1].tv_sec = 2000000000;
    chosen[1].tv_nsec = 1;
    if (utimensat(AT_FDCWD, "/posixdemo-times", chosen, 0)) return -1;
    if (stat("/posixdemo-times", &info) ||
        info.st_atime != 1000000000 || info.st_atime_nsec != 123456789 ||
        info.st_mtime != 2000000000 || info.st_mtime_nsec != 1)
        return -1;
    /* A NULL pair is the classic utime request, and the descriptor twin
       carries it without a path at all. The wall clock answers with real
       seconds, so NOW lands anywhere at or past the last chosen pair. */
    if (futimens(fd, 0)) return -1;
    if (fstat(fd, &info) || info.st_mtime < 1000000001 ||
        info.st_mtime_nsec > 999999999)
        return -1;
    /* A nanosecond half outside [0, 999999999] is EINVAL, and so is a
       dirfd or flag the wrapper does not carry. */
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
    /* Repeated slashes, "." and ".." fold into the canonical form, and the
       root is its own parent. */
    if (!realpath("/posixdemo-canonical//./../posixdemo-canonical/./target.txt",
                  path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (!realpath("/posixdemo-canonical/..", path) ||
        !string_equals(path, "/"))
        return -1;
    /* A relative path starts from the working directory. */
    if (chdir("/posixdemo-canonical")) return -1;
    if (!realpath("target.txt", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (!realpath("../posixdemo-canonical/target.txt", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (chdir("/")) return -1;
    /* Links expand: an absolute target restarts the walk at the root, a
       relative one continues from the link's directory. */
    if (symlink("/posixdemo-canonical/target.txt", "/posixdemo-canonical/absolute"))
        return -1;
    if (!realpath("/posixdemo-canonical/absolute", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    if (symlink("target.txt", "/posixdemo-canonical/relative")) return -1;
    if (!realpath("/posixdemo-canonical/relative", path) ||
        !string_equals(path, "/posixdemo-canonical/target.txt"))
        return -1;
    /* A NULL buffer makes the call allocate the answer itself. */
    char *heap = realpath("/posixdemo-canonical/target.txt", 0);
    if (!heap || !string_equals(heap, "/posixdemo-canonical/target.txt"))
        return -1;
    free(heap);
    /* The error surface: a missing component, a non-directory in the
       middle, a link loop, an empty path, and a result that cannot fit. */
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
    /* Directories open read-only so they can be listed; drain the root and
       expect exactly the boot and dev directories the system mounts. */
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
    /* Ownership and mode edits: chmod locks and unlocks a file, chown hands
       it to another owner and takes it back, and the umask shapes a create. */
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
    /* Overlapping ranges: memmove must behave as if copied through a
       temporary buffer, unlike a plain forward memcpy. */
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
    /* POSIX truncation contract: the full length is returned while the
       buffer stays bounded and nul-terminated. */
    if (snprintf(buffer, 4, "%s", "abcdefg") != 7) return -1;
    if (strcmp(buffer, "abc")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%lu%%", 1000UL) != 5) return -1;
    if (strcmp(buffer, "1000%")) return -1;
    /* printf itself must stream through the bounded serial flush path. */
    if (printf("libc printf alive: %d %s 0x%x\n", 42, "mich", 48879) != 34)
        return -1;
    return 0;
}

static int heap_demo(void) {
    unsigned long base =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (!base) return -1;
    /* Neighbor coalescing while the arena is still empty: two freed
       adjacent blocks must serve one larger request without growth. */
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
    /* A large request crosses page boundaries through the break syscall. */
    unsigned char *data = malloc(70000);
    if (!data) return -1;
    for (int index = 0; index < 70000; index++) data[index] = (unsigned char)index;
    for (int index = 0; index < 70000; index++)
        if (data[index] != (unsigned char)index) return -1;
    unsigned long grown =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (grown <= base) return -1;
    /* Freed space is reused instead of growing the arena again. */
    free(data);
    unsigned char *reused = malloc(70000);
    if (!reused) return -1;
    if ((unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0) != grown) return -1;
    /* A double free must not merge the same block twice. */
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
    /* realloc preserves the old contents across a move. */
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
    /* The heap window is bounded, so an oversized request reports ENOMEM. */
    errno = 0;
    if (malloc(32 * 1024 * 1024)) return -1;
    if (errno != ENOMEM) return -1;
    /* The break is grow-only: requesting the base back changes nothing. */
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
    /* 700 pattern bytes force the 512-byte buffer through a mid-stream
       flush while the file stays under the 4 KiB VFS cap. */
    if (fwrite(block, 1, 256, file) != 256) return -1;
    if (fwrite(block, 1, 256, file) != 256) return -1;
    if (fwrite(block, 1, 188, file) != 188) return -1;
    /* ftell must account for bytes still staged in the buffer. */
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
    /* The sticky EOF flag rises on the short read, not before it. */
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
    /* Writing into a read stream fails and sets the error flag without
       disturbing the pending reads. */
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
    /* A stuck or absent generator must not look like all-zero or all-one
       output, and the bit balance must sit near one half. */
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
    /* /dev/urandom rides the same DRBG through the file facade. */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    unsigned char third[256];
    if (read(fd, third, sizeof(third)) != (ssize_t)sizeof(third)) return -1;
    if (read(fd, first, 64) != 64) return -1;
    if (!memcmp(third, second, sizeof(third))) return -1;
    if (close(fd)) return -1;
    /* Reserved flags and zero length are rejected with EINVAL. */
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
    /* The kernel rounds up to whole ticks, so the slept span is never
       shorter than the requested twenty milliseconds. */
    long slept = (long)(after.tv_sec - before.tv_sec) * 1000000000L +
        (after.tv_nsec - before.tv_nsec);
    if (slept < 20000000L) return -1;
    /* Monotonic never runs backwards across the sleep. */
    if (after.tv_sec < before.tv_sec) return -1;
    struct timespec wall;
    if (clock_gettime(CLOCK_REALTIME, &wall) || wall.tv_sec < 1790240000LL ||
        wall.tv_nsec < 0 || wall.tv_nsec > 999999999L)
        return -1;
    /* time and gettimeofday draw on the same wall clock, so the two
       readings stay inside a one second window. */
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
    /* A zero interval is a scheduling point, not a park. */
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

    /* A caught signal posted to the caller itself is delivered on the
       syscall exit, and the restorer path resumes the interrupted code. */
    sigemptyset(&action.sa_mask);
    action.sa_handler = note_signal;
    action.sa_flags = 0;
    action.sa_restorer = 0;
    if (sigaction(SIGUSR1, &action, &previous)) return -1;
    signal_note = 0;
    if (raise(SIGUSR1)) return -1;
    if (signal_note != SIGUSR1) return -1;
    if (previous.sa_handler != SIG_DFL) return -1;

    /* A blocked signal parks instead of delivering, sigpending shows it,
       and opening the mask delivers on the unblock exit. */
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
    if (signal_note != SIGUSR1 || sigismember(&pending, SIGUSR1))
        return -1;

    /* A user loop with no syscalls still reaches its handler through the
       tick delivery: the child waits the parent into the loop first. */
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

    /* An interrupted nanosleep answers EINTR with the time it still
       owed, rounded to whole ticks. */
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

    /* A hardware fault reaches its handler with the faulting frame: the
       POSIX return repeats the instruction, so the handler leaves through
       _exit rather than resuming into the same fault. */
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
    if (signal_note != SIGSEGV) return -1;

    /* SIGKILL on a parked child surfaces as WIFSIGNALED with the real
       termsig, not a folded exit byte. */
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

    /* An ignored SIGCHLD is the auto-reap contract: the child leaves no
       zombie and waitpid answers ECHILD. */
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

    /* Rejections: an unknown signal, an untouchable one, a non positive
       pid, and a pid no task answers for. */
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
    /* Signal zero is the existence probe and reports success quietly. */
    if (raise(0)) return -1;
    return 0;
}

static int process_demo(void) {
    static const char payload[] = "posixdemo-child-payload";
    char *const child_argv[] = { "/posixdemo", "child", 0 };
    char *const child_envp[] = { "POSIXDEMO=child", 0 };
    int status = -1;
    /* Child contract: 0 sync flag, 1 payload, 2 CLOEXEC victim. */
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
    /* The child parks on the sync flag, so the WNOHANG poll cannot race. */
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

int main(int argc, char **argv) {
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
    if (process_demo()) return 85;
    mich_write("Mich x86_64: POSIX application process pass\n");
    return 0;
}
