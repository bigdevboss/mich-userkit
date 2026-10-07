#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <mich/syscall.h>

// Step-5 process fixture: init64 forks, execve's this image with a fixed
// contract - fd 0 sync flag, fd 1 payload, fd 2 dropped by CLOEXEC, cwd
// /posix-proc - and reaps exit code 7 through waitpid.

static int string_equals(const char *left, const char *right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static int wait_for_go(void) {
    for (unsigned long attempt = 0; attempt < 100000u; attempt++) {
        char flag = 0;
        // A seek plus a read would move the cursor the parent is about to
        // write through, so the poll reads its byte at a fixed offset.
        if (pread(0, &flag, 1, 0) != 1) return -1;
        if (flag == 'g') return 0;
        mich_yield();
    }
    // A silent -1 here reads as a mystery hang from the outside.
    mich_write("Mich x86_64: POSIX execve fixture flag timeout\n");
    return -1;
}

int main(int argc, char **argv) {
    if (wait_for_go()) return 60;
    if (argc != 3 || !string_equals(argv[0], "/boot/posixapp") ||
        !string_equals(argv[1], "alpha") || !string_equals(argv[2], "beta"))
        return 61;
    if (!environ || !string_equals(environ[0], "POSIXAPP=stage5") ||
        environ[1])
        return 62;

    char cwd[32];
    if (!getcwd(cwd, sizeof(cwd)) || !string_equals(cwd, "/posix-proc"))
        return 63;

    if (getpid() != mich_getpid() || getpid() == 1 || getppid() != 1)
        return 64;

    static const char payload[] = "mich-posix-stage5";
    char buffer[32];
    if (pread(1, buffer, sizeof(payload) - 1, 0) !=
        (ssize_t)(sizeof(payload) - 1))
        return 65;
    buffer[sizeof(payload) - 1] = 0;
    if (!string_equals(buffer, payload)) return 66;

    errno = 0;
    if (fcntl(2, F_GETFD) != -1 || errno != EBADF) return 67;

    mich_write("Mich x86_64: POSIX execve fixture alive\n");
    return 7;
}
