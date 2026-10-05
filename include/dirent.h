#ifndef MICH64_DIRENT_H
#define MICH64_DIRENT_H

#define DT_REG 1u
#define DT_DIR 2u

// A getdents record as the kernel packs it: d_ino, d_off, d_reclen, d_type,
// then the NUL-terminated name padded to an 8-byte boundary. Walk a result
// buffer by advancing d_reclen.
struct dirent {
    unsigned long long d_ino;
    unsigned long long d_off;
    unsigned int d_reclen;
    unsigned int d_type;
    char d_name[256];
};

int getdents(int fd, struct dirent *buffer, unsigned int length);

#endif
