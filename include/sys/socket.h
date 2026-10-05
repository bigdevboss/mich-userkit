#ifndef MICH64_SYS_SOCKET_H
#define MICH64_SYS_SOCKET_H

#include <stddef.h>
#include <sys/types.h>

// The v0 socket surface over the kernel net stack: AF_INET datagram and
// stream sockets through the staged request ABI. The family and type
// numbers are the POSIX ones the kernel expects, and every address is
// exactly the sixteen byte sockaddr_in the profile carries.

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

#define AF_INET 2u
#define SOCK_STREAM 1u
#define SOCK_DGRAM 2u

#define SOL_SOCKET 1u
#define SO_REUSEADDR 2u
#define SO_TYPE 3u
#define SO_ERROR 4u
#define SO_PROTOCOL 38u
#define SO_DOMAIN 39u

#define SHUT_RD 0u
#define SHUT_WR 1u
#define SHUT_RDWR 2u

struct sockaddr {
    sa_family_t sa_family;
    unsigned char sa_data[14];
};

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    int msg_iovlen;
    void *msg_control;
    socklen_t msg_controllen;
    int msg_flags;
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *address, socklen_t *length);
int connect(int fd, const struct sockaddr *address, socklen_t length);
ssize_t send(int fd, const void *buffer, size_t length, int flags);
ssize_t recv(int fd, void *buffer, size_t length, int flags);
ssize_t sendto(int fd, const void *buffer, size_t length, int flags,
               const struct sockaddr *destination, socklen_t destlen);
ssize_t recvfrom(int fd, void *buffer, size_t length, int flags,
                 struct sockaddr *source, socklen_t *sourcelen);
ssize_t sendmsg(int fd, const struct msghdr *message, int flags);
ssize_t recvmsg(int fd, struct msghdr *message, int flags);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int name, void *value, socklen_t *length);
int setsockopt(int fd, int level, int name, const void *value,
               socklen_t length);
int getsockname(int fd, struct sockaddr *address, socklen_t *length);
int getpeername(int fd, struct sockaddr *address, socklen_t *length);

#endif
